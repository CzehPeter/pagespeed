/**
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

#include "net/instaweb/util/public/simple_meta_data.h"
#include <assert.h>
#include <stdio.h>
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/writer.h"
#include "pagespeed/core/resource_util.h"

namespace {
const int64 TIME_UNINITIALIZED = -1;
}

namespace net_instaweb {

SimpleMetaData::SimpleMetaData()
    : parsing_http_(false),
      parsing_value_(false),
      headers_complete_(false),
      cache_fields_dirty_(true),
      expiration_time_ms_(TIME_UNINITIALIZED),
      timestamp_ms_(TIME_UNINITIALIZED),
      major_version_(0),
      minor_version_(0),
      status_code_(0) {
}

SimpleMetaData::~SimpleMetaData() {
  for (int i = 0, n = attribute_vector_.size(); i < n; ++i) {
    delete [] attribute_vector_[i].second;
  }
}

int SimpleMetaData::NumAttributes() const {
  return attribute_vector_.size();
}

const char* SimpleMetaData::Name(int index) const {
  return attribute_vector_[index].first;
}

const char* SimpleMetaData::Value(int index) const {
  return attribute_vector_[index].second;
}

bool SimpleMetaData::Lookup(const char* name, CharStarVector* values) const {
  AttributeMap::const_iterator p = attribute_map_.find(name);
  bool ret = false;
  if (p != attribute_map_.end()) {
    ret = true;
    *values = p->second;
  }
  return ret;
}

void SimpleMetaData::Add(const char* name, const char* value) {
  // TODO(jmarantz): Parse comma-separated values.  bmcquade sez:
  // you probably want to normalize these by splitting on commas and
  // adding a separate k,v pair for each comma-separated value. then
  // it becomes very easy to do things like search for individual
  // Content-Type tokens. Otherwise the client has to assume that
  // every single value could be comma-separated and they have to
  // parse it as such.  the list of header names that are not safe to
  // comma-split is at
  // http://src.chromium.org/viewvc/chrome/trunk/src/net/http/http_util.cc
  // (search for IsNonCoalescingHeader)

  CharStarVector dummy_values;
  std::pair<AttributeMap::iterator, bool> iter_inserted =
      attribute_map_.insert(AttributeMap::value_type(name, dummy_values));
  AttributeMap::iterator iter = iter_inserted.first;
  CharStarVector& values = iter->second;
  int value_buf_size = strlen(value) + 1;
  char* value_copy = new char[value_buf_size];
  memcpy(value_copy, value, value_buf_size);
  values.push_back(value_copy);
  attribute_vector_.push_back(StringPair(iter->first.c_str(), value_copy));
  cache_fields_dirty_ = true;
}

void SimpleMetaData::RemoveAll(const char* name) {
  AttributeVector temp_vector;  // Temp variable for new vector.
  temp_vector.reserve(attribute_vector_.size());
  for (int i = 0; i < NumAttributes(); ++i) {
    if (strcasecmp(Name(i),  name) != 0) {
      temp_vector.push_back(attribute_vector_[i]);
    } else {
      delete [] attribute_vector_[i].second;
    }
  }
  attribute_vector_.swap(temp_vector);

  // Note: we have to erase from the map second, because map owns the name.
  attribute_map_.erase(name);
}

// Serialize meta-data to a stream.
bool SimpleMetaData::Write(Writer* writer, MessageHandler* handler) const {
  bool ret = true;
  char buf[100];
  snprintf(buf, sizeof(buf), "HTTP/%d.%d %d ",
           major_version_, minor_version_, status_code_);
  ret &= writer->Write(buf, handler);
  ret &= writer->Write(reason_phrase_, handler);
  ret &= writer->Write("\r\n", handler);
  ret &= WriteHeaders(writer, handler);
  return ret;
}

bool SimpleMetaData::WriteHeaders(Writer* writer,
                                  MessageHandler* handler) const {
  bool ret = true;
  for (int i = 0, n = attribute_vector_.size(); ret && (i < n); ++i) {
    const StringPair& attribute = attribute_vector_[i];
    ret &= writer->Write(attribute.first, handler);
    ret &= writer->Write(": ", handler);
    ret &= writer->Write(attribute.second, handler);
    ret &= writer->Write("\r\n", handler);
  }
  ret &= writer->Write("\r\n", handler);
  return ret;
}

// TODO(jmaessen): http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.2
// I bet we're doing this wrong:
//  Header fields can be extended over multiple lines by preceding each extra
//  line with at least one SP or HT.
int SimpleMetaData::ParseChunk(const StringPiece& text,
                               MessageHandler* handler) {
  assert(!headers_complete_);
  int num_consumed = 0;
  int num_bytes = text.size();

  for (; num_consumed < num_bytes; ++num_consumed) {
    char c = text[num_consumed];
    if ((c == '/') && (parse_name_ == "HTTP")) {
      if (major_version_ != 0) {
        handler->Message(kError, "Multiple HTTP Lines");
      } else {
        parsing_http_ = true;
        parsing_value_ = true;
      }
    } else if (!parsing_value_ && (c == ':')) {
      parsing_value_ = true;
    } else if (c == '\r') {
      // Just ignore CRs for now, and break up headers on newlines for
      // simplicity.  It's not clear to me if it's important that we
      // reject headers that lack the CR in front of the LF.
    } else if (c == '\n') {
      if (parse_name_.empty()) {
        // blank line.  This marks the end of the headers.
        ++num_consumed;
        headers_complete_ = true;
        ComputeCaching();
        break;
      }
      if (parsing_http_) {
        // Parsing "1.0 200 OK\r", using sscanf for the integers, and
        // private method GrabLastToken for the "OK".
        if ((sscanf(parse_value_.c_str(), "%d.%d %d ",  // NOLINT
                    &major_version_, &minor_version_, &status_code_) != 3) ||
            !GrabLastToken(parse_value_, &reason_phrase_)) {
          // TODO(jmarantz): capture the filename/url, track the line numbers.
          handler->Message(kError, "Invalid HTML headers: %s",
                           parse_value_.c_str());
        }
        parsing_http_ = false;
      } else {
        Add(parse_name_.c_str(), parse_value_.c_str());
      }
      parsing_value_ = false;
      parse_name_.clear();
      parse_value_.clear();
    } else if (parsing_value_) {
      // Skip leading whitespace
      if (!parse_value_.empty() || !isspace(c)) {
        parse_value_ += c;
      }
    } else {
      parse_name_ += c;
    }
  }
  return num_consumed;
}

// Specific information about cache.  This is all embodied in the
// headers but is centrally parsed so we can try to get it right.
bool SimpleMetaData::IsCacheable() const {
  // We do not compute caching from accessors so that the
  // accessors can be easier to call from multiple threads
  // without mutexing.
  assert(!cache_fields_dirty_);
  return is_cacheable_;
}

bool SimpleMetaData::IsProxyCacheable() const {
  assert(!cache_fields_dirty_);
  return is_proxy_cacheable_;
}

// Returns the ms-since-1970 absolute time when this resource
// should be expired out of caches.
int64 SimpleMetaData::CacheExpirationTimeMs() const {
  assert(!cache_fields_dirty_);
  return expiration_time_ms_;
}

void SimpleMetaData::ComputeCaching() {
  pagespeed::Resource resource;
  for (int i = 0, n = NumAttributes(); i < n; ++i) {
    resource.AddResponseHeader(Name(i), Value(i));
  }

  CharStarVector values;
  int64 date;
  // Compute the timestamp if we can find it
  if (Lookup("Date", &values) && (values.size() == 1) &&
      pagespeed::resource_util::ParseTimeValuedHeader(values[0], &date)) {
    timestamp_ms_ = date;
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
  bool explicit_cacheable =
      pagespeed::resource_util::IsCacheableResource(resource);
  bool status_cacheable =
      pagespeed::resource_util::IsCacheableResourceStatusCode(status_code_);
  is_cacheable_ = ((!explicit_no_cache || explicit_cacheable) &&
                   status_cacheable);
  if (is_cacheable_) {
    int64 freshness_lifetime_ms;
    if (pagespeed::resource_util::GetFreshnessLifetimeMillis(
            resource, &freshness_lifetime_ms) &&
        has_timestamp_ms()) {
      expiration_time_ms_ = timestamp_ms_ + freshness_lifetime_ms;
    } else {
      expiration_time_ms_ = 0;  // no date: was cacheable, but expired in 1970.
    }

    // Assume it's proxy cacheable.  Then iterate over all the headers
    // with key "Cache-Control", and all the comma-separated values within
    // those values, and look for 'private'.
    is_proxy_cacheable_ = true;
    values.clear();
    if (Lookup("Cache-Control", &values)) {
      for (int i = 0, n = values.size(); i < n; ++i) {
        const char* cache_control = values[i];
        pagespeed::resource_util::DirectiveMap directive_map;
        if (pagespeed::resource_util::GetHeaderDirectives(
                cache_control, &directive_map)) {
          pagespeed::resource_util::DirectiveMap::iterator p =
              directive_map.find("private");
          if (p != directive_map.end()) {
            is_proxy_cacheable_ = false;
            break;
          }
        }
      }
    }
  } else {
    expiration_time_ms_ = 0;
    is_proxy_cacheable_ = false;
  }
  cache_fields_dirty_ = false;
}

bool SimpleMetaData::has_timestamp_ms() const {
  return timestamp_ms_ != TIME_UNINITIALIZED;
}

std::string SimpleMetaData::ToString() const {
  std::string str;
  StringWriter writer(&str);
  Write(&writer, NULL);
  return str;
}

// Grabs the last non-whitespace token from 'input' and puts it in 'output'.
bool SimpleMetaData::GrabLastToken(const std::string& input,
                                   std::string* output) {
  bool ret = false;
  // Safely grab the response code string from the end of parse_value_.
  int last_token_char = -1;
  for (int i = input.size() - 1; i >= 0; --i) {
    char c = input[i];
    if (isspace(c)) {
      if (last_token_char >= 0) {
        // We found the whole token.
        const char* token_start = input.c_str() + i + 1;
        int token_len = last_token_char - i;
        output->append(token_start, token_len);
        ret = true;
        break;
      }
    } else if (last_token_char == -1) {
      last_token_char = i;
    }
  }
  return ret;
}

}  // namespace net_instaweb
