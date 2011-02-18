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

#include "net/instaweb/http/public/response_headers.h"

#include "base/logging.h"
#include "net/instaweb/http/http.pb.h"  // for HttpResponseHeaders

#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/proto_util.h"
#include "net/instaweb/util/public/string_multi_map.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/public/time_util.h"
#include "net/instaweb/util/public/writer.h"
#include "pagespeed/core/resource_util.h"

namespace net_instaweb {

const int64 ResponseHeaders::kImplicitCacheTtlMs =
    5 * net_instaweb::Timer::kMinuteMs;

ResponseHeaders::ResponseHeaders() {
  proto_.reset(new HttpResponseHeaders);
  Clear();
}

ResponseHeaders::~ResponseHeaders() {
  Clear();
}

void ResponseHeaders::CopyFrom(const ResponseHeaders& other) {
  map_.reset(NULL);
  proto_->clear_header();

  proto_->set_cacheable(other.proto_->cacheable());
  proto_->set_proxy_cacheable(other.proto_->proxy_cacheable());
  proto_->set_expiration_time_ms(other.proto_->expiration_time_ms());
  proto_->set_timestamp_ms(other.proto_->timestamp_ms());
  proto_->set_major_version(other.proto_->major_version());
  proto_->set_minor_version(other.proto_->minor_version());
  proto_->set_status_code(other.proto_->status_code());
  proto_->set_reason_phrase(other.proto_->reason_phrase());

  for (int i = 0; i < other.NumAttributes(); ++i) {
    Add(other.Name(i), other.Value(i));
  }
  cache_fields_dirty_ = other.cache_fields_dirty_;
}

void ResponseHeaders::Clear() {
  Headers<HttpResponseHeaders>::Clear();

  proto_->set_cacheable(false);
  proto_->set_proxy_cacheable(false);   // accurate only if !cache_fields_dirty_
  proto_->clear_expiration_time_ms();
  proto_->clear_timestamp_ms();
  proto_->clear_status_code();
  proto_->clear_reason_phrase();
  proto_->clear_header();
  cache_fields_dirty_ = false;
}

int ResponseHeaders::status_code() const {
  return proto_->status_code();
}

const char* ResponseHeaders::reason_phrase() const {
  return proto_->has_reason_phrase()
      ? proto_->reason_phrase().c_str()
      : "(null)";
}

int64 ResponseHeaders::timestamp_ms() const {
  DCHECK(!cache_fields_dirty_) << "Call ComputeCaching() before timestamp_ms()";
  return proto_->timestamp_ms();
}

bool ResponseHeaders::has_timestamp_ms() const {
  return proto_->has_timestamp_ms();
}

void ResponseHeaders::set_status_code(int code) {
  proto_->set_status_code(code);
}

bool ResponseHeaders::has_status_code() const {
  return proto_->has_status_code();
}

void ResponseHeaders::set_reason_phrase(const StringPiece& reason_phrase) {
  proto_->set_reason_phrase(reason_phrase.data(), reason_phrase.size());
}

void ResponseHeaders::Add(const StringPiece& name, const StringPiece& value) {
  Headers<HttpResponseHeaders>::Add(name, value);
  cache_fields_dirty_ = true;
}

void ResponseHeaders::RemoveAll(const StringPiece& name) {
  if (Headers<HttpResponseHeaders>::RemoveAll(name)) {
    cache_fields_dirty_ = true;
  }
}

void ResponseHeaders::Replace(
    const StringPiece& name, const StringPiece& value) {
  cache_fields_dirty_ = true;
  Headers<HttpResponseHeaders>::Replace(name, value);
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
bool ResponseHeaders::IsCacheable() const {
  // We do not compute caching from accessors so that the
  // accessors can be easier to call from multiple threads
  // without mutexing.
  DCHECK(!cache_fields_dirty_) << "Call ComputeCaching() before IsCacheable()";
  return proto_->cacheable();
}

bool ResponseHeaders::IsProxyCacheable() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before IsProxyCacheable()";
  return proto_->proxy_cacheable();
}

// Returns the ms-since-1970 absolute time when this resource
// should be expired out of caches.
int64 ResponseHeaders::CacheExpirationTimeMs() const {
  DCHECK(!cache_fields_dirty_)
      << "Call ComputeCaching() before CacheExpirationTimeMs()";
  return proto_->expiration_time_ms();
}

void ResponseHeaders::SetDate(int64 date_ms) {
  std::string time_string;
  if (ConvertTimeToString(date_ms, &time_string)) {
    Replace(HttpAttributes::kDate, time_string);
  }
}

void ResponseHeaders::SetLastModified(int64 last_modified_ms) {
  std::string time_string;
  if (ConvertTimeToString(last_modified_ms, &time_string)) {
    Replace(HttpAttributes::kLastModified, time_string);
  }
}

void ResponseHeaders::ComputeCaching() {
  pagespeed::Resource resource;
  for (int i = 0, n = NumAttributes(); i < n; ++i) {
    resource.AddResponseHeader(Name(i), Value(i));
  }
  resource.SetResponseStatusCode(proto_->status_code());

  CharStarVector values;
  int64 date;
  // Compute the timestamp if we can find it
  if (Lookup("Date", &values) && (values.size() == 1) &&
      ConvertStringToTime(values[0], &date)) {
    proto_->set_timestamp_ms(date);
  }

  // TODO(jmarantz): Should we consider as cacheable a resource
  // that simply has no cacheable hints at all?  For now, let's
  // make that assumption.  We should review this policy with bmcquade,
  // souders, etc, but first let's try to measure some value with this
  // optimistic intrepretation.
  //
  // TODO(jmarantz): get from bmcquade a comprehensive ways in which these
  // policies will differ for Instaweb vs Pagespeed.
  bool explicit_no_cache =
      pagespeed::resource_util::HasExplicitNoCacheDirective(resource);
  bool likely_static =
      pagespeed::resource_util::IsLikelyStaticResource(resource);

  // status_cacheable implies that either the resource content was
  // cacheable, or the status code indicated some other aspect of
  // our system that we want to remember in the cache, such as
  // that fact that a fetch failed for a resource, and we don't want
  // to try again until some time has passed.
  bool status_cacheable =
      ((status_code() == HttpStatus::kRememberNotFoundStatusCode) ||
       pagespeed::resource_util::IsCacheableResourceStatusCode(status_code()));
  int64 freshness_lifetime_ms;
  bool explicit_cacheable =
      pagespeed::resource_util::GetFreshnessLifetimeMillis(
          resource, &freshness_lifetime_ms) && has_timestamp_ms();
  proto_->set_cacheable(!explicit_no_cache &&
                       (explicit_cacheable || likely_static) &&
                       status_cacheable);

  if (proto_->cacheable()) {
    // TODO(jmarantz): check "Age" resource and use that to reduce
    // the expiration_time_ms_.  This is, says, bmcquade@google.com,
    // typically use to indicate how long a resource has been sitting
    // in a proxy-cache.
    if (!explicit_cacheable) {
      // implicitly cached items stay alive in our system for 5 minutes
      // TODO(jmarantz): consider making this a flag, or getting some
      // other heuristic value from the PageSpeed libraries.
      freshness_lifetime_ms = kImplicitCacheTtlMs;
    }
    proto_->set_expiration_time_ms(proto_->timestamp_ms() +
                                  freshness_lifetime_ms);

    // Assume it's proxy cacheable.  Then iterate over all the headers
    // with key HttpAttributes::kCacheControl, and all the comma-separated
    // values within those values, and look for 'private'.
    proto_->set_proxy_cacheable(true);
    values.clear();
    if (Lookup(HttpAttributes::kCacheControl, &values)) {
      for (int i = 0, n = values.size(); i < n; ++i) {
        const char* cache_control = values[i];
        pagespeed::resource_util::DirectiveMap directive_map;
        if (pagespeed::resource_util::GetHeaderDirectives(
                cache_control, &directive_map)) {
          pagespeed::resource_util::DirectiveMap::iterator p =
              directive_map.find("private");
          if (p != directive_map.end()) {
            proto_->set_proxy_cacheable(false);
            break;
          }
        }
      }
    }
  } else {
    proto_->set_expiration_time_ms(0);
    proto_->set_proxy_cacheable(false);
  }
  cache_fields_dirty_ = false;
}

std::string ResponseHeaders::ToString() const {
  std::string str;
  StringWriter writer(&str);
  WriteAsHttp(&writer, NULL);
  return str;
}

void ResponseHeaders::SetStatusAndReason(HttpStatus::Code code) {
  set_status_code(code);
  set_reason_phrase(HttpStatus::GetReasonPhrase(code));
}

bool ResponseHeaders::ParseTime(const char* time_str, int64* time_ms) {
  return pagespeed::resource_util::ParseTimeValuedHeader(time_str, time_ms);
}

bool ResponseHeaders::IsGzipped() const {
  CharStarVector v;
  return (Lookup(HttpAttributes::kContentEncoding, &v) && (v.size() == 1) &&
          (strcmp(v[0], HttpAttributes::kGzip) == 0));
}

bool ResponseHeaders::ParseDateHeader(
    const StringPiece& attr, int64* date_ms) const {
  CharStarVector values;
  return (Lookup(attr, &values) &&
          (values.size() == 1) &&
          ConvertStringToTime(values[0], date_ms));
}

void ResponseHeaders::UpdateDateHeader(const StringPiece& attr, int64 date_ms) {
  RemoveAll(attr);
  std::string buf;
  if (ConvertTimeToString(date_ms, &buf)) {
    Add(attr, buf.c_str());
  }
}

namespace {

const char* BoolToString(bool b) {
  return ((b) ? "true" : "false");
}

}  // namespace

void ResponseHeaders::DebugPrint() const {
  fprintf(stderr, "%s\n", ToString().c_str());
  fprintf(stderr, "cache_fields_dirty_ = %s\n",
          BoolToString(cache_fields_dirty_));
  if (!cache_fields_dirty_) {
    fprintf(stderr, "expiration_time_ms_ = %ld\n",
            static_cast<long>(proto_->expiration_time_ms()));  // NOLINT
    fprintf(stderr, "timestamp_ms_ = %ld\n",
            static_cast<long>(proto_->timestamp_ms()));        // NOLINT
    fprintf(stderr, "cacheable_ = %s\n", BoolToString(proto_->cacheable()));
    fprintf(stderr, "proxy_cacheable_ = %s\n",
            BoolToString(proto_->proxy_cacheable()));
  }
}

}  // namespace net_instaweb
