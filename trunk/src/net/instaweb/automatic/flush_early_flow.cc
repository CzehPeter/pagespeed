/*
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: mmohabey@google.com (Megha Mohabey)

#include "net/instaweb/automatic/public/flush_early_flow.h"

#include <algorithm>
#include <set>

#include "base/logging.h"
#include "net/instaweb/automatic/public/proxy_fetch.h"
#include "net/instaweb/htmlparse/public/html_keywords.h"
#include "net/instaweb/http/http.pb.h"  // for HttpResponseHeaders
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/meta_data.h"  // for Code::kOK
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/user_agent_matcher.h"
#include "net/instaweb/js/public/js_minify.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/flush_early.pb.h"
#include "net/instaweb/rewriter/public/critical_images_finder.h"
#include "net/instaweb/rewriter/public/flush_early_content_writer_filter.h"
#include "net/instaweb/rewriter/public/flush_early_info_finder.h"
#include "net/instaweb/rewriter/public/lazyload_images_filter.h"
#include "net/instaweb/rewriter/public/js_defer_disabled_filter.h"
#include "net/instaweb/rewriter/public/js_disable_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewritten_content_scanning_filter.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_javascript_manager.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/proto_util.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"  // for Timer

namespace {

const char kPreloadScript[] = "function preload(x){"
    "var obj=document.createElement('object');"
    "obj.data=x;"
    "obj.width=0;"
    "obj.height=0;}";
const char kScriptBlock[] =
    "<script type=\"text/javascript\">(function(){%s})()</script>";

const char kFlushSubresourcesFilter[] = "FlushSubresourcesFilter";

const char kPrefetchObjectTagHtml[] = "preload(%s);";

const int kMaxParallelConnections = 6;

// Minimum fetch latency for sending pre-connect requests.
const int kMinLatencyForPreconnectMs = 100;

}  // namespace

namespace net_instaweb {

namespace {

void InitFlushEarlyDriverWithPropertyCacheValues(
    RewriteDriver* flush_early_driver, PropertyPage* page) {
  // Reading Flush early flow info from Property Page. After reading,
  // property page in new_driver is set to NULL, so that no one writes to
  // property cache while flushing early. Also property page isn't guaranteed to
  // exist in flush_early_driver lifetime.
  flush_early_driver->set_unowned_property_page(page);
  // Populates all fields which are needed from property_page as property_page
  // will be set to NULL afterwards.
  flush_early_driver->flush_early_info();
  FlushEarlyInfoFinder* finder =
      flush_early_driver->server_context()->flush_early_info_finder();
  if (finder != NULL && finder->IsMeaningful()) {
    finder->UpdateFlushEarlyInfoInDriver(flush_early_driver);
  }

  // Populating critical images from css in flush early driver.
  CriticalImagesFinder* critical_images_finder =
      flush_early_driver->server_context()->critical_images_finder();
  if (critical_images_finder->IsMeaningful()) {
    critical_images_finder->UpdateCriticalImagesSetInDriver(flush_early_driver);
  }
  flush_early_driver->set_unowned_property_page(NULL);
}

}  // namespace

class StaticJavascriptManager;

const char FlushEarlyFlow::kNumRequestsFlushedEarly[] =
    "num_requests_flushed_early";
const char FlushEarlyFlow::kNumResourcesFlushedEarly[] =
    "num_resources_flushed_early";
const char FlushEarlyFlow::kFlushEarlyRewriteLatencyMs[] =
    "flush_early_rewrite_latency_ms";
const char FlushEarlyFlow::kNumFlushEarlyHttpStatusCodeDeemedUnstable[] =
    "num_flush_early_http_status_code_deemed_unstable";

// TODO(mmohabey): Do not flush early if the html is cacheable.
// If this is called then the content type must be html.
// TODO(nikhilmadan): Disable flush early if the response code isn't
// consistently a 200.

// AsyncFetch that manages the parallelization of FlushEarlyFlow with the
// ProxyFetch flow. Note that this fetch is passed to ProxyFetch as the
// base_fetch.
// While the FlushEarlyFlow is running, it buffers up bytes from the ProxyFetch
// flow, while streaming out bytes from the FlushEarlyFlow flow.
// Once the FlushEarlyFlow is completed, it writes out all the buffered bytes
// from ProxyFetch, after which it starts streaming bytes from ProxyFetch.
class FlushEarlyFlow::FlushEarlyAsyncFetch : public AsyncFetch {
 public:
  FlushEarlyAsyncFetch(AsyncFetch* fetch, AbstractMutex* mutex)
      : base_fetch_(fetch),
        mutex_(mutex),
        flush_early_flow_done_(false),
        flushed_early_(false),
        headers_complete_called_(false),
        flush_called_(false),
        done_called_(false),
        done_value_(false),
        flush_handler_(NULL) {
    set_request_headers(fetch->request_headers());
    set_log_record(fetch->log_record());
  }

  // Indicates that the flush early flow is complete.
  void set_flush_early_flow_done(bool flushed_early) {
    bool should_delete = false;
    {
      ScopedMutex lock(mutex_.get());
      flush_early_flow_done_ = true;
      flushed_early_ = flushed_early;
      if (!flushed_early && headers_complete_called_) {
        base_fetch_->response_headers()->CopyFrom(*response_headers());
      }
      // Write out all the buffered content and call Flush and Done if it were
      // called earlier.
      if (!buffered_content_.empty()) {
        base_fetch_->Write(buffered_content_, NULL);
        buffered_content_.clear();
      }
      if (flush_called_) {
        DCHECK(flush_handler_ != NULL);
        base_fetch_->Flush(flush_handler_);
      }
      if (done_called_) {
        base_fetch_->Done(done_value_);
        should_delete = true;
      }
    }
    if (should_delete) {
      delete this;
    }
  }

 private:
  // If the flush early flow isn't done yet, do nothing here since
  // set_flush_early_flow_done will do the needful.
  // If we flushed early, then the FlushEarlyFlow would have already set
  // the headers. Hence, do nothing.
  // If we didn't flush early, copy the response headers into the base fetch.
  virtual void HandleHeadersComplete() {
    {
      ScopedMutex lock(mutex_.get());
      if (!flush_early_flow_done_ || flushed_early_) {
        headers_complete_called_ = true;
        return;
      }
    }
    base_fetch_->response_headers()->CopyFrom(*response_headers());
  }

  // If the flush early flow is still in progress, buffer the bytes. Otherwise,
  // write them out to base_fetch.
  virtual bool HandleWrite(const StringPiece& sp, MessageHandler* handler) {
    {
      ScopedMutex lock(mutex_.get());
      if (!flush_early_flow_done_) {
        buffered_content_.append(sp.data(), sp.size());
        return true;
      }
    }
    return base_fetch_->Write(sp, handler);
  }

  // If the flush early flow is still in progress, store the fact that flush
  // was called. Otherwise, pass the call to base_fetch.
  virtual bool HandleFlush(MessageHandler* handler) {
    {
      ScopedMutex lock(mutex_.get());
      if (!flush_early_flow_done_) {
        flush_called_ = true;
        flush_handler_ = handler;
        return true;
      }
    }
    return base_fetch_->Flush(handler);
  }

  // If the flush early flow is still in progress, store the fact that done was
  // called. Otherwise, pass the call to base_fetch.
  virtual void HandleDone(bool success) {
    {
      ScopedMutex lock(mutex_.get());
      if (!flush_early_flow_done_) {
        done_called_ = true;
        done_value_ = success;
        return;
      }
    }
    base_fetch_->Done(success);
    delete this;
  }

  AsyncFetch* base_fetch_;
  scoped_ptr<AbstractMutex> mutex_;
  GoogleString buffered_content_;
  bool flush_early_flow_done_;
  bool flushed_early_;
  bool headers_complete_called_;
  bool flush_called_;
  bool done_called_;
  bool done_value_;
  MessageHandler* flush_handler_;

  DISALLOW_COPY_AND_ASSIGN(FlushEarlyAsyncFetch);
};

void FlushEarlyFlow::Start(
    const GoogleString& url,
    AsyncFetch** base_fetch,
    RewriteDriver* driver,
    ProxyFetchFactory* factory,
    ProxyFetchPropertyCallbackCollector* property_cache_callback) {
  FlushEarlyAsyncFetch* flush_early_fetch = new FlushEarlyAsyncFetch(
      *base_fetch, driver->server_context()->thread_system()->NewMutex());
  FlushEarlyFlow* flow = new FlushEarlyFlow(
      url, *base_fetch, flush_early_fetch, driver, factory,
      property_cache_callback);

  // Change the base_fetch in ProxyFetch to flush_early_fetch.
  *base_fetch = flush_early_fetch;
  Function* func = MakeFunction(flow, &FlushEarlyFlow::FlushEarly,
                                &FlushEarlyFlow::Cancel);
  property_cache_callback->AddPostLookupTask(func);
}

void FlushEarlyFlow::InitStats(Statistics* stats) {
  stats->AddTimedVariable(kNumRequestsFlushedEarly,
                          ServerContext::kStatisticsGroup);
  stats->AddTimedVariable(
      FlushEarlyContentWriterFilter::kNumResourcesFlushedEarly,
      ServerContext::kStatisticsGroup);
  stats->AddTimedVariable(kNumFlushEarlyHttpStatusCodeDeemedUnstable,
                          ServerContext::kStatisticsGroup);
  stats->AddHistogram(kFlushEarlyRewriteLatencyMs)->EnableNegativeBuckets();
}

FlushEarlyFlow::FlushEarlyFlow(
    const GoogleString& url,
    AsyncFetch* base_fetch,
    FlushEarlyAsyncFetch* flush_early_fetch,
    RewriteDriver* driver,
    ProxyFetchFactory* factory,
    ProxyFetchPropertyCallbackCollector* property_cache_callback)
    : url_(url),
      dummy_head_writer_(&dummy_head_),
      num_resources_flushed_(0),
      num_rewritten_resources_(0),
      average_fetch_time_(0),
      base_fetch_(base_fetch),
      flush_early_fetch_(flush_early_fetch),
      driver_(driver),
      factory_(factory),
      manager_(driver->server_context()),
      property_cache_callback_(property_cache_callback),
      should_flush_early_lazyload_script_(false),
      should_flush_early_js_defer_script_(false),
      handler_(driver_->server_context()->message_handler()) {
  Statistics* stats = manager_->statistics();
  num_requests_flushed_early_ = stats->GetTimedVariable(
      kNumRequestsFlushedEarly);
  num_resources_flushed_early_ = stats->GetTimedVariable(
      FlushEarlyContentWriterFilter::kNumResourcesFlushedEarly);
  num_flush_early_http_status_code_deemed_unstable_ = stats->GetTimedVariable(
      kNumFlushEarlyHttpStatusCodeDeemedUnstable);
  flush_early_rewrite_latency_ms_ = stats->GetHistogram(
      kFlushEarlyRewriteLatencyMs);
  driver_->increment_async_events_count();
}

FlushEarlyFlow::~FlushEarlyFlow() {
  driver_->decrement_async_events_count();
}

void FlushEarlyFlow::FlushEarly() {
  const RewriteOptions* options = driver_->options();
  const PropertyCache::Cohort* cohort = manager_->page_property_cache()->
      GetCohort(RewriteDriver::kDomCohort);
  PropertyPage* page =
      property_cache_callback_->GetPropertyPageWithoutOwnership(
          ProxyFetchPropertyCallback::kPagePropertyCache);
  if (page != NULL && cohort != NULL) {
    PropertyValue* num_rewritten_resources_property_value = page->GetProperty(
        cohort,
        RewrittenContentScanningFilter::kNumProxiedRewrittenResourcesProperty);

    if (num_rewritten_resources_property_value->has_value()) {
      StringToInt(num_rewritten_resources_property_value->value().data(),
                  &num_rewritten_resources_);
    }
    PropertyValue* status_code_property_value = page->GetProperty(
        cohort, RewriteDriver::kStatusCodePropertyName);

    // We do not trigger flush early flow if the status code of the response is
    // not constant for property_cache_http_status_stability_threshold previous
    // requests.
    bool status_code_property_value_recently_constant =
        !status_code_property_value->has_value() ||
        status_code_property_value->IsRecentlyConstant(
            options->property_cache_http_status_stability_threshold());
    if (!status_code_property_value_recently_constant) {
      num_flush_early_http_status_code_deemed_unstable_->IncBy(1);
    }

    PropertyValue* property_value = page->GetProperty(
        cohort, RewriteDriver::kSubresourcesPropertyName);
    if (property_value != NULL && property_value->has_value()) {
      FlushEarlyInfo flush_early_info;
      ArrayInputStream value(property_value->value().data(),
                             property_value->value().size());
      flush_early_info.ParseFromZeroCopyStream(&value);
      if (!flush_early_info.http_only_cookie_present() &&
          flush_early_info.has_resource_html() &&
          !flush_early_info.resource_html().empty() &&
          flush_early_info.response_headers().status_code() ==
          HttpStatus::kOK && status_code_property_value_recently_constant) {
        // If the flush early info has non-empty resource html, we flush early.
        DCHECK(options->enable_flush_subresources_experimental());

        // Check whether to flush lazyload and js_defer script snippets early.
        PropertyValue* lazyload_property_value = page->GetProperty(
            cohort,
            LazyloadImagesFilter::kIsLazyloadScriptInsertedPropertyName);
        if (lazyload_property_value->has_value() &&
            StringCaseEqual(lazyload_property_value->value(), "1") &&
            options->Enabled(RewriteOptions::kLazyloadImages) &&
            LazyloadImagesFilter::ShouldApply(driver_)) {
          driver_->set_is_lazyload_script_flushed(true);
          should_flush_early_lazyload_script_ = true;
        }

        // We don't flush defer js here since blink js contains defer js.
        PropertyValue* defer_js_property_value = page->GetProperty(
            cohort,
            JsDeferDisabledFilter::kIsJsDeferScriptInsertedPropertyName);
        if (!options->Enabled(RewriteOptions::kSplitHtml) &&
            defer_js_property_value->has_value() &&
            StringCaseEqual(defer_js_property_value->value(), "1") &&
            options->Enabled(RewriteOptions::kDeferJavascript) &&
            JsDeferDisabledFilter::ShouldApply(driver_)) {
          driver_->set_is_defer_javascript_script_flushed(true);
          should_flush_early_js_defer_script_ = true;
        }

        int64 now_ms = manager_->timer()->NowMs();
        // Clone the RewriteDriver which is used rewrite the HTML that we are
        // trying to flush early.
        RewriteDriver* new_driver = driver_->Clone();
        new_driver->increment_async_events_count();
        new_driver->set_response_headers_ptr(base_fetch_->response_headers());
        new_driver->set_request_headers(base_fetch_->request_headers());
        new_driver->set_flushing_early(true);

        new_driver->SetWriter(base_fetch_);
        new_driver->set_user_agent(driver_->user_agent());
        new_driver->StartParse(url_);

        InitFlushEarlyDriverWithPropertyCacheValues(new_driver, page);
        if (flush_early_info.has_average_fetch_latency_ms()) {
          average_fetch_time_ = flush_early_info.average_fetch_latency_ms();
        }
        // Copy over the response headers from flush_early_info.
        GenerateResponseHeaders(flush_early_info);

        // Write the pre-head content out to the user. Note that we also pass
        // the pre-head content to new_driver but it is not written out by it.
        // This is so that we can flush other content such as the javascript
        // needed by filters from here. Also, we may need the pre-head to detect
        // the encoding of the page.
        base_fetch_->Write(flush_early_info.pre_head(), handler_);
        base_fetch_->Write("<head>", handler_);
        base_fetch_->Flush(handler_);

        // Parse and rewrite the flush early HTML.
        new_driver->ParseText(flush_early_info.pre_head());
        new_driver->ParseText("<head>");
        new_driver->ParseText(flush_early_info.resource_html());

        const StringSet* css_critical_images =
            new_driver->css_critical_images();
        if (new_driver->options()->
            flush_more_resources_early_if_time_permits() &&
            css_critical_images != NULL) {
          // Critical images inside css.
          StringSet::iterator it = css_critical_images->begin();
          for (; it != css_critical_images->end(); ++it) {
            new_driver->ParseText("<img src='");
            GoogleString escaped_url;
            HtmlKeywords::Escape(*it, &escaped_url);
            new_driver->ParseText(escaped_url);
            new_driver->ParseText("' />");
          }
        }
        driver_->set_flushed_early(true);
        // Keep driver alive till the FlushEarlyFlow is completed.
        num_requests_flushed_early_->IncBy(1);

        // This deletes the driver once done.
        new_driver->FinishParseAsync(
            MakeFunction(this, &FlushEarlyFlow::FlushEarlyRewriteDone, now_ms,
                         new_driver));
        return;
      } else if (!options->enable_flush_subresources_experimental()) {
        // TODO(mmohabey): Remove non experimental flow.
        GenerateDummyHeadAndCountResources(flush_early_info);
        if (flush_early_info.response_headers().status_code() ==
            HttpStatus::kOK && num_resources_flushed_ > 0) {
          handler_->Message(kInfo, "Flushed %d Subresources Early for %s.",
                            num_resources_flushed_, url_.c_str());
          num_requests_flushed_early_->IncBy(1);
          num_resources_flushed_early_->IncBy(num_resources_flushed_);
          GenerateResponseHeaders(flush_early_info);
          base_fetch_->Write(dummy_head_, handler_);
          base_fetch_->Flush(handler_);
          driver_->set_flushed_early(true);
        }
      }
    }
  }
  flush_early_fetch_->set_flush_early_flow_done(driver_->flushed_early());
  delete this;
}

void FlushEarlyFlow::Cancel() {
  flush_early_fetch_->set_flush_early_flow_done(false);
  delete this;
}

void FlushEarlyFlow::FlushEarlyRewriteDone(int64 start_time_ms,
                                           RewriteDriver* flush_early_driver) {
  int max_preconnect_attempts = std::min(
      kMaxParallelConnections, num_rewritten_resources_ -
      flush_early_driver->num_flushed_early_pagespeed_resources()) -
      flush_early_driver->num_flushed_early_pagespeed_resources();

  StaticJavascriptManager* static_js_manager =
        manager_->static_javascript_manager();
  if (should_flush_early_lazyload_script_) {
    // Flush Lazyload filter script content.
    WriteScript(LazyloadImagesFilter::GetLazyloadJsSnippet(
        driver_->options(), static_js_manager));
    if (!driver_->options()->lazyload_images_blank_url().empty()) {
      --max_preconnect_attempts;
    }
  }

  if (should_flush_early_js_defer_script_) {
    // Flush defer_javascript script content.
    WriteScript(JsDisableFilter::GetJsDisableScriptSnippet(driver_->options()));
    WriteExternalScript(static_js_manager->GetDeferJsUrl(driver_->options()));
    WriteScript(JsDeferDisabledFilter::kSuffix);
  }

  if (max_preconnect_attempts > 0 &&
      !flush_early_driver->options()->pre_connect_url().empty() &&
      average_fetch_time_ > kMinLatencyForPreconnectMs) {
    for (int index = 0; index < max_preconnect_attempts; ++index) {
      GoogleString url =
          StrCat(flush_early_driver->options()->pre_connect_url(),
                 "?id=",IntegerToString(index));
      base_fetch_->Write(StringPrintf("<link rel=\"stylesheet\" href=\"%s\"/>",
                                      url.c_str()), handler_);
    }
  }
  flush_early_driver->decrement_async_events_count();
  base_fetch_->Write("</head>", handler_);
  base_fetch_->Flush(handler_);
  flush_early_rewrite_latency_ms_->Add(
      manager_->timer()->NowMs() - start_time_ms);
  flush_early_fetch_->set_flush_early_flow_done(true);
  delete this;
}

void FlushEarlyFlow::WriteScript(const GoogleString& script_content) {
  base_fetch_->Write("<script type=\"text/javascript\">", handler_);
  base_fetch_->Write(script_content, handler_);
  base_fetch_->Write("</script>", handler_);
}

void FlushEarlyFlow::WriteExternalScript(const GoogleString& script_url) {
  base_fetch_->Write("<script src=\"", handler_);
  base_fetch_->Write(script_url, handler_);
  base_fetch_->Write("\" type=\"text/javascript\"></script>", handler_);
}

void FlushEarlyFlow::GenerateResponseHeaders(
    const FlushEarlyInfo& flush_early_info) {
  ResponseHeaders* response_headers = base_fetch_->response_headers();
  response_headers->UpdateFromProto(flush_early_info.response_headers());
  // TODO(mmohabey): Add this header only when debug filter is on.
  response_headers->Add(kPsaRewriterHeader, kFlushSubresourcesFilter);
  response_headers->SetDateAndCaching(manager_->timer()->NowMs(), 0,
                                      ", private, no-cache");
  response_headers->ComputeCaching();
  base_fetch_->HeadersComplete();
}

void FlushEarlyFlow::GenerateDummyHeadAndCountResources(
    const FlushEarlyInfo& flush_early_info) {
  Write(flush_early_info.pre_head());
  Write("<head>");
  GoogleString head_string, script, minified_script;
  bool has_script = false;
  switch (manager_->user_agent_matcher().GetPrefetchMechanism(
      driver_->user_agent().data())) {
    case UserAgentMatcher::kPrefetchNotSupported:
      LOG(DFATAL) << "Entered Flush Early Flow for a unsupported user agent";
      break;
    case UserAgentMatcher::kPrefetchLinkRelSubresource:
      head_string = GetHeadString(
          flush_early_info,
          FlushEarlyContentWriterFilter::kPrefetchLinkRelSubresourceHtml,
          FlushEarlyContentWriterFilter::kPrefetchLinkRelSubresourceHtml);
      break;
    case UserAgentMatcher::kPrefetchImageTag:
      has_script = true;
      script = GetHeadString(
          flush_early_info,
          FlushEarlyContentWriterFilter::kPrefetchImageTagHtml,
          FlushEarlyContentWriterFilter::kPrefetchImageTagHtml);
      break;
    case UserAgentMatcher::kPrefetchLinkScriptTag:
      head_string = GetHeadString(
          flush_early_info,
          FlushEarlyContentWriterFilter::kPrefetchLinkTagHtml,
          FlushEarlyContentWriterFilter::kPrefetchScriptTagHtml);
      break;
    case UserAgentMatcher::kPrefetchObjectTag:
      has_script = true;
      StrAppend(&script, kPreloadScript, GetHeadString(flush_early_info,
                kPrefetchObjectTagHtml, kPrefetchObjectTagHtml));
      break;
  }
  if (has_script) {
    if (!driver_->options()->Enabled(RewriteOptions::kDebug)) {
      pagespeed::js::MinifyJs(script, &minified_script);
      Write(StringPrintf(kScriptBlock, minified_script.c_str()));
    } else {
      Write(StringPrintf(kScriptBlock, script.c_str()));
    }
  } else {
    Write(head_string);
  }
  Write(StringPrintf(FlushEarlyContentWriterFilter::kPrefetchStartTimeScript,
                     num_resources_flushed_));
  Write("</head>");
}

GoogleString FlushEarlyFlow::GetHeadString(
    const FlushEarlyInfo& flush_early_info, const char* css_format,
    const char* js_format) {
  GoogleString head_string;
  for (int i = 0; i < flush_early_info.subresource_size(); ++i) {
    const char* chosen_format = css_format;
    if (flush_early_info.subresource(i).content_type() == JAVASCRIPT) {
      if (driver_->options()->Enabled(RewriteOptions::kDeferJavascript)) {
          continue;
      }
      chosen_format = js_format;
    }
    StrAppend(&head_string, StringPrintf(
        chosen_format,
        flush_early_info.subresource(i).rewritten_url().c_str()));
    ++num_resources_flushed_;
  }
  return head_string;
}

void FlushEarlyFlow::Write(const StringPiece& val) {
  dummy_head_writer_.Write(val, handler_);
}

}  // namespace net_instaweb
