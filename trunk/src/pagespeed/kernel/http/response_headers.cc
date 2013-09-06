// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Author: jmarantz@google.com (Joshua Marantz)

#include "pagespeed/kernel/http/response_headers.h"

#include <algorithm>                    // for min
#include <cstdio>     // for fprintf, stderr, snprintf
#include <memory>

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/escaping.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_multi_map.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/time_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/base/writer.h"
#include "pagespeed/kernel/http/caching_headers.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/headers.h"
#include "pagespeed/kernel/http/http.pb.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"

namespace net_instaweb {

// Specifies the maximum amount of forward drift we'll allow for a Date
// timestamp.  E.g. if it's 3:00:00 and the Date header says its 3:01:00,
// we'll leave the date-header in the future.  But if it's 3:03:01 then
// we'll set it back to 3:00:00 exactly in FixDateHeaders.
const int64 kMaxAllowedDateDriftMs = 3L * net_instaweb::Timer::kMinuteMs;

class MessageHandler;

const int64 ResponseHeaders::kDefaultImplicitCacheTtlMs;
const int64 ResponseHeaders::kDefaultMinCacheTtlMs;

ResponseHeaders::ResponseHeaders()
    : implicit_cache_ttl_ms_(kDefaultImplicitCacheTtlMs),
      min_cache_ttl_ms_(kDefaultMinCacheTtlMs) {
  proto_.reset(new HttpResponseHeaders);
  Clear();
}

ResponseHeaders::~ResponseHeaders() {
  Clear();
}

namespace {

// TODO(pulkitg): Change kRefreshExpirePercent to be configurable via flag.
const int64 kRefreshExpirePercent = 80;

void ApplyTimeDelta(const char* attr, int64 delta_ms,
                    ResponseHeaders* headers) {
  int64 time_ms;
  if (headers->ParseDateHeader(attr, &time_ms)) {
    int64 adjusted_time_ms = time_ms + delta_ms;
    if (adjusted_time_ms > 0) {
      headers->SetTimeHeader(attr, time_ms + delta_ms);
    }
  }
}

StringPieceVector NamesToSanitize() {
  StringPieceVector v(2);
  v.push_back(HttpAttributes::kSetCookie);
  v.push_back(HttpAttributes::kSetCookie2);
  return v;
}

}  // namespace

bool ResponseHeaders::IsImminentlyExpiring(
    int64 start_date_ms, int64 expire_ms, int64 now_ms) {
  // Consider a resource with 5 minute expiration time (the default
  // assumed by mod_pagespeed when a potentialy cacheable resource
  // lacks a cache control header, which happens a lot).  If the
  // origin TTL was 5 minutes and 4 minutes have expired, then we want
  // to re-fetch it so that we can avoid expiring the data.
  //
  // If we don't do this, then every 5 minutes, someone will see
  // this page unoptimized.  In a site with very low QPS, including
  // test instances of a site, this can happen quite often.
  const int64 ttl_ms = expire_ms - start_date_ms;
  // Only proactively refresh resources that have at least our
  // default expiration of 5 minutes.
  //
  // TODO(jmaessen): Lower threshold when If-Modified-Since checking is in
  // place; consider making this settable.
  // TODO(pradnya): We will freshen only if ttl is greater than the default
  // implicit ttl. If the implicit ttl has been overridden by a site, we will
  // not honor it here. Fix that.

  if (ttl_ms < ResponseHeaders::kDefaultImplicitCacheTtlMs) {
    return false;
  }
  int64 freshen_threshold = std::min(
      ResponseHeaders::kDefaultImplicitCacheTtlMs,
      ((100 - kRefreshExpirePercent) * ttl_ms) / 100);
  return (expire_ms - now_ms < freshen_threshold);
}

void ResponseHeaders::FixDateHeaders(int64 now_ms) {
  int64 date_ms = 0;
  bool has_date = true;

  if (cache_fields_dirty_) {
    // We don't want to call ComputeCaching() right here because it's expensive,
    // and if we decide we need to alter the Date header then we'll have to
    // recompute Caching later anyway.
    has_date = ParseDateHeader(HttpAttributes::kDate, &date_ms);
  } else if (proto_->has_date_ms()) {
    date_ms = proto_->date_ms();
  } else {
    has_date = false;
  }

  // If the Date is missing, set one.  If the Date is present but is older
  // than now_ms, correct it.  Also correct it if it's more than a fixed
  // amount in the future.
  if (!has_date || (date_ms < now_ms) ||
      (date_ms > now_ms + kMaxAllowedDateDriftMs)) {
    bool recompute_caching = !cache_fields_dirty_;
    SetDate(now_ms);
    if (has_date) {
      int64 delta_ms = now_ms - date_ms;
      ApplyTimeDelta(HttpAttributes::kExpires, delta_ms, this);

      // TODO(jmarantz): This code was refactored from http_dump_url_fetcher.cc,
      // which was adjusting the LastModified header when the date was fixed.
      // I wrote that code originally and can't think now why that would make
      // sense, so I'm commenting this out for now.  If this turns out to be
      // a problem replaying old Slurps then this code should be re-instated,
      // possibly based on a flag passed in.
      //     ApplyTimeDelta(HttpAttributes::kLastModified, delta_ms, this);
    } else {
      SetDate(now_ms);
      // TODO(jmarantz): see above.
      //     SetTimeHeader(HttpAttributes::kLastModified, now_ms);

      // If there was no Date header, there cannot possibly be any rationality
      // to an Expires header.  So remove it for now.  We can always add it in
      // if Page Speed computed a TTL.
      RemoveAll(HttpAttributes::kExpires);

      // If Expires was previously set, but there was no date, then
      // try to compute it from the TTL & the current time.  If there
      // was no TTL then we should just remove the Expires headers.
      int64 expires_ms;
      if (ParseDateHeader(HttpAttributes::kExpires, &expires_ms)) {
        ComputeCaching();

        // Page Speed's caching libraries will now compute the expires
        // for us based on the TTL and the date we just set, so we can
        // set a corrected expires header.
        if (proto_->has_expiration_time_ms()) {
          SetTimeHeader(HttpAttributes::kExpires, proto_->expiration_time_ms());
        }
        cache_fields_dirty_ = false;
        recompute_caching = false;
      }
    }

    if (recompute_caching) {
      ComputeCaching();
    }
  }
}

void ResponseHeaders::CopyFrom(const ResponseHeaders& other) {
  map_.reset(NULL);
  *(proto_.get()) = *(other.proto_.get());
  cache_fields_dirty_ = other.cache_fields_dirty_;
  force_cache_ttl_ms_ = other.force_cache_ttl_ms_;
  force_cached_ = other.force_cached_;
  implicit_cache_ttl_ms_ = other.implicit_cache_ttl_ms_;
  min_cache_ttl_ms_ = other.min_cache_ttl_ms_;
}

void ResponseHeaders::Clear() {
  Headers<HttpResponseHeaders>::Clear();

  proto_->set_browser_cacheable(false);  // accurate iff !cache_fields_dirty_
  proto_->set_requires_proxy_revalidation(false);
  proto_->set_requires_browser_revalidation(false);
  proto_->clear_expiration_time_ms();
  proto_->clear_date_ms();
  proto_->clear_last_modified_time_ms();
  proto_->clear_status_code();
  proto_->clear_reason_phrase();
  proto_->clear_header();
  proto_->clear_is_implicitly_cacheable();
  cache_fields_dirty_ = false;
  force_cache_ttl_ms_ = -1;
  force_cached_ = false;
  min_cache_ttl_applied_ = false;
}

int ResponseHeaders::status_code() const {
  return proto_->status_code();
}

void ResponseHeaders::set_status_code(int code) {
  proto_->set_status_code(code);
}

bool ResponseHeaders::has_status_code() const {
  return proto_->has_status_code();
}

const char* ResponseHeaders::reason_phrase() const {
  return proto_->has_reason_phrase()
      ? proto_->reason_phrase().c_str()
      : "(null)";
}

void ResponseHeaders::set_reason_phrase(const StringPiece& reason_phrase) {
  proto_->set_reason_phrase(reason_phrase.data(), reason_phrase.size());
}

int64 ResponseHeaders::last_modified_time_ms() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before last_modified_time_ms()";
  return proto_->last_modified_time_ms();
}

int64 ResponseHeaders::date_ms() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before date_ms()";
  return proto_->date_ms();
}

int64 ResponseHeaders::cache_ttl_ms() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before cache_ttl_ms()";
  return proto_->cache_ttl_ms();
}

bool ResponseHeaders::has_date_ms() const {
  return proto_->has_date_ms();
}

bool ResponseHeaders::is_implicitly_cacheable() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before is_implicitly_cacheable()";
  return proto_->is_implicitly_cacheable();
}

void ResponseHeaders::Add(const StringPiece& name, const StringPiece& value) {
  Headers<HttpResponseHeaders>::Add(name, value);
  cache_fields_dirty_ = true;
}

// Return true if Content type field changed.
// If there's already a content type specified, leave it.
// If there's already a mime type or a charset specified,
// leave that and fill in the missing piece (if specified).
bool ResponseHeaders::CombineContentTypes(const StringPiece& orig,
                                          const StringPiece& fresh) {
  bool ret = false;
  GoogleString mime_type, charset;
  ret = ParseContentType(orig, &mime_type, &charset);
  if (!ret) {
    GoogleString fresh_mime_type, fresh_charset;
    ret = ParseContentType(fresh, &fresh_mime_type, &fresh_charset);
    // Don't replace nothing with a charset only because
    // ; charset=xyz is not a valid ContentType header..
    if (ret && !fresh_mime_type.empty()) {
      Replace(HttpAttributes::kContentType, fresh);
      ret = true;
    } else {
      ret = false;
    }
  } else if (charset.empty() || mime_type.empty()) {
    GoogleString fresh_mime_type, fresh_charset;
    ret = ParseContentType(fresh, &fresh_mime_type, &fresh_charset);
    if (ret) {
      if (charset.empty()) {
        charset = fresh_charset;
      }
      if (mime_type.empty()) {
        mime_type = fresh_mime_type;
      }
      GoogleString full_type = StringPrintf(
          "%s;%s%s",
          mime_type.c_str(),
          charset.empty()? "" : " charset=",
          charset.c_str());
      Replace(HttpAttributes::kContentType, full_type);
      ret = true;
    }
  }
  if (ret) {
    cache_fields_dirty_ = true;
  }
  return ret;
}

bool ResponseHeaders::MergeContentType(const StringPiece& content_type) {
  bool ret = false;
  ConstStringStarVector old_values;
  Lookup(HttpAttributes::kContentType, &old_values);
  // If there aren't any content-type headers, we can just add this one.
  // If there is exactly one content-type header, then try to merge it
  // with what we were passed.
  // If there is already more than one content-type header, it's
  // unclear what exactly should happen, so don't change anything.
  if (old_values.size() < 1) {
    ret = CombineContentTypes("", content_type);
  } else if (old_values.size() == 1) {
    StringPiece old_val(*old_values[0]);
    ret = CombineContentTypes(old_val, content_type);
  }
  if (ret) {
    cache_fields_dirty_ = true;
  }
  return ret;
}

bool ResponseHeaders::Remove(const StringPiece& name,
                             const StringPiece& value) {
  if (Headers<HttpResponseHeaders>::Remove(name, value)) {
    cache_fields_dirty_ = true;
    return true;
  }
  return false;
}

bool ResponseHeaders::RemoveAll(const StringPiece& name) {
  if (Headers<HttpResponseHeaders>::RemoveAll(name)) {
    cache_fields_dirty_ = true;
    return true;
  }
  return false;
}

bool ResponseHeaders::RemoveAllFromSortedArray(
    const StringPiece* names, int names_size) {
  if (Headers<HttpResponseHeaders>::RemoveAllFromSortedArray(
          names, names_size)) {
    cache_fields_dirty_ = true;
    return true;
  }
  return false;
}

void ResponseHeaders::Replace(
    const StringPiece& name, const StringPiece& value) {
  cache_fields_dirty_ = true;
  Headers<HttpResponseHeaders>::Replace(name, value);
}

void ResponseHeaders::UpdateFrom(const Headers<HttpResponseHeaders>& other) {
  cache_fields_dirty_ = true;
  Headers<HttpResponseHeaders>::UpdateFrom(other);
}

void ResponseHeaders::UpdateFromProto(const HttpResponseHeaders& proto) {
  Clear();
  cache_fields_dirty_ = true;
  proto_->CopyFrom(proto);
}

bool ResponseHeaders::WriteAsBinary(Writer* writer, MessageHandler* handler) {
  if (cache_fields_dirty_) {
    ComputeCaching();
  }
  return Headers<HttpResponseHeaders>::WriteAsBinary(writer, handler);
}

bool ResponseHeaders::ReadFromBinary(const StringPiece& buf,
                                     MessageHandler* message_handler) {
  cache_fields_dirty_ = false;
  return Headers<HttpResponseHeaders>::ReadFromBinary(buf, message_handler);
}

// Serialize meta-data to a binary stream.
bool ResponseHeaders::WriteAsHttp(Writer* writer, MessageHandler* handler)
    const {
  bool ret = true;
  char buf[100];
  snprintf(buf, sizeof(buf), "HTTP/%d.%d %d ",
           major_version(), minor_version(), status_code());
  ret &= writer->Write(buf, handler);
  ret &= writer->Write(reason_phrase(), handler);
  ret &= writer->Write("\r\n", handler);
  ret &= Headers<HttpResponseHeaders>::WriteAsHttp(writer, handler);
  return ret;
}

// Specific information about cache.  This is all embodied in the
// headers but is centrally parsed so we can try to get it right.
bool ResponseHeaders::IsBrowserCacheable() const {
  // We do not compute caching from accessors so that the
  // accessors can be easier to call from multiple threads
  // without mutexing.
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before IsBrowserCacheable()";
  return proto_->browser_cacheable();
}

bool ResponseHeaders::RequiresBrowserRevalidation() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before RequiresBrowserRevalidation()";
  return proto_->requires_browser_revalidation();
}

bool ResponseHeaders::RequiresProxyRevalidation() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before RequiresProxyRevalidation()";
  return proto_->requires_proxy_revalidation();
}

bool ResponseHeaders::IsProxyCacheable() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before IsProxyCacheable()";
  return proto_->proxy_cacheable();
}

bool ResponseHeaders::IsProxyCacheableGivenRequest(
    const RequestHeaders& req_headers) const {
  if (!IsProxyCacheable()) {
    return false;
  }

  if (req_headers.Has(HttpAttributes::kAuthorization)) {
    // For something requested with authorization to be cacheable, it must
    // either  be something that goes through revalidation (which we currently
    // do not do) or something that has a Cache-Control: public.
    // See RFC2616, 14.8
    // (http://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html#sec14.8)
    return HasValue(HttpAttributes::kCacheControl, "public");
  }
  return true;
}

// Returns the ms-since-1970 absolute time when this resource
// should be expired out of caches.
int64 ResponseHeaders::CacheExpirationTimeMs() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before CacheExpirationTimeMs()";
  return proto_->expiration_time_ms();
}

void ResponseHeaders::SetDateAndCaching(
    int64 date_ms, int64 ttl_ms, const StringPiece& cache_control_suffix) {
  SetDate(date_ms);
  // Note: We set both Expires and Cache-Control headers so that legacy
  // HTTP/1.0 browsers and proxies correctly cache these resources.
  SetTimeHeader(HttpAttributes::kExpires, date_ms + ttl_ms);
  Replace(HttpAttributes::kCacheControl,
          StrCat("max-age=", Integer64ToString(ttl_ms / Timer::kSecondMs),
                 cache_control_suffix));
}

void ResponseHeaders::SetTimeHeader(const StringPiece& header, int64 time_ms) {
  GoogleString time_string;
  if (ConvertTimeToString(time_ms, &time_string)) {
    Replace(header, time_string);
  }
}

void ResponseHeaders::SetOriginalContentLength(int64 content_length) {
  // This does not impact caching headers, so avoid ComputeCaching()
  // by restoring cache_fields_dirty_ after we set the header.
  bool dirty = cache_fields_dirty_;
  Replace(HttpAttributes::kXOriginalContentLength,
          Integer64ToString(content_length));
  cache_fields_dirty_ = dirty;
}

bool ResponseHeaders::Sanitize() {
  // Remove cookies, which we will never store in a cache.
  StringPieceVector names_to_sanitize = NamesToSanitize();
  return RemoveAllFromSortedArray(&names_to_sanitize[0],
                                  names_to_sanitize.size());
}

void ResponseHeaders::GetSanitizedProto(HttpResponseHeaders* proto) const {
  proto->CopyFrom(*proto_.get());
  protobuf::RepeatedPtrField<NameValue>* headers = proto->mutable_header();
  StringPieceVector names_to_sanitize = NamesToSanitize();
  return RemoveFromHeaders(&names_to_sanitize[0],
                           names_to_sanitize.size(), headers);
}

bool ResponseHeaders::VaryCacheable(bool request_has_cookie) const {
  if (IsProxyCacheable()) {
    if (force_cache_ttl_ms_ > 0) {
      // If we've been asked to force cache a request, then we always consider
      // it as VaryCacheable.
      return true;
    }

    ConstStringStarVector values;
    Lookup(HttpAttributes::kVary, &values);
    bool vary_cacheable = true;
    for (int i = 0, n = values.size(); i < n; ++i) {
      StringPiece val(*values[i]);
      if (val.empty() ||
          StringCaseEqual(HttpAttributes::kAcceptEncoding, val) ||
          (!request_has_cookie &&
           StringCaseEqual(HttpAttributes::kCookie, val))) {
        // If the request doesn't have cookies set, we consider Vary: Cookie as
        // cacheable.
        continue;
      }
      vary_cacheable = false;
      break;
    }
    return vary_cacheable;
  }
  return false;
}

namespace {

// Subclass of pagespeed's cache computer to deal with our slightly different
// policies.
//
// The differences are:
//  1) TODO(sligocki): We can consider HTML to be cacheable by default
//     depending upon a user option.
//  2) We only consider HTTP status code 200, 301 and our internal use codes
//     to be cacheable. Others (such as 203, 206 and 304) are not cacheable
//     for us.
//
// This also abstracts away the pagespeed::Resource/ResponseHeaders distinction.
class InstawebCacheComputer : public CachingHeaders {
 public:
  explicit InstawebCacheComputer(const ResponseHeaders& headers)
      : CachingHeaders(headers.status_code()),
        response_headers_(headers) {
  }

  virtual ~InstawebCacheComputer() {}

  // Which status codes are cacheable by default.
  virtual bool IsCacheableResourceStatusCode() const {
    switch (status_code()) {
      // For our purposes, only a few status codes are cacheable.
      // Others like 203, 206 and 304 depend upon input headers and other state.
      case HttpStatus::kOK:
      case HttpStatus::kMovedPermanently:
      // These dummy status codes indicate something about our system that we
      // want to remember in the cache.
      case HttpStatus::kRememberNotCacheableStatusCode:
      case HttpStatus::kRememberNotCacheableAnd200StatusCode:
      case HttpStatus::kRememberFetchFailedStatusCode:
        return true;
      default:
        return false;
    }
  }

  // Which status codes do we allow to cache at all. Others will not be cached
  // even if explicitly marked as such because we may not be able to cache
  // them correctly (say 304 or 206, which depend upon input headers).
  bool IsAllowedCacheableStatusCode() {
    // For now it's identical to the default cacheable list.
    return IsCacheableResourceStatusCode();

    // Note: We have made a consious decision not to allow caching
    // 302 Found or 307 Temporary Redirect even if they explicitly
    // ask to be cached because most webmasters use 301 Moved Permanently
    // for redirects they actually want cached.
  }

  virtual bool IsLikelyStaticResourceType() const {
    if (IsRedirectStatusCode()) {
      return true;  // redirects are cacheable
    }
    const ContentType* type = response_headers_.DetermineContentType();
    return (type != NULL) && type->IsLikelyStaticResource();
  }

  virtual bool Lookup(const StringPiece& key, StringPieceVector* values) {
    ConstStringStarVector value_strings;
    bool ret = response_headers_.Lookup(key, &value_strings);
    if (ret) {
      values->resize(value_strings.size());
      for (int i = 0, n = value_strings.size(); i < n; ++i) {
        (*values)[i] = *value_strings[i];
      }
    } else {
      values->clear();
    }
    return ret && !values->empty();
  }

 private:
  const ResponseHeaders& response_headers_;
  DISALLOW_COPY_AND_ASSIGN(InstawebCacheComputer);
};

}  // namespace

void ResponseHeaders::ComputeCaching() {
  if (!cache_fields_dirty_) {
    return;
  }

  ConstStringStarVector values;
  int64 date;
  bool has_date = ParseDateHeader(HttpAttributes::kDate, &date);
  // Compute the timestamp if we can find it
  if (has_date) {
    proto_->set_date_ms(date);
  }

  // Computes caching info.
  InstawebCacheComputer computer(*this);

  // Can we force cache this response?
  bool force_caching_enabled = false;

  const ContentType* type = DetermineContentType();
  if ((force_cache_ttl_ms_) > 0 &&
      (status_code() == HttpStatus::kOK)) {
    force_caching_enabled = (type == NULL) || !type->IsHtmlLike();
  }

  // Note: Unlike pagespeed algorithm, we are very conservative about calling
  // a resource cacheable. Many status codes are technically cacheable but only
  // based upon precise input headers. Since we do not check those headers we
  // only allow a few hand-picked status codes to be cacheable at all.
  // Note that if force caching is enabled, we consider a privately cacheable
  // resource as cacheable.
  bool is_browser_cacheable = computer.IsCacheable();
  proto_->set_browser_cacheable(
      has_date &&
      computer.IsAllowedCacheableStatusCode() &&
      (force_caching_enabled || is_browser_cacheable));
  proto_->set_requires_browser_revalidation(computer.MustRevalidate());
  proto_->set_requires_proxy_revalidation(
      computer.ProxyRevalidate() || proto_->requires_browser_revalidation());
  if (proto_->browser_cacheable()) {
    // TODO(jmarantz): check "Age" resource and use that to reduce
    // the expiration_time_ms_.  This is, says, bmcquade@google.com,
    // typically use to indicate how long a resource has been sitting
    // in a proxy-cache. Or perhaps this should be part of the pagespeed
    // ResourceCacheComputer algorithms.
    // See: http://www.w3.org/Protocols/rfc2616/rfc2616-sec13.html
    //
    // Implicitly cached items stay alive in our system for the specified
    // implicit ttl ms.
    bool is_proxy_cacheable = computer.IsProxyCacheable();
    int64 cache_ttl_ms = implicit_cache_ttl_ms();
    if (computer.IsExplicitlyCacheable()) {
      // TODO(sligocki): Do we care about the return value.
      computer.GetFreshnessLifetimeMillis(&cache_ttl_ms);
      // If min_cache_ttl_ms is set, this overrides cache TTL hints even if
      // explicitly set in the header. Use the max of min_cache_ttl_ms and
      // the cache_ttl computed so far. Do this only for non HTML.
      if (type != NULL && !type->IsHtmlLike() &&
          min_cache_ttl_ms_ > cache_ttl_ms) {
        cache_ttl_ms = min_cache_ttl_ms_;
        min_cache_ttl_applied_ = true;
      }
    }
    if (force_caching_enabled &&
        (force_cache_ttl_ms_ > cache_ttl_ms || !is_proxy_cacheable)) {
      // We consider the response to have been force cached only if force
      // caching was enabled and the forced cache TTL is larger than the
      // original TTL or the original response wasn't cacheable.
      cache_ttl_ms = force_cache_ttl_ms_;
      force_cached_ = true;
    }

    proto_->set_cache_ttl_ms(cache_ttl_ms);
    proto_->set_expiration_time_ms(proto_->date_ms() + cache_ttl_ms);
    proto_->set_proxy_cacheable(force_cached_ || is_proxy_cacheable);

    // Do not cache HTML or redirects with Set-Cookie / Set-Cookie2 header even
    // though they may have explicit caching directives. This is to prevent the
    // caching of user sensitive data due to misconfigured caching headers.
    if (((type != NULL && type->IsHtmlLike()) ||
         computer.IsRedirectStatusCode()) &&
        (Has(HttpAttributes::kSetCookie) || Has(HttpAttributes::kSetCookie2))) {
      proto_->set_proxy_cacheable(false);
    }

    if (proto_->proxy_cacheable() && !force_cached_) {
      if (!computer.IsExplicitlyCacheable()) {
        // If the resource is proxy cacheable but it does not have explicit
        // caching headers and is not force cached, explicitly set the caching
        // headers.
        DCHECK(has_date);
        DCHECK(cache_ttl_ms == implicit_cache_ttl_ms());
        proto_->set_is_implicitly_cacheable(true);
        SetDateAndCaching(date, cache_ttl_ms, CacheControlValuesToPreserve());
      } else if (min_cache_ttl_applied_) {
        DCHECK(has_date);
        DCHECK(cache_ttl_ms == min_cache_ttl_ms());
        SetDateAndCaching(date, cache_ttl_ms, CacheControlValuesToPreserve());
      }
    }
  } else {
    proto_->set_expiration_time_ms(0);
    proto_->set_proxy_cacheable(false);
  }
  cache_fields_dirty_ = false;
}

GoogleString ResponseHeaders::CacheControlValuesToPreserve() {
  GoogleString to_preserve;
  if (HasValue(HttpAttributes::kCacheControl, "no-transform")) {
    to_preserve = ", no-transform";
  }
  if (HasValue(HttpAttributes::kCacheControl, "no-store")) {
    to_preserve += ", no-store";
  }
  return to_preserve;
}

GoogleString ResponseHeaders::ToString() const {
  GoogleString str;
  StringWriter writer(&str);
  WriteAsHttp(&writer, NULL);
  return str;
}

void ResponseHeaders::SetStatusAndReason(HttpStatus::Code code) {
  set_status_code(code);
  set_reason_phrase(HttpStatus::GetReasonPhrase(code));
}

bool ResponseHeaders::ParseTime(const char* time_str, int64* time_ms) {
  return ConvertStringToTime(time_str, time_ms);
}

// Content-coding values are case-insensitive:
// http://www.w3.org/Protocols/rfc2616/rfc2616-sec3.html
// See Section 3.5
bool ResponseHeaders::IsGzipped() const {
  ConstStringStarVector v;
  bool found = Lookup(HttpAttributes::kContentEncoding, &v);
  if (found) {
    for (int i = 0, n = v.size(); i < n; ++i) {
      if ((v[i] != NULL) && StringCaseEqual(*v[i], HttpAttributes::kGzip)) {
        return true;
      }
    }
  }
  return false;
}

bool ResponseHeaders::WasGzippedLast() const {
  ConstStringStarVector v;
  bool found = Lookup(HttpAttributes::kContentEncoding, &v);
  if (found) {
    int index = v.size() - 1;
    if ((index > -1) && (v[index] != NULL) &&
        StringCaseEqual(*v[index], HttpAttributes::kGzip)) {
      return true;
    }
  }
  return false;
}

// TODO(sligocki): Perhaps we should take in a URL here and use that to
// guess Content-Type as well. See Resource::DetermineContentType().
void ResponseHeaders::DetermineContentTypeAndCharset(
    const ContentType** content_type_out, GoogleString* charset_out) const {
  ConstStringStarVector content_types;

  if (content_type_out != NULL) {
    *content_type_out = NULL;
  }

  if (charset_out != NULL) {
    charset_out->clear();
  }

  // If there is more than one content-type header, we pick the LAST one,
  // (even if it's invalid!) as that's the behavior specified by the mime
  // sniffing spec (http://mimesniff.spec.whatwg.org/). We also use the
  // charset that comes with the same header.
  if (Lookup(HttpAttributes::kContentType, &content_types) &&
      !content_types.empty()) {
    GoogleString mime_type, charset;
    if (!ParseContentType(*content_types.back(), &mime_type, &charset)) {
      mime_type.clear();
      charset.clear();
    }

    if (content_type_out != NULL) {
      *content_type_out = MimeTypeToContentType(mime_type);
    }

    if (charset_out != NULL) {
      *charset_out = charset;
    }
  }
}

GoogleString ResponseHeaders::DetermineCharset() const {
  GoogleString charset;
  DetermineContentTypeAndCharset(NULL, &charset);
  return charset;
}

const ContentType* ResponseHeaders::DetermineContentType() const {
  const ContentType* content_type = NULL;
  DetermineContentTypeAndCharset(&content_type, NULL);
  return content_type;
}

bool ResponseHeaders::ParseDateHeader(
    const StringPiece& attr, int64* date_ms) const {
  const char* date_string = Lookup1(attr);
  return (date_string != NULL) && ConvertStringToTime(date_string, date_ms);
}

void ResponseHeaders::ParseFirstLine(const StringPiece& first_line) {
  if (first_line.starts_with("HTTP/")) {
    ParseFirstLineHelper(first_line.substr(5));
  } else {
    LOG(WARNING) << "Could not parse first line: " << first_line;
  }
}

void ResponseHeaders::ParseFirstLineHelper(const StringPiece& first_line) {
  int major_version, minor_version, status;
  // We reserve enough to avoid buffer overflow on sscanf command.
  GoogleString reason_phrase(first_line.size(), '\0');
  char* reason_phrase_cstr = &reason_phrase[0];
  int num_scanned = sscanf(
      first_line.as_string().c_str(), "%d.%d %d %[^\n\t]s",
      &major_version, &minor_version, &status,
      reason_phrase_cstr);
  if (num_scanned < 3) {
    LOG(WARNING) << "Could not parse first line: " << first_line;
  } else {
    if (num_scanned == 3) {
      reason_phrase = HttpStatus::GetReasonPhrase(
          static_cast<HttpStatus::Code>(status));
      reason_phrase_cstr = &reason_phrase[0];
    }
    set_first_line(major_version, minor_version, status, reason_phrase_cstr);
  }
}

void ResponseHeaders::SetCacheControlMaxAge(int64 ttl_ms) {
  // If the cache fields were not dirty before this call, recompute caching
  // before returning.
  bool recompute_caching = !cache_fields_dirty_;

  SetTimeHeader(HttpAttributes::kExpires, date_ms() + ttl_ms);

  ConstStringStarVector values;
  Lookup(HttpAttributes::kCacheControl, &values);

  GoogleString new_cache_control_value =
      StrCat("max-age=", Integer64ToString(ttl_ms / Timer::kSecondMs));

  for (int i = 0, n = values.size(); i < n; ++i) {
    if (values[i] != NULL) {
      StringPiece val(*values[i]);
      if (!val.empty() && !StringCaseStartsWith(val, "max-age")) {
        StrAppend(&new_cache_control_value, ",", val);
      }
    }
  }
  Replace(HttpAttributes::kCacheControl, new_cache_control_value);

  if (recompute_caching) {
    ComputeCaching();
  }
}

void ResponseHeaders::DebugPrint() const {
  fprintf(stderr, "%s\n", ToString().c_str());
  fprintf(stderr, "cache_fields_dirty_ = %s\n",
          BoolToString(cache_fields_dirty_));
  fprintf(stderr, "is_implicitly_cacheable = %s\n",
          BoolToString(proto_->is_implicitly_cacheable()));
  fprintf(stderr, "implicit_cache_ttl_ms_ = %s\n",
          Integer64ToString(implicit_cache_ttl_ms()).c_str());
  fprintf(stderr, "min_cache_ttl_ms_ = %s\n",
          Integer64ToString(min_cache_ttl_ms()).c_str());
  fprintf(stderr, "min_cache_ttl_applied_ = %s\n",
          BoolToString(min_cache_ttl_applied_));
  if (!cache_fields_dirty_) {
    fprintf(stderr, "expiration_time_ms_ = %s\n",
            Integer64ToString(proto_->expiration_time_ms()).c_str());
    fprintf(stderr, "last_modified_time_ms_ = %s\n",
            Integer64ToString(last_modified_time_ms()).c_str());
    fprintf(stderr, "date_ms_ = %s\n",
            Integer64ToString(proto_->date_ms()).c_str());
    fprintf(stderr, "cache_ttl_ms_ = %s\n",
            Integer64ToString(proto_->cache_ttl_ms()).c_str());
    fprintf(stderr, "browser_cacheable_ = %s\n",
            BoolToString(proto_->browser_cacheable()));
    fprintf(stderr, "proxy_cacheable_ = %s\n",
            BoolToString(proto_->proxy_cacheable()));
  }
}

bool ResponseHeaders::FindContentLength(int64* content_length) const {
  const char* val = Lookup1(HttpAttributes::kContentLength);
  return (val != NULL) && StringToInt64(val, content_length);
}

void ResponseHeaders::ForceCaching(int64 ttl_ms) {
  // If the cache fields were not dirty before this call, recompute caching
  // before returning.
  bool recompute_caching = !cache_fields_dirty_;
  if (ttl_ms > 0) {
    force_cache_ttl_ms_ = ttl_ms;
    cache_fields_dirty_ = true;
    if (recompute_caching) {
      ComputeCaching();
    }
  }
}

bool ResponseHeaders::UpdateCacheHeadersIfForceCached() {
  if (cache_fields_dirty_) {
    LOG(DFATAL)  << "Call ComputeCaching() before "
                 << "UpdateCacheHeadersIfForceCached";
    return false;
  }
  if (force_cached_) {
    int64 date = date_ms();
    int64 ttl = cache_ttl_ms();
    RemoveAll(HttpAttributes::kPragma);
    RemoveAll(HttpAttributes::kCacheControl);
    SetDateAndCaching(date, ttl);
    ComputeCaching();
    return true;
  }
  return false;
}

int64 ResponseHeaders::SizeEstimate() const {
  int64 len = STATIC_STRLEN("HTTP/1.x 123 ") +  // All statuses are 3 digits.
              strlen(reason_phrase()) + STATIC_STRLEN("\r\n");
  for (int i = 0, n = NumAttributes(); i < n; ++i) {
    len += Name(i).length() + STATIC_STRLEN(": ") +
           Value(i).length() + STATIC_STRLEN("\r\n");
  }
  len += STATIC_STRLEN("\r\n");
  return len;
}

bool ResponseHeaders::GetCookieString(GoogleString* cookie_str) const {
  cookie_str->clear();
  ConstStringStarVector cookies;
  if (!Lookup(HttpAttributes::kSetCookie, &cookies)) {
    return false;
  }

  StrAppend(cookie_str, "[");
  for (int i = 0, n = cookies.size(); i < n; ++i) {
    GoogleString escaped;
    EscapeToJsStringLiteral(*cookies[i], true, &escaped);
    StrAppend(cookie_str, escaped);
    if (i != (n-1)) {
      StrAppend(cookie_str, ",");
    }
  }
  StrAppend(cookie_str, "]");
  return true;
}

bool ResponseHeaders::HasCookie(StringPiece name,
                                StringPieceVector* values) const {
  bool has_cookie = false;
  ConstStringStarVector cookies;
  if (Lookup(HttpAttributes::kSetCookie, &cookies)) {
    // Iterate through the cookies.
    for (int i = 0, n = cookies.size(); i < n; ++i) {
      StringPieceVector cookie_pairs;
      // Get the vector of name-value pairs of cookies.
      SplitStringPieceToVector(*cookies[i], ";", &cookie_pairs, true);
      for (int j = 0, npairs = cookie_pairs.size(); j < npairs; ++j) {
        StringPiece::size_type index = cookie_pairs[j].find('=');
        if (index == StringPiece::npos) {
          StringPiece cookie_attribute = cookie_pairs[j];
          TrimWhitespace(&cookie_attribute);
          if (StringCaseEqual(cookie_attribute, name)) {
            has_cookie = true;
          }
        } else {
          StringPiece cookie_attribute = cookie_pairs[j].substr(0, index);
          StringPiece cookie_value = cookie_pairs[j].substr(index + 1,
              cookie_pairs[j].size() - index - 1);
          TrimWhitespace(&cookie_attribute);
          if (StringCaseEqual(cookie_attribute, name)) {
            has_cookie = true;
            if (values != NULL) {
              TrimWhitespace(&cookie_value);
              values->push_back(cookie_value);
            }
          }
        }
      }
    }
  }
  return has_cookie;
}

}  // namespace net_instaweb
