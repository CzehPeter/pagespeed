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

// Author: pulkitg@google.com (Pulkit Goyal)
//
// This class manages the flow of a blink request. In order to flush the
// critical html early before we start getting bytes back from the
// fetcher, we lookup property cache for BlinkCriticalLineData.
// If found, we flush critical html out and then trigger the normal
// ProxyFetch flow with customized options which extracts cookies and
// non cacheable panels from the page and sends it out.
// If BlinkCriticalLineData is not found in cache, we pass this request through
// normal ProxyFetch flow buffering the html. In the background we
// create a driver to parse it, run it through other filters, compute
// BlinkCriticalLineData and store it into the property cache.

#include "net/instaweb/automatic/public/blink_flow_critical_line.h"

#include "base/logging.h"
#include "net/instaweb/automatic/public/proxy_fetch.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/blink_critical_line_data.pb.h"
#include "net/instaweb/rewriter/public/blink_critical_line_data_finder.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

namespace {

// AsyncFetch that doesn't call HeadersComplete() on the base fetch. Note that
// this class only links the request headers from the base fetch and does not
// link the response headers.
// This is used as a wrapper around the base fetch when BlinkCriticalLineData is
// found in cache. This is done because the response headers and the
// BlinkCriticalLineData have been already been flushed out in the base fetch
// and we don't want to call HeadersComplete() twice on the base fetch.
// This class deletes itself when HandleDone() is called.
class AsyncFetchWithHeadersInhibited : public AsyncFetchUsingWriter {
 public:
  explicit AsyncFetchWithHeadersInhibited(AsyncFetch* fetch)
      : AsyncFetchUsingWriter(fetch),
        base_fetch_(fetch) {
    set_request_headers(fetch->request_headers());
  }

 private:
  virtual ~AsyncFetchWithHeadersInhibited() {}
  virtual void HandleHeadersComplete() {}
  virtual void HandleDone(bool success) {
    base_fetch_->Done(success);
    delete this;
  }

  AsyncFetch* base_fetch_;

  DISALLOW_COPY_AND_ASSIGN(AsyncFetchWithHeadersInhibited);
};

// SharedAsyncFetch that fetches the page and passes events through to the base
// fetch. It also determines if the page is html, and whether to trigger an
// async computation of the critical line data.
// TODO(rahulbansal): Buffer the html chunked rather than in one string.
class SharedFetch : public SharedAsyncFetch {
 public:
  SharedFetch(AsyncFetch* base_fetch,
              const GoogleString& url,
              ResourceManager* resource_manager,
              RewriteOptions* options,
              RewriteDriver* rewrite_driver)
      : SharedAsyncFetch(base_fetch),
        url_(url),
        resource_manager_(resource_manager),
        options_(options),
        compute_critical_line_data_(false),
        rewrite_driver_(rewrite_driver) {
    // Makes rewrite_driver live longer as ProxyFetch may called Cleanup()
    // on the rewrite_driver even if ComputeBlinkCriticalLineData() has not yet
    // been triggered.
    rewrite_driver_->increment_async_events_count();
  }

  virtual ~SharedFetch() {
    rewrite_driver_->decrement_async_events_count();
  }

  virtual void HandleHeadersComplete() {
    base_fetch()->HeadersComplete();
    if (response_headers()->status_code() == HttpStatus::kOK) {
      const ContentType* type = response_headers()->DetermineContentType();
      // TODO(pulkitg): Use ProxyFetch logic to check whether it is Html or not.
      if (type != NULL && type->IsHtmlLike()) {
        compute_critical_line_data_ = true;
      } else {
        VLOG(1) << "Non html page, not rewritable: " << url_;
      }
    } else {
      VLOG(1) << "Non 200 response code for: " << url_;
    }
  }

  virtual bool HandleWrite(const StringPiece& content,
                           MessageHandler* handler) {
    bool ret = base_fetch()->Write(content, handler);
    if (compute_critical_line_data_) {
      content.AppendToString(&buffer_);
    }
    return ret;
  }

  virtual void HandleDone(bool success) {
    // TODO(pulkitg): Add Statistics class variables.
    compute_critical_line_data_ &= success;
    if (!compute_critical_line_data_) {
      base_fetch()->Done(success);
      delete this;
    } else {
      // Store base_fetch() in a temp since it might get deleted before calling
      // Done.
      AsyncFetch* fetch = base_fetch();
      critical_line_headers_.CopyFrom(*response_headers());
      critical_line_computation_driver_ =
          resource_manager_->NewCustomRewriteDriver(
          options_.release());
      // Wait for all rewrites to complete. This is important because fully
      // rewritten html is used to compute BlinkCriticalLineData.
      critical_line_computation_driver_->set_fully_rewrite_on_flush(true);
      critical_line_computation_driver_->SetWriter(&value_);
      critical_line_computation_driver_->set_response_headers_ptr(
          &critical_line_headers_);
      critical_line_computation_driver_->AddRewriteTask(
          MakeFunction(this, &SharedFetch::Parse));
      // We call Done after scheduling the rewrite on the driver since we expect
      // this to be very low cost. Calling Done on base_fetch() before
      // scheduling the rewrite causes problems with testing.
      fetch->Done(success);
    }
  }

  void Parse() {
    if (critical_line_computation_driver_->StartParse(url_)) {
      critical_line_computation_driver_->ParseText(buffer_);
      critical_line_computation_driver_->FinishParseAsync(MakeFunction(
          this, &SharedFetch::CompleteFinishParse));
    } else {
      LOG(ERROR) << "StartParse failed for url: " << url_;
      critical_line_computation_driver_->Cleanup();
      delete this;
    }
  }

  void CompleteFinishParse() {
    StringPiece rewritten_content;
    value_.ExtractContents(&rewritten_content);
    resource_manager_->blink_critical_line_data_finder()
        ->ComputeBlinkCriticalLineData(rewritten_content, rewrite_driver_);
    delete this;
  }

 private:
  GoogleString url_;
  ResourceManager* resource_manager_;
  scoped_ptr<RewriteOptions> options_;
  bool compute_critical_line_data_;
  ResponseHeaders critical_line_headers_;
  GoogleString buffer_;
  HTTPValue value_;
  // RewriteDriver passed to ProxyFetch to serve user-facing request.
  RewriteDriver* rewrite_driver_;
  // RewriteDriver used to parse the buffered html content.
  RewriteDriver* critical_line_computation_driver_;

  DISALLOW_COPY_AND_ASSIGN(SharedFetch);
};

}  // namespace

void BlinkFlowCriticalLine::Start(
    const GoogleString& url,
    AsyncFetch* base_fetch,
    RewriteOptions* options,
    ProxyFetchFactory* factory,
    ResourceManager* manager,
    ProxyFetchPropertyCallbackCollector* property_callback) {
  BlinkFlowCriticalLine* flow = new BlinkFlowCriticalLine(
      url, base_fetch, options, factory, manager, property_callback);
  Function* func = MakeFunction(
      flow, &BlinkFlowCriticalLine::BlinkCriticalLineDataLookupDone,
      property_callback);
  property_callback->AddPostLookupTask(func);
}

BlinkFlowCriticalLine::~BlinkFlowCriticalLine() {
}

BlinkFlowCriticalLine::BlinkFlowCriticalLine(
    const GoogleString& url,
    AsyncFetch* base_fetch,
    RewriteOptions* options,
    ProxyFetchFactory* factory,
    ResourceManager* manager,
    ProxyFetchPropertyCallbackCollector* property_callback)
    : url_(url),
      base_fetch_(base_fetch),
      options_(options),
      factory_(factory),
      manager_(manager),
      property_callback_(property_callback),
      finder_(manager->blink_critical_line_data_finder()) {}

void BlinkFlowCriticalLine::BlinkCriticalLineDataLookupDone(
    ProxyFetchPropertyCallbackCollector* collector) {
  PropertyPage* page = collector->GetPropertyPageWithoutOwnership(
      ProxyFetchPropertyCallback::kPagePropertyCache);
  // finder_ will be never NULL because it is checked before entering
  // BlinkFlowCriticalLine.
  blink_critical_line_data_.reset(
      finder_->ExtractBlinkCriticalLineData(page, options_));

  if (blink_critical_line_data_.get() != NULL) {
    BlinkCriticalLineDataHit();
    return;
  }
  // If BlinkCriticalLineData is not present in the cache.
  TriggerProxyFetch(false);
}

void BlinkFlowCriticalLine::BlinkCriticalLineDataHit() {
  // TODO(rahulbansal): Add CacheHit Case.
}

void BlinkFlowCriticalLine::TriggerProxyFetch(bool critical_line_data_found) {
  AsyncFetch* fetch = NULL;
  RewriteOptions* options = NULL;
  if (!critical_line_data_found) {
    options = options_->Clone();
    // TODO(pulkitg): We are temporarily disabling all rewriters since
    // SharedJsonFetch uses the output of ProxyFetch which may be rewritten. Fix
    // this.
    options_->ClearFilters();
  }

  // NewCustomRewriteDriver takes ownership of custom_options_.
  manager_->ComputeSignature(options_);
  RewriteDriver* driver = manager_->NewCustomRewriteDriver(options_);

  if (critical_line_data_found) {
    // Remove any headers that can lead to a 304, since blink can't handle 304s.
    base_fetch_->request_headers()->RemoveAll(HttpAttributes::kIfNoneMatch);
    base_fetch_->request_headers()->RemoveAll(HttpAttributes::kIfModifiedSince);
    // Pass a new fetch into proxy fetch that inhibits HeadersComplete() on the
    // base fetch. It also doesn't attach the response headers from the base
    // fetch since headers have already been flushed out.
    fetch = base_fetch_;
    // TODO(pulkitg) : Uncomment following statement when
    // BlinkCriticalLineDataHit() code is ready.
    // fetch = new AsyncFetchWithHeadersInhibited(base_fetch_);
  } else {
    SetFilterOptions(options);
    fetch = new SharedFetch(
        base_fetch_, url_, manager_, options, driver);
  }
  factory_->StartNewProxyFetch(url_, fetch, driver, property_callback_);
  delete this;
}

void BlinkFlowCriticalLine::SetFilterOptions(RewriteOptions* options) const {
  options->DisableFilter(RewriteOptions::kCombineCss);
  options->DisableFilter(RewriteOptions::kCombineJavascript);
  options->DisableFilter(RewriteOptions::kMoveCssToHead);
  options->DisableFilter(RewriteOptions::kLazyloadImages);
  // TODO(rahulbansal): ConvertMetaTags is a special case incompatible filter
  // which actually causes a SIGSEGV.
  options->DisableFilter(RewriteOptions::kConvertMetaTags);
  options->DisableFilter(RewriteOptions::kDeferJavascript);

  options->ForceEnableFilter(RewriteOptions::kHtmlWriterFilter);
  options->ForceEnableFilter(RewriteOptions::kStripNonCacheable);
  options->ForceEnableFilter(RewriteOptions::kDisableJavascript);

  options->set_min_image_size_low_resolution_bytes(0);
  // Enable inlining for all the images in html.
  options->set_max_inlined_preview_images_index(-1);
}

}  // namespace net_instaweb
