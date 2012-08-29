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

#include "net/instaweb/http/public/cache_url_async_fetcher.h"

#include "base/logging.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/http_value_writer.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/timer.h"

namespace instaweb { class LoggingInfo; }

namespace net_instaweb {

namespace {

class CachePutFetch : public SharedAsyncFetch {
 public:
  CachePutFetch(const GoogleString& url, AsyncFetch* base_fetch,
                bool respect_vary, bool default_cache_html,
                HTTPCache* cache, Histogram* backend_first_byte_latency,
                MessageHandler* handler)
      : SharedAsyncFetch(base_fetch),
        url_(url),
        respect_vary_(respect_vary),
        default_cache_html_(default_cache_html),
        cache_(cache),
        backend_first_byte_latency_(backend_first_byte_latency),
        handler_(handler),
        cacheable_(false),
        cache_value_writer_(&cache_value_, cache_) {
    if (backend_first_byte_latency_ != NULL) {
      start_time_ms_ = cache_->timer()->NowMs();
    }
  }

  virtual ~CachePutFetch() {}

  virtual void HandleHeadersComplete() {
    // We compute the latency here as it's the spot where we're doing an
    // actual backend fetch and not potentially using the cache.
    int64 now_ms = cache_->timer()->NowMs();
    if (backend_first_byte_latency_ != NULL) {
      backend_first_byte_latency_->Add(now_ms - start_time_ms_);
    }
    ResponseHeaders* headers = response_headers();
    headers->FixDateHeaders(now_ms);
    bool is_html = headers->IsHtmlLike();
    const char* cache_control = headers->Lookup1(HttpAttributes::kCacheControl);
    if (default_cache_html_ && is_html &&
        // TODO(sligocki): Use some sort of computed
        // headers->HasExplicitCachingTtl() instead
        // of just checking for the existence of 2 headers.
        (cache_control == NULL || cache_control == StringPiece("public")) &&
        !headers->Has(HttpAttributes::kExpires)) {
      headers->Add(HttpAttributes::kCacheControl,
          "max-age=" + Integer64ToString(headers->implicit_cache_ttl_ms()));
    }
    headers->ComputeCaching();

    cacheable_ = headers->IsProxyCacheableGivenRequest(*request_headers());
    if (cacheable_ && (respect_vary_ || is_html)) {
      cacheable_ = headers->VaryCacheable(
          request_headers()->Has(HttpAttributes::kCookie));
    }

    if (cacheable_) {
      // Make a copy of the headers which we will send to the
      // cache_value_writer_ later.
      saved_headers_.CopyFrom(*headers);
    }

    base_fetch()->HeadersComplete();
  }

  virtual bool HandleWrite(const StringPiece& content,
                           MessageHandler* handler) {
    bool ret = true;
    ret &= base_fetch()->Write(content, handler);
    if (cacheable_) {
      ret &= cache_value_writer_.Write(content, handler);
    }
    return ret;
  }

  virtual bool HandleFlush(MessageHandler* handler) {
    // Note cache_value_.Flush doesn't do anything.
    return base_fetch()->Flush(handler);
  }

  virtual void HandleDone(bool success) {
    if (success && cacheable_ && cache_value_writer_.has_buffered()) {
      // The X-Original-Content-Length header will have been added after
      // HandleHeadersComplete(), so extract its value and add it to the
      // saved headers.
      const char* orig_content_length = extra_response_headers()->Lookup1(
          HttpAttributes::kXOriginalContentLength);
      if (orig_content_length != NULL) {
        saved_headers_.Replace(HttpAttributes::kXOriginalContentLength,
                               orig_content_length);
      }
      // Finalize the headers.
      cache_value_writer_.SetHeaders(&saved_headers_);
    }

    // Finish fetch.
    base_fetch()->Done(success);
    // Add result to cache.
    if (success && cacheable_ && cache_value_writer_.has_buffered()) {
      cache_->Put(url_, &cache_value_, handler_);
    }
    delete this;
  }

 private:
  const GoogleString url_;
  bool respect_vary_;
  bool default_cache_html_;
  HTTPCache* cache_;
  Histogram* backend_first_byte_latency_;
  MessageHandler* handler_;

  bool cacheable_;
  HTTPValue cache_value_;
  HTTPValueWriter cache_value_writer_;
  int64 start_time_ms_;  // only used if backend_first_byte_latency_ != NULL
  ResponseHeaders saved_headers_;

  DISALLOW_COPY_AND_ASSIGN(CachePutFetch);
};

class CacheFindCallback : public HTTPCache::Callback {
 public:
  CacheFindCallback(const GoogleString& url,
                    AsyncFetch* base_fetch,
                    CacheUrlAsyncFetcher* owner,
                    MessageHandler* handler)
      : url_(url),
        base_fetch_(base_fetch),
        cache_(owner->http_cache()),
        fetcher_(owner->fetcher()),
        backend_first_byte_latency_(
            owner->backend_first_byte_latency_histogram()),
        fallback_responses_served_(owner->fallback_responses_served()),
        num_conditional_refreshes_(owner->num_conditional_refreshes()),
        handler_(handler),
        respect_vary_(owner->respect_vary()),
        ignore_recent_fetch_failed_(owner->ignore_recent_fetch_failed()),
        serve_stale_if_fetch_error_(owner->serve_stale_if_fetch_error()),
        default_cache_html_(owner->default_cache_html()) {
    // Note that this is a cache lookup: there are no request-headers.  At
    // this level, we have already made a policy decision that any Vary
    // headers present will be ignored (see
    // http://code.google.com/speed/page-speed/docs/install.html#respectvary ).
    set_response_headers(base_fetch->response_headers());
  }
  virtual ~CacheFindCallback() {}

  virtual LoggingInfo* logging_info() {
    return base_fetch_->logging_info();
  }

  virtual void Done(HTTPCache::FindResult find_result) {
    switch (find_result) {
      case HTTPCache::kFound: {
        VLOG(1) << "Found in cache: " << url_;
        http_value()->ExtractHeaders(response_headers(), handler_);

        // Respond with a 304 if the If-Modified-Since / If-None-Match values
        // are equal to those in the request.
        if (ShouldReturn304()) {
          response_headers()->Clear();
          response_headers()->SetStatusAndReason(HttpStatus::kNotModified);
          base_fetch_->HeadersComplete();
        } else {
          base_fetch_->HeadersComplete();

          StringPiece contents;
          http_value()->ExtractContents(&contents);
          // TODO(sligocki): We are writing all the content in one shot, this
          // fact might be useful to the HtmlParser if this is HTML. Perhaps
          // we should add an API for conveying that information.
          base_fetch_->Write(contents, handler_);
        }

        base_fetch_->Done(true);
        break;
      }
      // Note: currently no resources fetched through CacheUrlAsyncFetcher
      // will be marked RememberFetchFailedOrNotCacheable.
      // TODO(sligocki): Should we mark resources as such in this class?
      case HTTPCache::kRecentFetchFailed:
      case HTTPCache::kRecentFetchNotCacheable:
        VLOG(1) << "RecentFetchFailedOrNotCacheable: " << url_;
        if (!ignore_recent_fetch_failed_) {
          base_fetch_->Done(false);
          break;
        } else {
          // If we are ignoring advice of kRecentFetchFailedOrNotCacheable,
          // we will refetch the resource as we would for kNotFound.
          //
          // For example, we should do this for fetches that are being proxied.
          // fall through
        }
      case HTTPCache::kNotFound: {
        VLOG(1) << "Did not find in cache: " << url_;
        // If fallback_http_value() is populated, use it in case the fetch
        // fails. Note that this is only populated if the response in cache is
        // stale.
        AsyncFetch* base_fetch = base_fetch_;
        if (serve_stale_if_fetch_error_) {
          FallbackSharedAsyncFetch* fallback_fetch =
              new FallbackSharedAsyncFetch(
                  base_fetch_, fallback_http_value(), handler_);
          fallback_fetch->set_fallback_responses_served(
              fallback_responses_served_);
          base_fetch = fallback_fetch;
        }
        CachePutFetch* put_fetch = new CachePutFetch(
            url_, base_fetch, respect_vary_, default_cache_html_, cache_,
            backend_first_byte_latency_, handler_);
        DCHECK_EQ(response_headers(), base_fetch_->response_headers());

        // Remove any Etags added by us before sending the request out.
        const char* etag = request_headers()->Lookup1(
            HttpAttributes::kIfNoneMatch);
        if (etag != NULL &&
            StringCaseStartsWith(etag, HTTPCache::kEtagPrefix)) {
          put_fetch->request_headers()->RemoveAll(HttpAttributes::kIfNoneMatch);
        }

        ConditionalSharedAsyncFetch* conditional_fetch =
            new ConditionalSharedAsyncFetch(
                put_fetch, fallback_http_value(), handler_);
        conditional_fetch->set_num_conditional_refreshes(
            num_conditional_refreshes_);

        fetcher_->Fetch(url_, handler_, conditional_fetch);
        break;
      }
    }

    delete this;
  }

  virtual bool IsCacheValid(const GoogleString& key,
                            const ResponseHeaders& headers) {
    // base_fetch_ is assumed to have the key (URL).
    if (base_fetch_->IsCachedResultValid(headers)) {
      // The response may have been cached when respect_vary_ was disabled.
      // Hence we need to make sure that it is still usable for the current
      // request.
      // Also, if we cached a response with "Vary: Cookie", we cannot use it if
      // the current request has a Cookie header in the request.
      bool is_html = headers.IsHtmlLike();
      if (respect_vary_ || is_html) {
        return headers.VaryCacheable(
            request_headers()->Has(HttpAttributes::kCookie));
      }
      return true;
    }
    return false;
  }

 private:
  bool ShouldReturn304() const {
    if (ConditionalHeadersMatch(HttpAttributes::kIfNoneMatch,
                                HttpAttributes::kEtag)) {
      // If the Etag matches, return a 304.
      return true;
    }
    // Otherwise, return a 304 only if there was no If-None-Match header in the
    // request and the last modified timestamp matches.
    // (from http://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html)
    return request_headers()->Lookup1(HttpAttributes::kIfNoneMatch) == NULL &&
        ConditionalHeadersMatch(HttpAttributes::kIfModifiedSince,
                                HttpAttributes::kLastModified);
  }

  bool ConditionalHeadersMatch(const StringPiece& request_header,
                               const StringPiece& response_header) const {
    const char* request_header_value =
        request_headers()->Lookup1(request_header);
    const char* response_header_value =
        response_headers()->Lookup1(response_header);
    return request_header_value != NULL && response_header_value != NULL &&
        strcmp(request_header_value, response_header_value) == 0;
  }

  const RequestHeaders* request_headers() const {
    return base_fetch_->request_headers();
  }
  RequestHeaders* request_headers() { return base_fetch_->request_headers(); }

  const GoogleString url_;
  RequestHeaders request_headers_;
  AsyncFetch* base_fetch_;
  HTTPCache* cache_;
  UrlAsyncFetcher* fetcher_;
  Histogram* backend_first_byte_latency_;
  Variable* fallback_responses_served_;
  Variable* num_conditional_refreshes_;
  MessageHandler* handler_;

  bool respect_vary_;
  bool ignore_recent_fetch_failed_;
  bool serve_stale_if_fetch_error_;
  bool default_cache_html_;

  DISALLOW_COPY_AND_ASSIGN(CacheFindCallback);
};

}  // namespace

CacheUrlAsyncFetcher::~CacheUrlAsyncFetcher() {
}

bool CacheUrlAsyncFetcher::Fetch(
    const GoogleString& url, MessageHandler* handler, AsyncFetch* base_fetch) {

  // TODO(mmohabey): If the url is in cache, then use it for serving the head
  // request.
  if (base_fetch->request_headers()->method() != RequestHeaders::kGet) {
    // Without this if there is URL which responds both on GET
    // and POST. If GET comes first, and POST next then the cached
    // entry will be reused. POST is allowed to invalidate GET response
    // in cache, but not use the value in cache. For now just bypassing
    // cache for non GET requests.
    fetcher_->Fetch(url, handler, base_fetch);
    return false;
  }

  CacheFindCallback* find_callback =
      new CacheFindCallback(url, base_fetch, this, handler);

  http_cache_->Find(url, handler, find_callback);
  // Cache interface does not tell us if the request was immediately resolved,
  // so we must say that it wasn't.
  return false;
}

}  // namespace net_instaweb
