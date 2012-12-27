/*
 * Copyright 2010 Google Inc.
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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/rewriter/public/server_context.h"

#include <algorithm>                   // for std::binary_search
#include <cstddef>                     // for size_t
#include <set>

#include "base/logging.h"               // for operator<<, etc
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/beacon_critical_images_finder.h"
#include "net/instaweb/rewriter/public/blink_critical_line_data_finder.h"
#include "net/instaweb/rewriter/public/critical_images_finder.h"
#include "net/instaweb/rewriter/public/flush_early_info_finder.h"
#include "net/instaweb/rewriter/public/furious_matcher.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_namer.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_driver_pool.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/basictypes.h"        // for int64
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/client_state.h"
#include "net/instaweb/util/public/dynamic_annotations.h"  // RunningOnValgrind
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/md5_hasher.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/named_lock_manager.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/query_params.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/stl_util.h"          // for STLDeleteElements
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_synchronizer.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/public/url_to_filename_encoder.h"
#include "net/instaweb/util/public/writer.h"

namespace net_instaweb {

class AbstractClientState;
class RewriteFilter;

namespace {

const int64 kRefreshExpirePercent = 80;

// Attributes that should not be automatically copied from inputs to outputs
const char* kExcludedAttributes[] = {
  HttpAttributes::kCacheControl,
  HttpAttributes::kContentEncoding,
  HttpAttributes::kContentLength,
  HttpAttributes::kContentType,
  HttpAttributes::kDate,
  HttpAttributes::kEtag,
  HttpAttributes::kExpires,
  HttpAttributes::kLastModified,
  // Rewritten resources are publicly cached, so we should avoid cookies
  // which are generally meant for private data.
  HttpAttributes::kSetCookie,
  HttpAttributes::kSetCookie2,
  HttpAttributes::kTransferEncoding,
  HttpAttributes::kVary
};

// Track a property cache lookup triggered from a beacon response. When
// complete, Done will update and and writeback the beacon cohort with the
// critical image set.
class BeaconPropertyCallback : public PropertyPage {
 public:
  BeaconPropertyCallback(
      ServerContext* server_context,
      const StringPiece& key,
      StringSet* html_critical_images_set,
      StringSet* css_critical_images_set)
      : PropertyPage(server_context->thread_system()->NewMutex(), key),
        server_context_(server_context),
        html_critical_images_set_(html_critical_images_set),
        css_critical_images_set_(css_critical_images_set) {
  }


  virtual ~BeaconPropertyCallback() {}

  virtual void Done(bool success) {
    const PropertyCache::Cohort* cohort =
        server_context_->page_property_cache()->GetCohort(
            BeaconCriticalImagesFinder::kBeaconCohort);
    server_context_->critical_images_finder()->UpdateCriticalImagesCacheEntry(
        this, server_context_->page_property_cache(),
        html_critical_images_set_, css_critical_images_set_);
    server_context_->page_property_cache()->WriteCohort(cohort, this);
    delete this;
  }

 private:
  ServerContext* server_context_;
  StringSet* html_critical_images_set_;
  StringSet* css_critical_images_set_;
  DISALLOW_COPY_AND_ASSIGN(BeaconPropertyCallback);
};

}  // namespace

const int64 ServerContext::kGeneratedMaxAgeMs = Timer::kYearMs;

// Statistics group names.
const char ServerContext::kStatisticsGroup[] = "Statistics";

// Our HTTP cache mostly stores full URLs, including the http: prefix,
// mapping them into the URL contents and HTTP headers.  However, we
// also put name->hash mappings into the HTTP cache, and we prefix
// these with "ResourceName:" to disambiguate them.
//
// Cache entries prefixed this way map the base name of a resource
// into the hash-code of the contents.  This mapping has a TTL based
// on the minimum TTL of the input resources used to construct the
// resource.  After that TTL has expired, we will need to re-fetch the
// resources from their origin, and recompute the hash.
//
// Whenever we change the hashing function we can bust caches by
// changing this prefix.
//
// TODO(jmarantz): inject the SVN version number here to automatically bust
// caches whenever pagespeed is upgraded.
const char ServerContext::kCacheKeyResourceNamePrefix[] = "rname/";

// We set etags for our output resources to "W/0".  The "W" means
// that this etag indicates a functional consistency, but is not
// guaranteeing byte-consistency.  This distinction is important because
// we serve different bytes for clients that do not accept gzip.
//
// This value is a shared constant so that it can also be used in
// the Apache-specific code that repairs headers after mod_headers
// alters them.
const char ServerContext::kResourceEtagValue[] = "W/\"0\"";

class ResourceManagerHttpCallback : public OptionsAwareHTTPCacheCallback {
 public:
  ResourceManagerHttpCallback(
      Resource::NotCacheablePolicy not_cacheable_policy,
      Resource::AsyncCallback* resource_callback,
      ServerContext* resource_manager,
      const RequestContextPtr& request_context);
  virtual ~ResourceManagerHttpCallback();
  virtual void Done(HTTPCache::FindResult find_result);

 private:
  Resource::AsyncCallback* resource_callback_;
  ServerContext* server_context_;
  Resource::NotCacheablePolicy not_cacheable_policy_;
  DISALLOW_COPY_AND_ASSIGN(ResourceManagerHttpCallback);
};

class GlobalOptionsRewriteDriverPool : public RewriteDriverPool {
 public:
  explicit GlobalOptionsRewriteDriverPool(ServerContext* context)
      : server_context_(context) {
  }

  virtual RewriteOptions* TargetOptions() const {
    return server_context_->global_options();
  }

 private:
  ServerContext* server_context_;
};

ServerContext::ServerContext(RewriteDriverFactory* factory)
    : thread_system_(factory->thread_system()),
      rewrite_stats_(NULL),
      file_system_(factory->file_system()),
      filename_encoder_(NULL),
      url_namer_(NULL),
      user_agent_matcher_(NULL),
      scheduler_(factory->scheduler()),
      default_system_fetcher_(NULL),
      hasher_(NULL),
      blink_critical_line_data_finder_(NULL),
      lock_hasher_(20),
      contents_hasher_(21),
      statistics_(NULL),
      store_outputs_in_file_system_(false),
      response_headers_finalized_(true),
      enable_property_cache_(true),
      lock_manager_(NULL),
      message_handler_(NULL),
      available_rewrite_drivers_(new GlobalOptionsRewriteDriverPool(this)),
      trying_to_cleanup_rewrite_drivers_(false),
      factory_(factory),
      rewrite_drivers_mutex_(thread_system_->NewMutex()),
      html_workers_(NULL),
      rewrite_workers_(NULL),
      low_priority_rewrite_workers_(NULL),
      static_javascript_manager_(NULL),
      thread_synchronizer_(new ThreadSynchronizer(thread_system_)),
      furious_matcher_(factory_->NewFuriousMatcher()),
      usage_data_reporter_(factory_->usage_data_reporter()) {
  // Make sure the excluded-attributes are in abc order so binary_search works.
  // Make sure to use the same comparator that we pass to the binary_search.
#ifndef NDEBUG
  for (int i = 1, n = arraysize(kExcludedAttributes); i < n; ++i) {
    DCHECK(CharStarCompareInsensitive()(kExcludedAttributes[i - 1],
                                        kExcludedAttributes[i]));
  }
#endif
}

ServerContext::~ServerContext() {
  {
    ScopedMutex lock(rewrite_drivers_mutex_.get());

    // Actually release anything that got deferred above.
    trying_to_cleanup_rewrite_drivers_ = false;
    for (RewriteDriverSet::iterator i =
             deferred_release_rewrite_drivers_.begin();
         i != deferred_release_rewrite_drivers_.end(); ++i) {
      ReleaseRewriteDriverImpl(*i);
    }
    deferred_release_rewrite_drivers_.clear();
  }

  // We scan for "leaked_rewrite_drivers" in apache/install/Makefile.tests
  if (!active_rewrite_drivers_.empty()) {
    message_handler_->Message(
        kError, "ServerContext: %d leaked_rewrite_drivers on destruction",
        static_cast<int>(active_rewrite_drivers_.size()));
  }
  STLDeleteElements(&active_rewrite_drivers_);
  available_rewrite_drivers_.reset();
  STLDeleteElements(&additional_driver_pools_);
  decoding_driver_.reset(NULL);
}

void ServerContext::InitWorkersAndDecodingDriver() {
  html_workers_ = factory_->WorkerPool(
      RewriteDriverFactory::kHtmlWorkers);
  rewrite_workers_ = factory_->WorkerPool(
      RewriteDriverFactory::kRewriteWorkers);
  low_priority_rewrite_workers_ = factory_->WorkerPool(
      RewriteDriverFactory::kLowPriorityRewriteWorkers);
  decoding_driver_.reset(NewUnmanagedRewriteDriver(
      NULL, global_options()->Clone(), RequestContextPtr(NULL)));
  // Apply platform configuration mutation for consistency's sake.
  factory_->ApplyPlatformSpecificConfiguration(decoding_driver_.get());
  // Inserts platform-specific rewriters into the resource_filter_map_, so that
  // the decoding process can recognize those rewriter ids.
  factory_->AddPlatformSpecificDecodingPasses(decoding_driver_.get());
  // This call is for backwards compatibility.  When adding new platform
  // specific rewriters to implementations of RewriteDriverFactory, please
  // do not rely on this call to include them in the decoding process.  Instead,
  // add them to your implementation of AddPlatformSpecificDecodingPasses.
  factory_->AddPlatformSpecificRewritePasses(decoding_driver_.get());
}

// TODO(jmarantz): consider moving this method to ResponseHeaders
void ServerContext::SetDefaultLongCacheHeadersWithCharset(
    const ContentType* content_type, StringPiece charset,
    ResponseHeaders* header) const {
  header->set_major_version(1);
  header->set_minor_version(1);
  header->SetStatusAndReason(HttpStatus::kOK);

  header->RemoveAll(HttpAttributes::kContentType);
  if (content_type != NULL) {
    GoogleString header_val = content_type->mime_type();
    if (!charset.empty()) {
      // Note: if charset was quoted, content_type's parsing would not unquote
      // it, so here we just append it back in instead of quoting it again.
      StrAppend(&header_val, "; charset=", charset);
    }
    header->Add(HttpAttributes::kContentType, header_val);
  }

  int64 now_ms = http_cache_->timer()->NowMs();
  header->SetDateAndCaching(now_ms, kGeneratedMaxAgeMs);

  // While PageSpeed claims the "Vary" header is needed to avoid proxy cache
  // issues for clients where some accept gzipped content and some don't, it
  // should not be done here.  It should instead be done by whatever code is
  // conditionally gzipping the content based on user-agent, e.g. mod_deflate.
  // header->Add(HttpAttributes::kVary, HttpAttributes::kAcceptEncoding);

  // ETag is superfluous for mod_pagespeed as we sign the URL with the
  // content hash.  However, we have seen evidence that IE8 will not
  // serve images from its cache when the image lacks an ETag.  Since
  // we sign URLs, there is no reason to have a unique signature in
  // the ETag.
  header->Replace(HttpAttributes::kEtag, kResourceEtagValue);

  // TODO(jmarantz): Replace last-modified headers by default?
  ConstStringStarVector v;
  if (!header->Lookup(HttpAttributes::kLastModified, &v)) {
    header->SetLastModified(now_ms);
  }

  // TODO(jmarantz): Page-speed suggested adding a "Last-Modified" header
  // for cache validation.  To do this we must track the max of all
  // Last-Modified values for all input resources that are used to
  // create this output resource.  For now we are using the current
  // time.

  header->ComputeCaching();
}

void ServerContext::MergeNonCachingResponseHeaders(
    const ResponseHeaders& input_headers,
    ResponseHeaders* output_headers) {
  for (int i = 0, n = input_headers.NumAttributes(); i < n; ++i) {
    const GoogleString& name = input_headers.Name(i);
    if (!IsExcludedAttribute(name.c_str())) {
      output_headers->Add(name, input_headers.Value(i));
    }
  }
}

// TODO(jmarantz): consider moving this method to ResponseHeaders
void ServerContext::SetContentType(const ContentType* content_type,
                                     ResponseHeaders* header) {
  CHECK(content_type != NULL);
  header->Replace(HttpAttributes::kContentType, content_type->mime_type());
  header->ComputeCaching();
}

void ServerContext::set_filename_prefix(const StringPiece& file_prefix) {
  file_prefix.CopyToString(&file_prefix_);
}

bool ServerContext::Write(const ResourceVector& inputs,
                          const StringPiece& contents,
                          const ContentType* type,
                          StringPiece charset,
                          OutputResource* output,
                          MessageHandler* handler) {
  output->SetType(type);
  output->set_charset(charset);
  ResponseHeaders* meta_data = output->response_headers();
  SetDefaultLongCacheHeadersWithCharset(type, charset, meta_data);
  meta_data->SetStatusAndReason(HttpStatus::kOK);
  ApplyInputCacheControl(inputs, meta_data);
  AddOriginalContentLengthHeader(inputs, meta_data);

  // The URL for any resource we will write includes the hash of contents,
  // so it can can live, essentially, forever. So compute this hash,
  // and cache the output using meta_data's default headers which are to cache
  // forever.
  Writer* writer = output->BeginWrite(handler);
  bool ret = (writer != NULL);
  if (ret) {
    ret = writer->Write(contents, handler);
    output->EndWrite(handler);

    if (output->kind() != kOnTheFlyResource &&
        (http_cache_->force_caching() || meta_data->IsProxyCacheable())) {
      // This URL should already be mapped to the canonical rewrite domain,
      // But we should store its unsharded form in the cache.
      http_cache_->Put(output->HttpCacheKey(), &output->value_, handler);
    }

    // If we're asked to, also save a debug dump
    if (store_outputs_in_file_system_) {
      output->DumpToDisk(handler);
    }

    // If our URL is derived from some pre-existing URL (and not invented by
    // us due to something like outlining), cache the mapping from original URL
    // to the constructed one.
    if (output->kind() != kOutlinedResource) {
      CachedResult* cached = output->EnsureCachedResultCreated();
      cached->set_optimizable(true);
      cached->set_url(output->url());  // Note: output->url() will be sharded.
    }
  } else {
    // Note that we've already gotten a "could not open file" message;
    // this just serves to explain why and suggest a remedy.
    handler->Message(kInfo, "Could not create output resource"
                     " (bad filename prefix '%s'?)",
                     file_prefix_.c_str());
  }
  return ret;
}

void ServerContext::ApplyInputCacheControl(const ResourceVector& inputs,
                                           ResponseHeaders* headers) {
  headers->ComputeCaching();
  bool proxy_cacheable = headers->IsProxyCacheable();
  bool cacheable = headers->IsCacheable();
  bool no_store = headers->HasValue(HttpAttributes::kCacheControl,
                                    "no-store");
  int64 max_age = headers->cache_ttl_ms();
  for (int i = 0, n = inputs.size(); i < n; ++i) {
    const ResourcePtr& input_resource(inputs[i]);
    if (input_resource.get() != NULL && input_resource->HttpStatusOk()) {
      ResponseHeaders* input_headers = input_resource->response_headers();
      input_headers->ComputeCaching();
      if (input_headers->cache_ttl_ms() < max_age) {
        max_age = input_headers->cache_ttl_ms();
      }
      proxy_cacheable &= input_headers->IsProxyCacheable();
      cacheable &= input_headers->IsCacheable();
      no_store |= input_headers->HasValue(HttpAttributes::kCacheControl,
                                          "no-store");
    }
  }
  if (cacheable) {
    if (proxy_cacheable) {
      return;
    } else {
      headers->SetDateAndCaching(headers->date_ms(), max_age, ",private");
    }
  } else {
    GoogleString directives = ",no-cache";
    if (no_store) {
      directives += ",no-store";
    }
    headers->SetDateAndCaching(headers->date_ms(), 0, directives);
  }
  headers->ComputeCaching();
}

void ServerContext::AddOriginalContentLengthHeader(
    const ResourceVector& inputs, ResponseHeaders* headers) {
  // Determine the total original content length for input resource, and
  // use this to set the X-Original-Content-Length header in the output.
  int64 input_size = 0;
  for (int i = 0, n = inputs.size(); i < n; ++i) {
    const ResourcePtr& input_resource(inputs[i]);
    ResponseHeaders* input_headers = input_resource->response_headers();
    const char* original_content_length_header = input_headers->Lookup1(
        HttpAttributes::kXOriginalContentLength);
    int64 original_content_length;
    if (original_content_length_header != NULL &&
        StringToInt64(original_content_length_header,
                      &original_content_length)) {
      input_size += original_content_length;
    }
  }
  // Only add the header if there were actual input resources with
  // known sizes involved (which is not always the case, e.g., in tests where
  // synthetic input resources are used).
  if (input_size > 0) {
    headers->SetOriginalContentLength(input_size);
  }
}

bool ServerContext::IsPagespeedResource(const GoogleUrl& url) {
  // Various things URL decoding produces which we ignore here.
  ResourceNamer namer;
  OutputResourceKind kind;
  RewriteFilter* filter;
  return decoding_driver_->DecodeOutputResourceName(url, &namer, &kind,
                                                    &filter);
}

bool ServerContext::IsImminentlyExpiring(int64 start_date_ms,
                                           int64 expire_ms) const {
  // Consider a resource with 5 minute expiration time (the default
  // assumed by mod_pagespeed when a potentialy cacheable resource
  // lacks a cache control header, which happens a lot).  If the
  // origin TTL was 5 minutes and 4 minutes have expired, then we want
  // to re-fetch it so that we can avoid expiring the data.
  //
  // If we don't do this, then every 5 minutes, someone will see
  // this page unoptimized.  In a site with very low QPS, including
  // test instances of a site, this can happen quite often.
  int64 now_ms = timer()->NowMs();
  int64 ttl_ms = expire_ms - start_date_ms;
  // Only proactively refresh resources that have at least our
  // default expiration of 5 minutes.
  //
  // TODO(jmaessen): Lower threshold when If-Modified-Since checking is in
  // place; consider making this settable.
  // TODO(pradnya): We will freshen only if ttl is greater than the default
  // implicit ttl. If the implicit ttl has been overridden by a site, we will
  // not honor it here. Fix that.
  if (ttl_ms >= ResponseHeaders::kImplicitCacheTtlMs) {
    int64 freshen_threshold = std::min(
        ResponseHeaders::kImplicitCacheTtlMs,
        ((100 - kRefreshExpirePercent) * ttl_ms) / 100);
    if (expire_ms - now_ms < freshen_threshold) {
      return true;
    }
  }
  return false;
}

void ServerContext::RefreshIfImminentlyExpiring(
    Resource* resource, MessageHandler* handler) const {
  if (!http_cache_->force_caching() && resource->IsCacheableTypeOfResource()) {
    const ResponseHeaders* headers = resource->response_headers();
    int64 start_date_ms = headers->date_ms();
    int64 expire_ms = headers->CacheExpirationTimeMs();
    if (IsImminentlyExpiring(start_date_ms, expire_ms)) {
      resource->Freshen(NULL, handler);
    }
  }
}

ResourceManagerHttpCallback::ResourceManagerHttpCallback(
    Resource::NotCacheablePolicy not_cacheable_policy,
    Resource::AsyncCallback* resource_callback,
    ServerContext* resource_manager,
    const RequestContextPtr& request_context)
    : OptionsAwareHTTPCacheCallback(
          resource_callback->resource()->rewrite_options(),
          request_context),
      resource_callback_(resource_callback),
      server_context_(resource_manager),
      not_cacheable_policy_(not_cacheable_policy) {
}

ResourceManagerHttpCallback::~ResourceManagerHttpCallback() {
}

void ResourceManagerHttpCallback::Done(HTTPCache::FindResult find_result) {
  ResourcePtr resource(resource_callback_->resource());
  MessageHandler* handler = server_context_->message_handler();

  // Note, we pass lock_failure==false to the resource callbacks
  // when we are taking action based on the cache.  We haven't locked,
  // but we didn't fail-to-lock.  Resource callbacks need to know if
  // the lock failed, because they will delete expired cache metadata
  // if they have the lock, or if the lock was not needed, but they
  // should not delete it if they fail to lock.
  switch (find_result) {
    case HTTPCache::kFound:
      resource->Link(http_value(), handler);
      resource->response_headers()->CopyFrom(*response_headers());
      resource->DetermineContentType();
      server_context_->RefreshIfImminentlyExpiring(resource.get(), handler);
      resource_callback_->Done(false /* lock_failure */,
                               true /* resource_ok */);
      break;
    case HTTPCache::kRecentFetchFailed:
      // TODO(jmarantz): in this path, should we try to fetch again
      // sooner than 5 minutes?  The issue is that in this path we are
      // serving for the user, not for a rewrite.  This could get
      // frustrating, even if the software is functioning as intended,
      // because a missing resource that is put in place by a site
      // admin will not be checked again for 5 minutes.
      //
      // The "good" news is that if the admin is willing to crank up
      // logging to 'info' then http_cache.cc will log the
      // 'remembered' failure.
      resource_callback_->Done(false /* lock_failure */,
                               false /* resource_ok */);
      break;
    case HTTPCache::kRecentFetchNotCacheable:
      switch (not_cacheable_policy_) {
        case Resource::kLoadEvenIfNotCacheable:
          resource->LoadAndCallback(not_cacheable_policy_,
                                    resource_callback_, handler);
          break;
        case Resource::kReportFailureIfNotCacheable:
          resource_callback_->Done(false /* lock_failure */,
                                   false /* resource_ok */);
          break;
        default:
          LOG(DFATAL) << "Unexpected not_cacheable_policy_!";
          resource_callback_->Done(false /* lock_failure */,
                                   false /* resource_ok */);
          break;
      }
      break;
    case HTTPCache::kNotFound:
      // If not, load it asynchronously.
      // Link the fallback value which can be used if the fetch fails.
      resource->LinkFallbackValue(fallback_http_value());
      resource->LoadAndCallback(not_cacheable_policy_,
                                resource_callback_, handler);
      break;
  }
  delete this;
}

// TODO(sligocki): Move into Resource? This would allow us to treat
// file- and URL-based resources differently as far as cacheability, etc.
// Specifically, we are now making a cache request for file-based resources
// which will always fail, for FileInputResources, we should just Load them.
// TODO(morlovich): Should this load non-cacheable + non-loaded resources?
void ServerContext::ReadAsync(
    Resource::NotCacheablePolicy not_cacheable_policy,
    const RequestContextPtr& request_context,
    Resource::AsyncCallback* callback) {
  // If the resource is not already loaded, and this type of resource (e.g.
  // URL vs File vs Data) is cacheable, then try to load it.
  ResourcePtr resource = callback->resource();
  if (resource->loaded()) {
    RefreshIfImminentlyExpiring(resource.get(), message_handler_);
    callback->Done(false /* lock_failure */, true /* resource_ok */);
  } else if (resource->IsCacheableTypeOfResource()) {
    ResourceManagerHttpCallback* resource_manager_callback =
        new ResourceManagerHttpCallback(not_cacheable_policy,
                                        callback,
                                        this,
                                        request_context);
    http_cache_->Find(resource->url(), message_handler_,
                      resource_manager_callback);
  }
}

NamedLock* ServerContext::MakeCreationLock(const GoogleString& name) {
  const char kLockSuffix[] = ".outputlock";

  GoogleString lock_name = StrCat(lock_hasher_.Hash(name), kLockSuffix);
  return lock_manager_->CreateNamedLock(lock_name);
}

NamedLock* ServerContext::MakeInputLock(const GoogleString& name) {
  const char kLockSuffix[] = ".lock";

  GoogleString lock_name = StrCat(lock_hasher_.Hash(name), kLockSuffix);
  return lock_manager_->CreateNamedLock(lock_name);
}

namespace {
// Constants governing resource lock timeouts.
// TODO(jmaessen): Set more appropriately?
const int64 kBreakLockMs = 30 * Timer::kSecondMs;
const int64 kBlockLockMs = 5 * Timer::kSecondMs;
}  // namespace

bool ServerContext::TryLockForCreation(NamedLock* creation_lock) {
  return creation_lock->TryLockStealOld(kBreakLockMs);
}

void ServerContext::LockForCreation(NamedLock* creation_lock,
                                      QueuedWorkerPool::Sequence* worker,
                                      Function* callback) {
  // TODO(jmaessen): It occurs to me that we probably ought to be
  // doing something like this if we *really* care about lock aging:
  // if (!creation_lock->LockTimedWaitStealOld(kBlockLockMs,
  //                                           kBreakLockMs)) {
  //   creation_lock->TryLockStealOld(0);  // Force lock steal
  // }
  // This updates the lock hold time so that another thread is less likely
  // to steal the lock while we're doing the blocking rewrite.
  creation_lock->LockTimedWaitStealOld(
      kBlockLockMs, kBreakLockMs,
      new QueuedWorkerPool::Sequence::AddFunction(worker, callback));
}

bool ServerContext::HandleBeacon(const StringPiece& unparsed_url) {
  // The url HandleBeacon recieves is a relative url, so adding some dummy
  // host to make it complete url so that i can use GoogleUrl for parsing.
  GoogleUrl base("http://www.example.com");
  GoogleUrl url(base, unparsed_url);

  if (!url.is_valid() || !url.has_query()) {
    message_handler_->Message(kWarning, "Invalid beacon url: %s",
                              unparsed_url.as_string().c_str());
    return false;
  }

  // Beacon urls are of the form http://a.com/xyz/beacon?ets=load:xxx&url=....
  // Extract the URL. A valid URL query param is required to attempt parsing of
  // the ets and critimg query params. However, an invalid ets or critimg param
  // will not prevent attempting to parsing of the other. This is because these
  // values are generated by separate client-side JS and that failure of one
  // should not prevent attempting to parse the other.
  QueryParams query_params;
  query_params.Parse(url.Query());
  ConstStringStarVector param_values;
  GoogleUrl url_query_param;

  if (query_params.Lookup("url", &param_values) && param_values.size() == 1 &&
      param_values[0] != NULL) {
    // The url query param returned by the beacon is encoded with
    // EncodeURIComponent, so decode it.
    url_query_param.Reset(UrlToFilenameEncoder::Unescape(*param_values[0]));

    if (!url_query_param.is_valid()) {
      message_handler_->Message(kWarning,
                                "Invalid URL query param in beacon: %s",
                                param_values[0]->c_str());
      return false;
    }
  } else {
    message_handler_->Message(kWarning, "Missing URL query param in beacon: %s",
                              unparsed_url.as_string().c_str());
    return false;
  }

  bool success = true;

  // Extract the onload time from the ets query param.
  param_values.clear();
  if (query_params.Lookup("ets", &param_values) && param_values.size() == 1 &&
      param_values[0] != NULL) {
    int value = -1;

    StringPiece param_value_str(*param_values[0]);
    size_t index = param_value_str.find(":");
    if (index < param_value_str.size()) {
      StringPiece load_time_str = param_value_str.substr(index + 1);
      StringToInt(load_time_str.as_string(), &value);
    }
    if (value < 0) {
      success = false;
    } else {
      rewrite_stats_->total_page_load_ms()->Add(value);
      rewrite_stats_->page_load_count()->Add(1);
      rewrite_stats_->beacon_timings_ms_histogram()->Add(value);
    }
  }

  // Extract critical image URLs
  param_values.clear();
  if (page_property_cache() != NULL && page_property_cache()->enabled() &&
      query_params.Lookup("critimg", &param_values) &&
      param_values.size() == 1 && param_values[0] != NULL) {
    // Beacon property callback takes ownership of both critical images sets.
    scoped_ptr<StringSet> html_critical_images_set(new StringSet);
    // TODO(jud): Add css critical image detection to the beacon.
    scoped_ptr<StringSet> css_critical_images_set(NULL);
    StringPieceVector crit_img_hashes;
    SplitStringPieceToVector(*param_values[0], ",", &crit_img_hashes, true);
    for (StringPieceVector::iterator i = crit_img_hashes.begin();
         i != crit_img_hashes.end(); ++i) {
      // Critical image URLs should be reported by the beacon as hashes, using a
      // hash function that matches HashString in util/public/string_hash.h.
      html_critical_images_set->insert(i->as_string());
    }

    // Store the critical image information in the property cache. This is done
    // by looking up the property page for the URL specified in the beacon, and
    // performing the page update and cohort write in
    // BeaconPropertyCallback::Done(). Done() is called when the read completes.
    scoped_ptr<BeaconPropertyCallback> property_callback(
        new BeaconPropertyCallback(
            this, url_query_param.AllExceptQuery(),
            html_critical_images_set.release(),
            css_critical_images_set.release()));
    page_property_cache()->Read(property_callback.release());
  }

  return success;
}

// TODO(jmaessen): Note that we *could* re-structure the
// rewrite_driver freelist code as follows: Keep a
// std::vector<RewriteDriver*> of all rewrite drivers.  Have each
// driver hold its index in the vector (as a number or iterator).
// Keep index of first in use.  To free, swap with first in use,
// adjusting indexes, and increment first in use.  To allocate,
// decrement first in use and return that driver.  If first in use was
// 0, allocate a fresh driver and push it.
//
// The benefit of Jan's idea is that we could avoid the overhead
// of keeping the RewriteDrivers in a std::set, which has log n
// insert/remove behavior, and instead get constant time and less
// memory overhead.

RewriteDriver* ServerContext::NewCustomRewriteDriver(
    RewriteOptions* options, const RequestContextPtr& request_ctx) {
  RewriteDriver* rewrite_driver = NewUnmanagedRewriteDriver(
      NULL /* no pool as custom*/,
      options,
      request_ctx);
  {
    ScopedMutex lock(rewrite_drivers_mutex_.get());
    active_rewrite_drivers_.insert(rewrite_driver);
  }
  if (factory_ != NULL) {
    factory_->ApplyPlatformSpecificConfiguration(rewrite_driver);
  }
  rewrite_driver->AddFilters();
  if (factory_ != NULL) {
    factory_->AddPlatformSpecificRewritePasses(rewrite_driver);
  }
  return rewrite_driver;
}

RewriteDriver* ServerContext::NewUnmanagedRewriteDriver(
    RewriteDriverPool* pool, RewriteOptions* options,
    const RequestContextPtr& request_ctx) {
  RewriteDriver* rewrite_driver = new RewriteDriver(
      message_handler_, file_system_, default_system_fetcher_);
  rewrite_driver->set_options_for_pool(pool, options);
  rewrite_driver->SetResourceManager(this);
  rewrite_driver->set_request_context(request_ctx);
  return rewrite_driver;
}

RewriteDriver* ServerContext::NewRewriteDriver(
    const RequestContextPtr& request_ctx) {
  return NewRewriteDriverFromPool(
      available_rewrite_drivers_.get(), request_ctx);
}

RewriteDriver* ServerContext::NewRewriteDriverFromPool(
    RewriteDriverPool* pool, const RequestContextPtr& request_ctx) {
  RewriteDriver* rewrite_driver = NULL;

  RewriteOptions* options = pool->TargetOptions();
  // Note that options->signature() takes a reader-lock so it's thread-safe
  // even if another thread is concurrently handling a cache-flush request.
  GoogleString expected_signature = options->signature();
  {
    ScopedMutex lock(rewrite_drivers_mutex_.get());
    while ((rewrite_driver = pool->PopDriver()) != NULL) {
      // Note: there is currently some activity to make the RewriteOptions
      // signature insensitive to changes that need not affect the metadata
      // cache key.  As we are dependent on a comprehensive signature in
      // order to correctly determine whether we can recycle a RewriteDriver,
      // we would have to use a separate signature for metadata_cache_key
      // vs this purpose.
      //
      // So for now, let us keep all the options incorporated into the
      // signature, and revisit the issue of pulling options out if we
      // find we are having poor hit-rate in the metadata cache during
      // operations.
      if (rewrite_driver->options()->signature() == expected_signature) {
        break;
      } else {
        delete rewrite_driver;
        rewrite_driver = NULL;
      }
    }
  }

  if (rewrite_driver == NULL) {
    rewrite_driver = NewUnmanagedRewriteDriver(
        pool, options->Clone(), request_ctx);
    if (factory_ != NULL) {
      factory_->ApplyPlatformSpecificConfiguration(rewrite_driver);
    }
    rewrite_driver->AddFilters();
    if (factory_ != NULL) {
      factory_->AddPlatformSpecificRewritePasses(rewrite_driver);
    }
  } else {
    rewrite_driver->set_request_context(request_ctx);
  }

  {
    ScopedMutex lock(rewrite_drivers_mutex_.get());
    active_rewrite_drivers_.insert(rewrite_driver);
  }
  return rewrite_driver;
}

void ServerContext::ReleaseRewriteDriver(RewriteDriver* rewrite_driver) {
  ScopedMutex lock(rewrite_drivers_mutex_.get());
  ReleaseRewriteDriverImpl(rewrite_driver);
}

void ServerContext::ReleaseRewriteDriverImpl(RewriteDriver* rewrite_driver) {
  if (trying_to_cleanup_rewrite_drivers_) {
    deferred_release_rewrite_drivers_.insert(rewrite_driver);
    return;
  }

  int count = active_rewrite_drivers_.erase(rewrite_driver);
  if (count != 1) {
    LOG(ERROR) << "ReleaseRewriteDriver called with driver not in active set.";
    DLOG(FATAL);
  } else {
    RewriteDriverPool* pool = rewrite_driver->controlling_pool();
    if (pool == NULL) {
      delete rewrite_driver;
    } else {
      pool->RecycleDriver(rewrite_driver);
    }
  }
}

void ServerContext::ShutDownDrivers() {
  // Try to get any outstanding rewrites to complete, one-by-one.
  {
    ScopedMutex lock(rewrite_drivers_mutex_.get());
    // Prevent any rewrite completions from directly deleting drivers or
    // affecting active_rewrite_drivers_. We can now release the lock so
    // that the rewrites can call ReleaseRewriteDriver. Note that this is
    // making an assumption that we're not allocating new rewrite drivers
    // during the shutdown.
    trying_to_cleanup_rewrite_drivers_ = true;
  }

  if (!active_rewrite_drivers_.empty()) {
    message_handler_->Message(kInfo, "%d rewrite(s) still ongoing at exit",
                              static_cast<int>(active_rewrite_drivers_.size()));
  }

  for (RewriteDriverSet::iterator i = active_rewrite_drivers_.begin();
       i != active_rewrite_drivers_.end(); ++i) {
    // Warning: the driver may already have been mostly cleaned up except for
    // not getting into ReleaseRewriteDriver before our lock acquisition at
    // the start of this function; this code is relying on redundant
    // BoundedWaitForCompletion and Cleanup being safe when
    // trying_to_cleanup_rewrite_drivers_ is true.
    // ResourceManagerTest.ShutDownAssumptions() exists to cover this scenario.
    RewriteDriver* active = *i;
    int64 timeout_ms = Timer::kSecondMs;
    if (RunningOnValgrind()) {
      timeout_ms *= 20;
    }
    active->BoundedWaitFor(RewriteDriver::kWaitForShutDown, timeout_ms);
    active->Cleanup();  // Note: only cleans up if the rewrites are complete.
    // TODO(jmarantz): rename RewriteDriver::Cleanup to CleanupIfDone.
  }
}

size_t ServerContext::num_active_rewrite_drivers() {
  ScopedMutex lock(rewrite_drivers_mutex_.get());
  return active_rewrite_drivers_.size();
}

RewriteOptions* ServerContext::global_options() {
  if (base_class_options_.get() == NULL) {
    base_class_options_.reset(factory_->default_options()->Clone());
  }
  return base_class_options_.get();
}

const RewriteOptions* ServerContext::global_options() const {
  if (base_class_options_.get() == NULL) {
    return factory_->default_options();
  }
  return base_class_options_.get();
}

void ServerContext::reset_global_options(RewriteOptions* options) {
  base_class_options_.reset(options);
}

RewriteOptions* ServerContext::NewOptions() {
  return factory_->NewRewriteOptions();
}

ServerContext::OptionsBoolPair ServerContext::GetQueryOptions(
    GoogleUrl* request_url, RequestHeaders* request_headers,
    ResponseHeaders* response_headers) {
  scoped_ptr<RewriteOptions> query_options;
  bool success = false;
  switch (RewriteQuery::Scan(factory(), request_url, request_headers,
                             response_headers, &query_options,
                             message_handler_)) {
    case RewriteQuery::kInvalid:
      query_options.reset(NULL);
      break;
    case RewriteQuery::kNoneFound:
      query_options.reset(NULL);
      success = true;
      break;
    case RewriteQuery::kSuccess:
      success = true;
      break;
    default:
      query_options.reset(NULL);
  }
  // Note: success==false is treated as an error (we return 405 in
  // proxy_interface.cc), while query_options==NULL merely means there are no
  // query options.
  return OptionsBoolPair(query_options.release(), success);
}

RewriteOptions* ServerContext::GetCustomOptions(RequestHeaders* request_headers,
                                                RewriteOptions* domain_options,
                                                RewriteOptions* query_options) {
  RewriteOptions* options = global_options();
  scoped_ptr<RewriteOptions> custom_options;
  scoped_ptr<RewriteOptions> scoped_domain_options(domain_options);
  if (scoped_domain_options.get() != NULL) {
    custom_options.reset(NewOptions());
    custom_options->Merge(*options);
    custom_options->Merge(*scoped_domain_options);
    options = custom_options.get();
  }

  scoped_ptr<RewriteOptions> query_options_ptr(query_options);
  // Check query params & request-headers
  if (query_options_ptr.get() != NULL) {
    // Subtle memory management to handle deleting any domain_options
    // after the merge, and transferring ownership to the caller for
    // the new merged options.
    scoped_ptr<RewriteOptions> options_buffer(custom_options.release());
    custom_options.reset(NewOptions());
    custom_options->Merge(*options);
    custom_options->Merge(*query_options);
    // Don't run any experiments if this is a special query-params request.
    custom_options->set_running_furious_experiment(false);
  }

  if (request_headers->IsXmlHttpRequest()) {
    // For XmlHttpRequests, disable filters that insert js. Otherwise, there
    // will be two copies of the same scripts in the html dom -- one from main
    // html page and another from html content fetched from ajax and this
    // will corrupt global variable state.
    // Sometimes, js present in the ajax request does not get executed.
    // TODO(sriharis): Set a flag in RewriteOptions indicating that we are
    // working with Ajax and thus should not assume the base URL is correct.
    // Note that there is no guarantee that the header will be set on an ajax
    // request and so the option will not be set for all ajax requests.
    if (custom_options == NULL) {
      custom_options.reset(options->Clone());
    }
    custom_options->DisableFilter(RewriteOptions::kLazyloadImages);
    custom_options->DisableFilter(RewriteOptions::kDelayImages);
    custom_options->DisableFilter(RewriteOptions::kPrioritizeVisibleContent);
    custom_options->DisableFilter(RewriteOptions::kDeferJavascript);
    custom_options->DisableFilter(RewriteOptions::kLocalStorageCache);
  }

  url_namer()->ConfigureCustomOptions(*request_headers, custom_options.get());

  return custom_options.release();
}

void ServerContext::ComputeSignature(RewriteOptions* rewrite_options) const {
  rewrite_options->ComputeSignature(lock_hasher());
}

bool ServerContext::IsExcludedAttribute(const char* attribute) {
  const char** end = kExcludedAttributes + arraysize(kExcludedAttributes);
  return std::binary_search(kExcludedAttributes, end, attribute,
                            CharStarCompareInsensitive());
}

void ServerContext::set_enable_property_cache(bool enabled) {
  enable_property_cache_ = enabled;
  if (page_property_cache_.get() != NULL) {
    page_property_cache_->set_enabled(enabled);
  }
  if (client_property_cache_.get() != NULL) {
    client_property_cache_->set_enabled(enabled);
  }
}

void ServerContext::MakePropertyCaches(CacheInterface* backend_cache) {
  // The property caches are L2-only.  We cannot use the L1 cache because
  // this data can get stale quickly.
  page_property_cache_.reset(MakePropertyCache(
      PropertyCache::kPagePropertyCacheKeyPrefix, backend_cache));
  client_property_cache_.reset(MakePropertyCache(
      PropertyCache::kClientPropertyCacheKeyPrefix, backend_cache));
  client_property_cache_->AddCohort(ClientState::kClientStateCohort);
}

PropertyCache* ServerContext::MakePropertyCache(
    const GoogleString& cache_key_prefix, CacheInterface* cache) const {
  PropertyCache* pcache = new PropertyCache(
      cache_key_prefix, cache, timer(), statistics(), thread_system_);
  pcache->set_enabled(enable_property_cache_);
  return pcache;
}

AbstractClientState* RewriteDriverFactory::NewClientState() {
  return new ClientState();
}

void ServerContext::set_blink_critical_line_data_finder(
    BlinkCriticalLineDataFinder* finder) {
  blink_critical_line_data_finder_.reset(finder);
}

void ServerContext::set_critical_images_finder(
    CriticalImagesFinder* finder) {
  critical_images_finder_.reset(finder);
}

void ServerContext::set_flush_early_info_finder(
    FlushEarlyInfoFinder* finder) {
  flush_early_info_finder_.reset(finder);
}

}  // namespace net_instaweb
