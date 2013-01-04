/*
 * Copyright 2011 Google Inc.
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

// Author: sligocki@google.com (Shawn Ligocki)

#include "net/instaweb/automatic/public/proxy_fetch.h"

#include <algorithm>
#include <cstddef>

#include "base/logging.h"
#include "net/instaweb/http/public/cache_url_async_fetcher.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/public/furious_matcher.h"
#include "net/instaweb/rewriter/public/furious_util.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/abstract_client_state.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/queued_alarm.h"
#include "net/instaweb/util/public/stl_util.h"
#include "net/instaweb/util/public/thread_synchronizer.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"

namespace net_instaweb {

const char ProxyFetch::kCollectorDone[] = "Collector:Done";
const char ProxyFetch::kCollectorPrefix[] = "Collector:";
const char ProxyFetch::kCollectorReady[] = "Collector:Ready";
const char ProxyFetch::kCollectorDelete[] = "Collector:Delete";
const char ProxyFetch::kCollectorDetach[] = "CollectorDetach";
const char ProxyFetch::kCollectorDoneDelete[] = "CollectorDoneDelete";

const char ProxyFetch::kHeadersSetupRaceAlarmQueued[] =
    "HeadersSetupRace:AlarmQueued";
const char ProxyFetch::kHeadersSetupRaceDone[] = "HeadersSetupRace:Done";
const char ProxyFetch::kHeadersSetupRaceFlush[] = "HeadersSetupRace:Flush";
const char ProxyFetch::kHeadersSetupRacePrefix[] = "HeadersSetupRace:";
const char ProxyFetch::kHeadersSetupRaceWait[] = "HeadersSetupRace:Wait";

ProxyFetchFactory::ProxyFetchFactory(ServerContext* manager)
    : manager_(manager),
      timer_(manager->timer()),
      handler_(manager->message_handler()),
      outstanding_proxy_fetches_mutex_(manager->thread_system()->NewMutex()) {
}

ProxyFetchFactory::~ProxyFetchFactory() {
  // Factory should outlive all fetches.
  DCHECK(outstanding_proxy_fetches_.empty());
  // Note: access to the set-size is not mutexed but in theory we should
  // be quiesced by this point.
  LOG(INFO) << "ProxyFetchFactory exiting with "
            << outstanding_proxy_fetches_.size()
            << " outstanding requests.";
}

ProxyFetch* ProxyFetchFactory::CreateNewProxyFetch(
    const GoogleString& url_in, AsyncFetch* async_fetch,
    RewriteDriver* driver,
    ProxyFetchPropertyCallbackCollector* property_callback,
    AsyncFetch* original_content_fetch) {
  const GoogleString* url_to_fetch = &url_in;

  // Check whether this an encoding of a non-rewritten resource served
  // from a non-transparently proxied domain.
  UrlNamer* namer = manager_->url_namer();
  GoogleString decoded_resource;
  GoogleUrl gurl(url_in), request_origin;
  DCHECK(!manager_->IsPagespeedResource(gurl))
      << "expect ResourceFetch called for pagespeed resources, not ProxyFetch";

  bool cross_domain = false;
  if (gurl.is_valid()) {
    if (namer->Decode(gurl, &request_origin, &decoded_resource)) {
      const RewriteOptions* options = driver->options();
      if (namer->IsAuthorized(gurl, *options)) {
        // The URL is proxied, but is not rewritten as a pagespeed resource,
        // so don't try to do the cache-lookup or URL fetch without stripping
        // the proxied portion.
        url_to_fetch = &decoded_resource;
        cross_domain = true;
      } else {
        async_fetch->response_headers()->SetStatusAndReason(
            HttpStatus::kForbidden);
        driver->Cleanup();
        if (property_callback != NULL) {
          property_callback->Detach(HttpStatus::kForbidden);
        }
        async_fetch->Done(false);
        if (original_content_fetch != NULL) {
          original_content_fetch->Done(false);
        }
        return NULL;
      }
    }
  }

  ProxyFetch* fetch = new ProxyFetch(
      *url_to_fetch, cross_domain, property_callback, async_fetch,
      original_content_fetch, driver, manager_, timer_, this);
  if (cross_domain) {
    // If we're proxying resources from a different domain, the host header is
    // likely set to the proxy host rather than the origin host.  Depending on
    // the origin, this will not work: it will not expect to see
    // the Proxy Host in its headers.
    fetch->request_headers()->RemoveAll(HttpAttributes::kHost);

    // The domain is also supposed to be cookieless, so enforce not
    // sending any cookies to origin, as a precaution against contamination.
    fetch->request_headers()->RemoveAll(HttpAttributes::kCookie);
    fetch->request_headers()->RemoveAll(HttpAttributes::kCookie2);

    // Similarly we don't want to forward authorization, since we may end up
    // forwarding it to wrong host. For proxy-authorization, we remove it here
    // since if our own server implements it, it should do so before touching
    // ProxyInterface, and this prevents it from accidentally leaking.
    // TODO(morlovich): Should we also change 401 and 407 into a 403 on
    // response?
    fetch->request_headers()->RemoveAll(HttpAttributes::kAuthorization);
    fetch->request_headers()->RemoveAll(HttpAttributes::kProxyAuthorization);
  } else {
    // If we didn't already remove all the cookies, remove the furious
    // ones so we don't confuse the origin.
    furious::RemoveFuriousCookie(fetch->request_headers());
  }
  RegisterNewFetch(fetch);
  return fetch;
}

void ProxyFetchFactory::StartNewProxyFetch(
    const GoogleString& url_in, AsyncFetch* async_fetch,
    RewriteDriver* driver,
    ProxyFetchPropertyCallbackCollector* property_callback,
    AsyncFetch* original_content_fetch) {
  ProxyFetch* fetch = CreateNewProxyFetch(
      url_in, async_fetch, driver, property_callback, original_content_fetch);
  if (fetch != NULL) {
    fetch->StartFetch();
  }
}

void ProxyFetchFactory::RegisterNewFetch(ProxyFetch* fetch) {
  ScopedMutex lock(outstanding_proxy_fetches_mutex_.get());
  outstanding_proxy_fetches_.insert(fetch);
}

void ProxyFetchFactory::RegisterFinishedFetch(ProxyFetch* fetch) {
  ScopedMutex lock(outstanding_proxy_fetches_mutex_.get());
  outstanding_proxy_fetches_.erase(fetch);
}

ProxyFetchPropertyCallback::ProxyFetchPropertyCallback(
    CacheType cache_type,
    const PropertyCache& property_cache,
    const StringPiece& key,
    UserAgentMatcher::DeviceType device_type,
    ProxyFetchPropertyCallbackCollector* collector,
    AbstractMutex* mutex)
    : PropertyPage(mutex, property_cache, key, collector->request_context()),
      cache_type_(cache_type),
      device_type_(device_type),
      collector_(collector) {
}

bool ProxyFetchPropertyCallback::IsCacheValid(int64 write_timestamp_ms) const {
  return collector_->IsCacheValid(write_timestamp_ms);
}

void ProxyFetchPropertyCallback::Done(bool success) {
  collector_->Done(this, success);
}

ProxyFetchPropertyCallbackCollector::ProxyFetchPropertyCallbackCollector(
    ServerContext* server_context, const StringPiece& url,
    const RequestContextPtr& request_ctx, const RewriteOptions* options,
    const StringPiece& user_agent)
    : mutex_(server_context->thread_system()->NewMutex()),
      server_context_(server_context),
      url_(url.data(), url.size()),
      request_context_(request_ctx),
      user_agent_(user_agent.data(), user_agent.size()),
      detached_(false),
      done_(false),
      success_(true),
      proxy_fetch_(NULL),
      post_lookup_task_vector_(new std::vector<Function*>),
      options_(options),
      status_code_(HttpStatus::kUnknownStatusCode) {
}

ProxyFetchPropertyCallbackCollector::~ProxyFetchPropertyCallbackCollector() {
  if (post_lookup_task_vector_ != NULL &&
      !post_lookup_task_vector_->empty()) {
    LOG(DFATAL) << "ProxyFetchPropertyCallbackCollector function vector is not "
                << "empty.";
  }
  STLDeleteElements(&pending_callbacks_);
  STLDeleteValues(&property_pages_);
  STLDeleteValues(&property_pages_for_device_types_);
}

void ProxyFetchPropertyCallbackCollector::AddCallback(
    ProxyFetchPropertyCallback* callback) {
  ScopedMutex lock(mutex_.get());
  pending_callbacks_.insert(callback);
}

PropertyPage* ProxyFetchPropertyCallbackCollector::GetPropertyPage(
    ProxyFetchPropertyCallback::CacheType cache_type) {
  ScopedMutex lock(mutex_.get());
  PropertyPage* page = property_pages_[cache_type];
  property_pages_[cache_type] = NULL;
  return page;
}

UserAgentMatcher::DeviceType
ProxyFetchPropertyCallbackCollector::GetDeviceTypeFromDeviceCacheMutexHeld() {
  // TODO(ksimbili): Pass the property page from device cache.
  const UserAgentMatcher* user_agent_matcher =
      server_context_->user_agent_matcher();
  return user_agent_matcher->GetDeviceTypeForUA(user_agent_.c_str());
}

void ProxyFetchPropertyCallbackCollector::SetPropertyPageForDeviceTypeMutexHeld(
    UserAgentMatcher::DeviceType device_type) {
  property_pages_[ProxyFetchPropertyCallback::kPagePropertyCache] =
      property_pages_for_device_types_[device_type];
  property_pages_for_device_types_[device_type] = NULL;
}

PropertyPage*
ProxyFetchPropertyCallbackCollector::GetPropertyPageWithoutOwnership(
    ProxyFetchPropertyCallback::CacheType cache_type) {
  ScopedMutex lock(mutex_.get());
  PropertyPage* page = property_pages_[cache_type];
  return page;
}

bool ProxyFetchPropertyCallbackCollector::IsCacheValid(
    int64 write_timestamp_ms) const {
  ScopedMutex lock(mutex_.get());
  // Since PropertyPage::CallDone is not yet called, we know that
  // ProxyFetchPropertyCallbackCollector::Done is not called and hence done_ is
  // false and hence this has not yet been deleted.
  DCHECK(!done_);
  // But Detach might have been called already and then options_ is not valid.
  if (detached_) {
    return false;
  }
  return (options_ == NULL ||
          options_->IsUrlCacheValid(url_, write_timestamp_ms));
}

// Calls to Done(), ConnectProxyFetch(), and Detach() may occur on
// different threads.  Exactly one of ConnectProxyFetch and Detach will
// never race with each other, as they correspond to the construction
// or destruction of ProxyFetch, but either can race with Done().  Note
// that ConnectProxyFetch can be followed by Detach if it turns out that
// a URL without a known extension is *not* HTML.  See
// ProxyInterfaceTest.PropCacheNoWritesIfNonHtmlDelayedCache.

void ProxyFetchPropertyCallbackCollector::Done(
    ProxyFetchPropertyCallback* callback, bool success) {
  ServerContext* resource_manager = NULL;
  ProxyFetch* fetch = NULL;
  scoped_ptr<std::vector<Function*> > post_lookup_task_vector;
  bool do_delete = false;
  bool call_post = false;
  {
    ScopedMutex lock(mutex_.get());
    pending_callbacks_.erase(callback);
    if (callback->cache_type() ==
        ProxyFetchPropertyCallback::kPagePropertyCache) {
      property_pages_for_device_types_[callback->device_type()] = callback;
    } else {
      property_pages_[callback->cache_type()] = callback;
    }
    success_ &= success;

    if (pending_callbacks_.empty()) {
      SetPropertyPageForDeviceTypeMutexHeld(
          GetDeviceTypeFromDeviceCacheMutexHeld());
      // There is a race where Detach() can be called immediately after we
      // release the lock below, and it (Detach) deletes 'this' (because we
      // just set done_ to true), which means we cannot rely on any data
      // members being valid after releasing the lock, so we copy them all.
      resource_manager = server_context_;
      post_lookup_task_vector.reset(post_lookup_task_vector_.release());
      call_post = true;
    }
  }
  if (call_post) {
    ThreadSynchronizer* sync = resource_manager->thread_synchronizer();
    sync->Signal(ProxyFetch::kCollectorReady);
    sync->Wait(ProxyFetch::kCollectorDetach);
    sync->Wait(ProxyFetch::kCollectorDone);
    if (post_lookup_task_vector.get() != NULL) {
      for (int i = 0, n = post_lookup_task_vector->size(); i < n; ++i) {
        (*post_lookup_task_vector.get())[i]->CallRun();
      }
    }
    {
      ScopedMutex lock(mutex_.get());
      done_ = true;
      fetch = proxy_fetch_;
      do_delete = detached_;
    }
    if (fetch != NULL) {
      fetch->PropertyCacheComplete(success_, this);  // deletes this.
    } else if (do_delete) {
      UpdateStatusCodeInPropertyCache();
      delete this;
      sync->Signal(ProxyFetch::kCollectorDelete);
      sync->Signal(ProxyFetch::kCollectorDoneDelete);
    }
  }
}

void ProxyFetchPropertyCallbackCollector::ConnectProxyFetch(
    ProxyFetch* proxy_fetch) {
  bool ready = false;
  {
    ScopedMutex lock(mutex_.get());
    DCHECK(proxy_fetch_ == NULL);
    DCHECK(!detached_);
    proxy_fetch_ = proxy_fetch;
    ready = done_;
  }
  if (ready) {
    proxy_fetch->PropertyCacheComplete(success_, this);  // deletes this.
  }
}

void ProxyFetchPropertyCallbackCollector::UpdateStatusCodeInPropertyCache() {
  // If we have not transferred the ownership of PagePropertyCache to
  // ProxyFetch yet, and we have the status code, then write the status_code in
  // PropertyCache.
  PropertyPage* page =
      property_pages_[ProxyFetchPropertyCallback::kPagePropertyCache];
  PropertyCache* pcache = server_context_->page_property_cache();
  if (pcache != NULL && page != NULL &&
      status_code_ != HttpStatus::kUnknownStatusCode) {
    const PropertyCache::Cohort* dom = pcache->GetCohort(
        RewriteDriver::kDomCohort);
    if (dom != NULL) {
      PropertyValue* value = page->GetProperty(
          dom, RewriteDriver::kStatusCodePropertyName);
      pcache->UpdateValue(IntegerToString(status_code_), value);
      pcache->WriteCohort(dom, page);
    } else {
      server_context_->message_handler()->Message(
          kInfo, "dom cohort is not available for url %s.", url_.c_str());
    }
  }
}

void ProxyFetchPropertyCallbackCollector::Detach(HttpStatus::Code status_code) {
  bool do_delete = false;
  ThreadSynchronizer* sync = server_context_->thread_synchronizer();
  scoped_ptr<std::vector<Function*> > post_lookup_task_vector;
  {
    ScopedMutex lock(mutex_.get());
    proxy_fetch_ = NULL;
    DCHECK(!detached_);
    detached_ = true;
    do_delete = done_;
    post_lookup_task_vector.reset(post_lookup_task_vector_.release());
    status_code_ = status_code;
  }
  // Do not access class variables below this as the object might be deleted by
  // Done() in a different thread.
  if (post_lookup_task_vector.get() != NULL) {
    for (int i = 0, n = post_lookup_task_vector->size(); i < n; ++i) {
      (*post_lookup_task_vector.get())[i]->CallCancel();
    }
  }
  sync->Signal(ProxyFetch::kCollectorDetach);
  sync->Wait(ProxyFetch::kCollectorDoneDelete);
  if (do_delete) {
    UpdateStatusCodeInPropertyCache();
    delete this;
    sync->Signal(ProxyFetch::kCollectorDelete);
  }
}

void ProxyFetchPropertyCallbackCollector::AddPostLookupTask(Function* func) {
  bool do_run = false;
  {
    ScopedMutex lock(mutex_.get());
    DCHECK(!detached_);
    do_run = post_lookup_task_vector_.get() == NULL;
    if (!do_run) {
      post_lookup_task_vector_->push_back(func);
    }
  }
  if (do_run) {
    func->CallRun();
  }
}

ProxyFetch::ProxyFetch(
    const GoogleString& url,
    bool cross_domain,
    ProxyFetchPropertyCallbackCollector* property_cache_callback,
    AsyncFetch* async_fetch,
    AsyncFetch* original_content_fetch,
    RewriteDriver* driver,
    ServerContext* manager,
    Timer* timer,
    ProxyFetchFactory* factory)
    : SharedAsyncFetch(async_fetch),
      url_(url),
      server_context_(manager),
      timer_(timer),
      cross_domain_(cross_domain),
      claims_html_(false),
      started_parse_(false),
      parse_text_called_(false),
      done_called_(false),
      property_cache_callback_(property_cache_callback),
      original_content_fetch_(original_content_fetch),
      driver_(driver),
      queue_run_job_created_(false),
      mutex_(manager->thread_system()->NewMutex()),
      network_flush_outstanding_(false),
      sequence_(NULL),
      done_outstanding_(false),
      finishing_(false),
      done_result_(false),
      waiting_for_flush_to_finish_(false),
      idle_alarm_(NULL),
      factory_(factory),
      prepare_success_(false) {
  set_request_headers(async_fetch->request_headers());
  set_response_headers(async_fetch->response_headers());

  // Now that we've created the RewriteDriver, include the client_id generated
  // from the original request headers, if any.
  const char* client_id = async_fetch->request_headers()->Lookup1(
      HttpAttributes::kXGooglePagespeedClientId);
  if (client_id != NULL) {
    driver_->set_client_id(client_id);
  }

  // Make request headers available to the filters.
  driver_->set_request_headers(request_headers());

  // Set the user agent in the rewrite driver if it is not set already.
  if (driver_->user_agent().empty()) {
    const char* user_agent = request_headers()->Lookup1(
        HttpAttributes::kUserAgent);
    if (user_agent != NULL) {
      VLOG(1) << "Setting user-agent to " << user_agent;
      driver_->set_user_agent(user_agent);
    } else {
      VLOG(1) << "User-agent empty";
    }
  }

  driver_->EnableBlockingRewrite(request_headers());

  // Set the implicit cache ttl for the response headers based on the value
  // specified in the options.
  response_headers()->set_implicit_cache_ttl_ms(
      Options()->implicit_cache_ttl_ms());

  VLOG(1) << "Attaching RewriteDriver " << driver_
          << " to HtmlRewriter " << this;
}

ProxyFetch::~ProxyFetch() {
  DCHECK(done_called_) << "Callback should be called before destruction";
  DCHECK(!queue_run_job_created_);
  DCHECK(!network_flush_outstanding_);
  DCHECK(!done_outstanding_);
  DCHECK(!waiting_for_flush_to_finish_);
  DCHECK(text_queue_.empty());
  DCHECK(property_cache_callback_ == NULL);
}

bool ProxyFetch::StartParse() {
  driver_->SetWriter(base_fetch());

  // The response headers get munged between when we initially determine
  // which rewrite options we need (in proxy_interface.cc) and here.
  // Therefore, we can not set the Set-Cookie header there, and must
  // do it here instead.
  if (Options()->need_to_store_experiment_data() &&
      Options()->running_furious()) {
    int furious_value = Options()->furious_id();
    server_context_->furious_matcher()->StoreExperimentData(
        furious_value, url_,
        server_context_->timer()->NowMs() +
            Options()->furious_cookie_duration_ms(),
        response_headers());
  }
  driver_->set_response_headers_ptr(response_headers());
  {
    // PropertyCacheComplete checks sequence_ to see whether it should
    // start processing queued text, so we need to mutex-protect it.
    // Often we expect the PropertyCache lookup to complete before
    // StartParse is called, but that is not guaranteed.
    ScopedMutex lock(mutex_.get());
    sequence_ = driver_->html_worker();
  }

  // Start parsing.
  // TODO(sligocki): Allow calling StartParse with GoogleUrl.
  if (!driver_->StartParse(url_)) {
    // We don't expect this to ever fail.
    LOG(ERROR) << "StartParse failed for URL: " << url_;
    return false;
  } else {
    VLOG(1) << "Parse successfully started.";
    return true;
  }
}

const RewriteOptions* ProxyFetch::Options() {
  return driver_->options();
}

void ProxyFetch::HandleHeadersComplete() {
  if (original_content_fetch_ != NULL) {
    ResponseHeaders* headers = original_content_fetch_->response_headers();
    headers->CopyFrom(*response_headers());
    original_content_fetch_->HeadersComplete();
  }
  // Figure out semantic info from response_headers_
  claims_html_ = response_headers()->IsHtmlLike();

  // Make sure we never serve cookies if the domain we are serving
  // under isn't the domain of the origin.
  if (cross_domain_) {
    // ... by calling Sanitize to remove them.
    bool changed = response_headers()->Sanitize();
    if (changed) {
      response_headers()->ComputeCaching();
    }
  }
}

void ProxyFetch::AddPagespeedHeader() {
  if (Options()->enabled()) {
    response_headers()->Add(kPageSpeedHeader, Options()->x_header_value());
    response_headers()->ComputeCaching();
  }
}

void ProxyFetch::SetupForHtml() {
  const RewriteOptions* options = Options();
  if (options->enabled() && options->IsAllowed(url_)) {
    started_parse_ = StartParse();
    if (started_parse_) {
      // TODO(sligocki): Get these in the main flow.
      // Add, remove and update headers as appropriate.
      int64 ttl_ms;
      GoogleString cache_control_suffix;
      if ((options->max_html_cache_time_ms() == 0) ||
          response_headers()->HasValue(
              HttpAttributes::kCacheControl, "no-cache") ||
          response_headers()->HasValue(
              HttpAttributes::kCacheControl, "must-revalidate")) {
        ttl_ms = 0;
        cache_control_suffix = ", no-cache";
        // Preserve values like no-store and no-transform.
        cache_control_suffix +=
            response_headers()->CacheControlValuesToPreserve();
      } else {
        ttl_ms = std::min(options->max_html_cache_time_ms(),
                          response_headers()->cache_ttl_ms());
        // TODO(sligocki): We defensively set Cache-Control: private, but if
        // original HTML was publicly cacheable, we should be able to set
        // the rewritten HTML as publicly cacheable likewise.
        // NOTE: If we do allow "public", we need to deal with other
        // Cache-Control quantifiers, like "proxy-revalidate".
        cache_control_suffix = ", private";
      }

      // When testing, wait a little here for unit tests to make sure
      // we don't race ahead & run filters while we are still cleaning
      // up headers.  When this particular bug is fixed,
      // HeadersComplete will *not* be called on base_fetch() until
      // after this function returns, so we'd block indefinitely.
      // Instead, block just for 200ms so the test can pass with
      // limited delay.  Note that this is a no-op except in test
      // ProxyInterfaceTest.FiltersRaceSetup which enables thread-sync
      // prefix "HeadersSetupRace:".
      ThreadSynchronizer* sync = server_context_->thread_synchronizer();
      sync->Signal(kHeadersSetupRaceWait);
      sync->TimedWait(kHeadersSetupRaceFlush, kTestSignalTimeoutMs);

      response_headers()->SetDateAndCaching(
          response_headers()->date_ms(), ttl_ms, cache_control_suffix);
      // TODO(sligocki): Support Etags and/or Last-Modified.
      response_headers()->RemoveAll(HttpAttributes::kEtag);
      response_headers()->RemoveAll(HttpAttributes::kLastModified);

      // HTML sizes are likely to be altered by HTML rewriting.
      response_headers()->RemoveAll(HttpAttributes::kContentLength);

      // TODO(sligocki): See mod_instaweb.cc line 528, which strips Expires and
      // Content-MD5.  Perhaps we should do that here as well.
    }
  }
}

void ProxyFetch::StartFetch() {
  factory_->manager_->url_namer()->PrepareRequest(
      Options(), &url_, request_headers(), &prepare_success_,
      MakeFunction(this, &ProxyFetch::DoFetch),
      factory_->handler_);
}

void ProxyFetch::DoFetch() {
  if (prepare_success_) {
    const RewriteOptions* options = driver_->options();

    if (options->enabled() && options->IsAllowed(url_)) {
      // Pagespeed enabled on URL.
      if (options->ajax_rewriting_enabled()) {
        // For Ajax rewrites, we go through RewriteDriver to give it
        // a chance to optimize resources. (If they are HTML, it will
        // not touch them, and we will stream them to the parser here).
        driver_->FetchResource(url_, this);
        return;
      }
      // Otherwise we just do a normal fetch from cache, and if it's
      // HTML we will do a streaming rewrite.
    } else {
      // Pagespeed disabled on URL.
      if (options->reject_blacklisted()) {
        // We were asked to error out in this case.
        response_headers()->SetStatusAndReason(
            options->reject_blacklisted_status_code());
        Done(true);
        return;
      }
      // Else we should do a passthrough. In that case, we still do a normal
      // origin fetch, but we will never rewrite anything, since
      // SetupForHtml() will re-check enabled() and IsAllowed();
    }

    cache_fetcher_.reset(driver_->CreateCacheFetcher());
    cache_fetcher_->Fetch(url_, factory_->handler_, this);
  } else {
    Done(false);
  }
}

void ProxyFetch::ScheduleQueueExecutionIfNeeded() {
  mutex_->DCheckLocked();

  // Already queued -> no need to queue again.
  if (queue_run_job_created_) {
    return;
  }

  // We're waiting for any property-cache lookups and previous flushes to
  // complete, so no need to queue it here.  The queuing will happen when
  // the PropertyCache lookup is complete or from FlushDone.
  if (waiting_for_flush_to_finish_ || (property_cache_callback_ != NULL)) {
    return;
  }

  queue_run_job_created_ = true;
  sequence_->Add(MakeFunction(this, &ProxyFetch::ExecuteQueued));
}

void ProxyFetch::PropertyCacheComplete(
    bool success, ProxyFetchPropertyCallbackCollector* callback_collector) {
  driver_->TracePrintf("PropertyCache lookup completed");
  ScopedMutex lock(mutex_.get());

  if (driver_ == NULL) {
    LOG(DFATAL) << "Expected non-null driver.";
  } else {
    // Set the page property, device property and client state objects
    // in the driver.
    driver_->set_property_page(
        callback_collector->GetPropertyPage(
            ProxyFetchPropertyCallback::kPagePropertyCache));
    driver_->set_device_property_page(
        callback_collector->GetPropertyPage(
            ProxyFetchPropertyCallback::kDevicePropertyCache));
    driver_->set_client_state(GetClientState(callback_collector));
  }
  // We have to set the callback to NULL to let ScheduleQueueExecutionIfNeeded
  // proceed (it waits until it's NULL). And we have to delete it because then
  // we have no reference to it to delete it in Finish.
  if (property_cache_callback_ == NULL) {
    LOG(DFATAL) << "Expected non-null property_cache_callback_.";
  } else {
    delete property_cache_callback_;
    ThreadSynchronizer* sync = server_context_->thread_synchronizer();
    sync->Signal(ProxyFetch::kCollectorDelete);
    property_cache_callback_ = NULL;
  }
  if (sequence_ != NULL) {
    ScheduleQueueExecutionIfNeeded();
  }
}

AbstractClientState* ProxyFetch::GetClientState(
    ProxyFetchPropertyCallbackCollector* collector) {
  // Do nothing if the client ID is unknown.
  if (driver_->client_id().empty()) {
    return NULL;
  }
  PropertyCache* cache = server_context_->client_property_cache();
  PropertyPage* client_property_page = collector->GetPropertyPage(
      ProxyFetchPropertyCallback::kClientPropertyCache);
  AbstractClientState* client_state =
      server_context_->factory()->NewClientState();
  client_state->InitFromPropertyCache(
      driver_->client_id(), cache, client_property_page, timer_);
  return client_state;
}

bool ProxyFetch::HandleWrite(const StringPiece& str,
                             MessageHandler* message_handler) {
  // TODO(jmarantz): check if the server is being shut down and punt.
  if (original_content_fetch_ != NULL) {
    original_content_fetch_->Write(str, message_handler);
  }

  if (claims_html_ && !html_detector_.already_decided()) {
    if (html_detector_.ConsiderInput(str)) {
      // Figured out whether really HTML or not.
      if (html_detector_.probable_html()) {
        SetupForHtml();
      }

      // Now we're done mucking about with headers, add one noting our
      // involvement.
      AddPagespeedHeader();

      if ((property_cache_callback_ != NULL) && started_parse_) {
        // Connect the ProxyFetch in the PropertyCacheCallbackCollector.  This
        // ensures that we will not start executing HTML filters until
        // property cache lookups are complete.
        property_cache_callback_->ConnectProxyFetch(this);
      }

      // If we buffered up any bytes in previous calls, make sure to
      // release them.
      GoogleString buffer;
      html_detector_.ReleaseBuffered(&buffer);
      if (!buffer.empty()) {
        // Recurse on initial buffer of whitespace before processing
        // this call's input below.
        Write(buffer, message_handler);
      }
    } else {
      // Don't know whether HTML or not --- wait for more data.
      return true;
    }
  }

  bool ret = true;
  if (started_parse_) {
    // Buffer up all text & flushes until our worker-thread gets a chance
    // to run. Also split up HTML into manageable chunks if we get a burst,
    // as it will make it easier to insert flushes in between them in
    // ExecuteQueued(), which we want to do in order to limit memory use and
    // latency.
    size_t chunk_size = Options()->flush_buffer_limit_bytes();
    StringStarVector chunks;
    for (size_t pos = 0; pos < str.size(); pos += chunk_size) {
      GoogleString* buffer =
          new GoogleString(str.data() + pos,
                           std::min(chunk_size, str.size() - pos));
      chunks.push_back(buffer);
    }

    {
      ScopedMutex lock(mutex_.get());
      for (int c = 0, n = chunks.size(); c < n; ++c) {
        text_queue_.push_back(chunks[c]);
      }
      ScheduleQueueExecutionIfNeeded();
    }
  } else {
    // Pass other data (css, js, images) directly to http writer.
    ret = base_fetch()->Write(str, message_handler);
  }
  return ret;
}

bool ProxyFetch::HandleFlush(MessageHandler* message_handler) {
  // TODO(jmarantz): check if the server is being shut down and punt.

  if (claims_html_ && !html_detector_.already_decided()) {
    return true;
  }

  bool ret = true;
  if (started_parse_) {
    // Buffer up Flushes for handling in our QueuedWorkerPool::Sequence
    // in ExecuteQueued.  Note that this can re-order Flushes behind
    // pending text, and aggregate together multiple flushes received from
    // the network into one.
    if (Options()->flush_html()) {
      ScopedMutex lock(mutex_.get());
      network_flush_outstanding_ = true;
      ScheduleQueueExecutionIfNeeded();
    }
  } else {
    ret = base_fetch()->Flush(message_handler);
  }
  return ret;
}

void ProxyFetch::HandleDone(bool success) {
  // TODO(jmarantz): check if the server is being shut down and punt,
  // possibly by calling Finish(false).
  if (original_content_fetch_ != NULL) {
    original_content_fetch_->Done(success);
    // Null the pointer since original_content_fetch_ is not guaranteed to exist
    // beyond this point.
    original_content_fetch_ = NULL;
  }

  bool finish = true;

  if (success) {
    if (claims_html_ && !html_detector_.already_decided()) {
      // This is an all-whitespace document, so we couldn't figure out
      // if it's HTML or not. Handle as pass-through.
      html_detector_.ForceDecision(false /* not html */);
      GoogleString buffered;
      html_detector_.ReleaseBuffered(&buffered);
      AddPagespeedHeader();
      base_fetch()->HeadersComplete();
      Write(buffered, server_context_->message_handler());
    }
  } else if (!response_headers()->headers_complete()) {
    // This is a fetcher failure, like connection refused, not just an error
    // status code.
    response_headers()->SetStatusAndReason(HttpStatus::kNotFound);
  }

  VLOG(1) << "Fetch result:" << success << " " << url_
          << " : " << response_headers()->status_code();
  if (started_parse_) {
    ScopedMutex lock(mutex_.get());
    done_outstanding_ = true;
    done_result_ = success;
    ScheduleQueueExecutionIfNeeded();
    finish = false;
  }

  if (finish) {
    Finish(success);
  }
}

bool ProxyFetch::IsCachedResultValid(const ResponseHeaders& headers) {
  return headers.IsDateLaterThan(Options()->cache_invalidation_timestamp()) &&
      Options()->IsUrlCacheValid(url_, headers.date_ms());
}

void ProxyFetch::FlushDone() {
  ScopedMutex lock(mutex_.get());
  DCHECK(waiting_for_flush_to_finish_);
  waiting_for_flush_to_finish_ = false;

  if (!text_queue_.empty() || network_flush_outstanding_ || done_outstanding_) {
    ScheduleQueueExecutionIfNeeded();
  }
}

void ProxyFetch::ExecuteQueued() {
  bool do_flush = false;
  bool do_finish = false;
  bool done_result = false;
  bool force_flush = false;

  size_t buffer_limit = Options()->flush_buffer_limit_bytes();
  StringStarVector v;
  {
    ScopedMutex lock(mutex_.get());
    DCHECK(!waiting_for_flush_to_finish_);

    // See if we should force a flush based on how much stuff has
    // accumulated.
    size_t total = 0;
    size_t force_flush_chunk_count = 0;  // set only if force_flush is true.
    for (size_t c = 0, n = text_queue_.size(); c < n; ++c) {
      total += text_queue_[c]->length();
      if (total >= buffer_limit) {
        force_flush = true;
        force_flush_chunk_count = c + 1;
        break;
      }
    }

    // Are we forcing a flush of some, but not all, of the queued
    // content?
    bool partial_forced_flush =
        force_flush && (force_flush_chunk_count != text_queue_.size());
    if (partial_forced_flush) {
      for (size_t c = 0; c < force_flush_chunk_count; ++c) {
        v.push_back(text_queue_[c]);
      }
      size_t old_len = text_queue_.size();
      text_queue_.erase(text_queue_.begin(),
                        text_queue_.begin() + force_flush_chunk_count);
      DCHECK_EQ(old_len, v.size() + text_queue_.size());

      // Note that in this case, since text_queue_ isn't empty,
      // the call to ScheduleQueueExecutionIfNeeded from FlushDone
      // will make us run again.
    } else {
      v.swap(text_queue_);
    }
    do_flush = network_flush_outstanding_ || force_flush;
    do_finish = done_outstanding_;
    done_result = done_result_;

    network_flush_outstanding_ = false;

    // Note that we don't clear done_outstanding_ here yet, as we
    // can only handle it if we are not also handling a flush.
    queue_run_job_created_ = false;
    if (do_flush) {
      // Stop queuing up invocations of us until the flush we will do
      // below is done.
      waiting_for_flush_to_finish_ = true;
    }
  }

  if (!parse_text_called_) {
    DCHECK(request_context().get() != NULL);
    ScopedMutex lock(log_record()->mutex());
    TimingInfo* timing_info =
        log_record()->logging_info()->mutable_timing_info();
    if (timing_info->has_request_start_ms()) {
      timing_info->set_time_to_start_parse_ms(
          server_context_->timer()->NowMs() -
              timing_info->request_start_ms());
    }
    parse_text_called_ = true;
  }

  // Collect all text received from the fetcher
  for (int i = 0, n = v.size(); i < n; ++i) {
    GoogleString* str = v[i];
    driver_->ParseText(*str);
    delete str;
  }
  if (do_flush) {
    if (force_flush) {
      driver_->RequestFlush();
    }
    if (driver_->flush_requested()) {
      // A flush is about to happen, so we don't want to redundantly
      // flush due to idleness.
      CancelIdleAlarm();
    } else {
      // We will not actually flush, just run through the state-machine, so
      // we want to just advance the idleness timeout.
      QueueIdleAlarm();
    }
    driver_->ExecuteFlushIfRequestedAsync(
        MakeFunction(this, &ProxyFetch::FlushDone));
  } else if (do_finish) {
    CancelIdleAlarm();
    Finish(done_result);
  } else {
    // Advance timeout.
    QueueIdleAlarm();
  }
}


void ProxyFetch::Finish(bool success) {
  ProxyFetchPropertyCallbackCollector* detach_callback = NULL;
  {
    ScopedMutex lock(mutex_.get());
    DCHECK(!waiting_for_flush_to_finish_);
    done_outstanding_ = false;
    finishing_ = true;

    // Avoid holding two locks (this->mutex_ + property_cache_callback_->mutex_)
    // by copying the pointer and detaching after unlocking this->mutex_.
    detach_callback = property_cache_callback_;
    property_cache_callback_ = NULL;
  }
  // The only way detach_callback can be non-NULL here is if the resource isn't
  // being parsed (it's not HTML) and the collector hasn't finished yet, but in
  // that case we never attached the collector to us, so when it's done it won't
  // access us, which is good since we self-delete at the end of this method.
  if (detach_callback != NULL) {
    // Set the status code only for html responses or errors in property cache.
    bool is_response_ok = response_headers()->status_code() == HttpStatus::kOK;
    bool not_html = html_detector_.already_decided() &&
        !html_detector_.probable_html();
    HttpStatus::Code status_code = HttpStatus::kUnknownStatusCode;
    if (!is_response_ok || (claims_html_ && !not_html)) {
      status_code = static_cast<HttpStatus::Code>(
          response_headers()->status_code());
    }
    detach_callback->Detach(status_code);
  }

  if (driver_ != NULL) {
    if (started_parse_) {
      driver_->FinishParseAsync(
        MakeFunction(this, &ProxyFetch::CompleteFinishParse, success));
      return;

    } else {
      // In the unlikely case that StartParse fails (invalid URL?) or the
      // resource is not HTML, we must manually mark the driver for cleanup.
      driver_->Cleanup();
      driver_ = NULL;
    }
  }

  base_fetch()->Done(success);
  done_called_ = true;
  factory_->RegisterFinishedFetch(this);

  // In ProxyInterfaceTest.HeadersSetupRace, raise a signal that
  // indicates the test functionality is complete.  In other contexts
  // this is a no-op.
  ThreadSynchronizer* sync = server_context_->thread_synchronizer();
  delete this;
  sync->Signal(kHeadersSetupRaceDone);
}

void ProxyFetch::CompleteFinishParse(bool success) {
  driver_ = NULL;
  // Have to call directly -- sequence is gone with driver.
  Finish(success);
}

void ProxyFetch::CancelIdleAlarm() {
  if (idle_alarm_ != NULL) {
    idle_alarm_->CancelAlarm();
    idle_alarm_ = NULL;
  }
}

void ProxyFetch::QueueIdleAlarm() {
  const RewriteOptions* options = Options();
  if (!options->flush_html() || (options->idle_flush_time_ms() <= 0)) {
    return;
  }

  CancelIdleAlarm();
  idle_alarm_ = new QueuedAlarm(
      driver_->scheduler(), sequence_,
      timer_->NowUs() + Options()->idle_flush_time_ms() * Timer::kMsUs,
      MakeFunction(this, &ProxyFetch::HandleIdleAlarm));

  // In ProxyInterfaceTest.HeadersSetupRace, raise a signal that
  // indicates the idle-callback has initiated.  In other contexts
  // this is a no-op.
  ThreadSynchronizer* sync = server_context_->thread_synchronizer();
  sync->Signal(kHeadersSetupRaceAlarmQueued);
}

void ProxyFetch::HandleIdleAlarm() {
  // Clear references to the alarm object as it will be deleted once we exit.
  idle_alarm_ = NULL;

  if (waiting_for_flush_to_finish_ || done_outstanding_ || finishing_) {
    return;
  }

  // Inject an own flush, and queue up its dispatch.
  driver_->ShowProgress("- Flush injected due to input idleness -");
  driver_->RequestFlush();
  Flush(factory_->message_handler());
}

}  // namespace net_instaweb
