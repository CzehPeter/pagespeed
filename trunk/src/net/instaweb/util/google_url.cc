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
//         nforman@google.com  (Naomi Forman)

#include "net/instaweb/util/public/google_url.h"

#include <cstddef>
#include <string>
#include "base/logging.h"
#include "net/instaweb/util/public/query_params.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

GoogleUrl::GoogleUrl()
    : gurl_() {
}

GoogleUrl::GoogleUrl(const GURL& gurl)
    : gurl_(gurl) {
}

GoogleUrl::GoogleUrl(const GoogleString& spec)
    : gurl_(spec) {
}
GoogleUrl::GoogleUrl(const StringPiece& sp)
    : gurl_(sp.as_string()) {
}

GoogleUrl::GoogleUrl(const char* str)
    : gurl_(str) {
}

// The following three constructors create a new GoogleUrl by resolving the
// String(Piece) against the base.
GoogleUrl::GoogleUrl(const GoogleUrl& base, const GoogleString& str) {
  Reset(base, str);
}

GoogleUrl::GoogleUrl(const GoogleUrl& base, const StringPiece& sp) {
  Reset(base, sp);
}

GoogleUrl::GoogleUrl(const GoogleUrl& base, const char* str) {
  Reset(base, str);
}

bool GoogleUrl::Reset(const GoogleUrl& base, const GoogleString& str) {
  gurl_ = base.gurl_.Resolve(str);
  return gurl_.is_valid();
}

bool GoogleUrl::Reset(const GoogleUrl& base, const StringPiece& sp) {
  gurl_ = base.gurl_.Resolve(sp.as_string());
  return gurl_.is_valid();
}

bool GoogleUrl::Reset(const GoogleUrl& base, const char* str) {
  gurl_ = base.gurl_.Resolve(str);
  return gurl_.is_valid();
}

GoogleUrl* GoogleUrl::CopyAndAddQueryParam(const StringPiece& name,
                                          const StringPiece& value) {
  QueryParams query_params;
  query_params.Parse(Query());
  query_params.Add(name, value);
  GoogleString query_params_string = query_params.ToString();
  url_canon::Replacements<char> replace_query;
  url_parse::Component query;
  query.len = query_params_string.size();
  replace_query.SetQuery(query_params_string.c_str(), query);
  GoogleUrl* result =
      new GoogleUrl(gurl_.ReplaceComponents(replace_query));
  return result;
}

size_t GoogleUrl::LeafEndPosition(const GURL& gurl) {
  url_parse::Parsed parsed = gurl.parsed_for_possibly_invalid_spec();
  if (parsed.path.is_valid()) {
    return parsed.path.end();
  }
  if (parsed.port.is_valid()) {
    return parsed.port.end();
  }
  if (parsed.host.is_valid()) {
    return parsed.host.end();
  }
  if (parsed.password.is_valid()) {
    return parsed.password.end();
  }
  if (parsed.username.is_valid()) {
    return parsed.username.end();
  }
  if (parsed.scheme.is_valid()) {
    return parsed.scheme.end();
  }
  return 0;
}

// Returns the offset at which the leaf ends in valid url spec.
// If there is no path, steps backward until valid end is found.
size_t GoogleUrl::LeafEndPosition() const {
  return LeafEndPosition(gurl_);
}

size_t GoogleUrl::LeafStartPosition(const GURL& gurl) {
  url_parse::Parsed parsed = gurl.parsed_for_possibly_invalid_spec();
  size_t start_reverse_search_from = std::string::npos;
  if (parsed.query.is_valid() && (parsed.query.begin > 0)) {
    // query includes '?', so start the search from the character
    // before it.
    start_reverse_search_from = parsed.query.begin - 1;
  }
  return gurl.possibly_invalid_spec().rfind('/', start_reverse_search_from);
}

// Returns the offset at which the leaf starts in the fully
// qualified spec.
size_t GoogleUrl::LeafStartPosition() const {
  return LeafStartPosition(gurl_);
}

size_t GoogleUrl::PathStartPosition(const GURL& gurl) {
  const std::string& spec = gurl.spec();
  url_parse::Parsed parsed = gurl.parsed_for_possibly_invalid_spec();
  size_t origin_size = parsed.path.begin;
  if (!parsed.path.is_valid()) {
    origin_size = spec.size();
  }
  CHECK_LT(0, static_cast<int>(origin_size));
  CHECK_LE(origin_size, spec.size());
  return origin_size;
}

// Find the start of the path, includes '/'
size_t GoogleUrl::PathStartPosition() const {
  return PathStartPosition(gurl_);
}

bool GoogleUrl::Reset(const StringPiece& new_value) {
  gurl_ = GURL(new_value.as_string());
  return gurl_.is_valid();
}

bool GoogleUrl::Reset(const GoogleUrl& new_value) {
  gurl_ = GURL(new_value.gurl_);
  return gurl_.is_valid();
}

void GoogleUrl::Clear() {
  gurl_ = GURL();
}

StringPiece GoogleUrl::AllExceptQuery() const {
  const std::string& spec = gurl_.possibly_invalid_spec();
  if (gurl_.is_valid()) {
    size_t leaf_end = LeafEndPosition();
    return StringPiece(spec.data(), leaf_end);
  } else {
    return StringPiece(spec.data(), 0);
  }
}

StringPiece GoogleUrl::AllAfterQuery() const {
  const std::string& spec = gurl_.possibly_invalid_spec();
  if (gurl_.is_valid()) {
    url_parse::Parsed parsed = gurl_.parsed_for_possibly_invalid_spec();
    size_t query_end;
    if (gurl_.has_query()) {
      query_end = parsed.query.end();
    } else {
      query_end = LeafEndPosition();
    }
    return StringPiece(spec.data() + query_end, spec.size() - query_end);
  } else {
    return StringPiece(spec.data(), 0);
  }
}

// Find the last slash before the question-mark, if any.  See
// http://en.wikipedia.org/wiki/URI_scheme -- the query-string
// syntax is not well-defined.  But the query-separator is well-defined:
// it's a ? so I believe this implies that the first ? has to delimit
// the query string.
StringPiece GoogleUrl::AllExceptLeaf() const {
  DCHECK(gurl_.is_valid()) << "Invalid URL: " << gurl_.possibly_invalid_spec();
  size_t last_slash = LeafStartPosition();
  CHECK(last_slash != std::string::npos);
  return StringPiece(gurl_.spec().data(), last_slash + 1);
}

StringPiece GoogleUrl::LeafWithQuery() const {
  DCHECK(gurl_.is_valid()) << "Invalid URL: " << gurl_.possibly_invalid_spec();
  size_t last_slash = LeafStartPosition();
  const std::string& spec = gurl_.spec();
  CHECK(last_slash != spec.npos);
  return StringPiece(spec.data() + last_slash + 1,
                     spec.size() - (last_slash + 1));
}

StringPiece GoogleUrl::LeafSansQuery() const {
  DCHECK(gurl_.is_valid()) << "Invalid URL: " << gurl_.possibly_invalid_spec();
  const std::string& spec = gurl_.spec();
  size_t after_last_slash = LeafStartPosition() + 1;
  size_t leaf_length = spec.size() - after_last_slash;
  if (!gurl_.has_query()) {
    return StringPiece(spec.data() + after_last_slash,
                       leaf_length);
  }
  url_parse::Parsed parsed = gurl_.parsed_for_possibly_invalid_spec();
  // parsed.query.len doesn't include the '?'
  return StringPiece(spec.data() + after_last_slash,
                     leaf_length - (parsed.query.len + 1));
}

// For "http://a.com/b/c/d?e=f/g returns "http://a.com" without trailing slash
StringPiece GoogleUrl::Origin() const {
  DCHECK(gurl_.is_valid()) << "Invalid URL: " << gurl_.possibly_invalid_spec();
  size_t origin_size = PathStartPosition();
  return StringPiece(gurl_.spec().data(), origin_size);
}

// For "http://a.com/b/c/d?e=f/g returns "/b/c/d?e=f/g" including leading slash
StringPiece GoogleUrl::PathAndLeaf() const {
  DCHECK(gurl_.is_valid()) << "Invalid URL: " << gurl_.possibly_invalid_spec();
  const std::string& spec = gurl_.spec();
  size_t origin_size = PathStartPosition();
  return StringPiece(spec.data() + origin_size,
                     spec.size() - origin_size);
}

// For "http://a.com/b/c/d/g.html returns "/b/c/d/" including leading and
// trailing slashes.
// For queries, "http://a.com/b/c/d?E=f/g" returns "/b/c/".
StringPiece GoogleUrl::PathSansLeaf() const {
  DCHECK(gurl_.is_valid()) << "Invalid URL: " << gurl_.possibly_invalid_spec();
  size_t path_start = PathStartPosition();
  size_t leaf_start = LeafStartPosition();
  return StringPiece(gurl_.spec().data() + path_start,
                     leaf_start - path_start + 1);
}

// Extracts the filename portion of the path and returns it. The filename
// is everything after the last slash in the path. This may be empty.
GoogleString GoogleUrl::ExtractFileName() const {
  return gurl_.ExtractFileName();
}

StringPiece GoogleUrl::Host() const {
  if (!gurl_.has_host()) {
    return NULL;
  }
  url_parse::Parsed parsed = gurl_.parsed_for_possibly_invalid_spec();
  return StringPiece(gurl_.spec().data() + parsed.host.begin,
                     parsed.host.len);
}

StringPiece GoogleUrl::HostAndPort() const {
  if (!gurl_.has_host()) {
    return NULL;
  }
  url_parse::Parsed parsed = gurl_.parsed_for_possibly_invalid_spec();
  return StringPiece(gurl_.spec().data() + parsed.host.begin,
                     parsed.host.len + parsed.port.len + 1);  // Yes, it works.
}

StringPiece GoogleUrl::PathSansQuery() const {
  const std::string& spec = gurl_.spec();
  url_parse::Parsed parsed = gurl_.parsed_for_possibly_invalid_spec();
  size_t path_start = PathStartPosition();
  size_t path_length = parsed.path.len;
  return StringPiece(spec.data() + path_start, path_length);
}

StringPiece GoogleUrl::Query() const {
  if (!gurl_.has_query()) {
    return NULL;
  }
  url_parse::Parsed parsed = gurl_.parsed_for_possibly_invalid_spec();
  return StringPiece(gurl_.spec().data() + parsed.query.begin,
                     parsed.query.len);
}

StringPiece GoogleUrl::Scheme() const {
  if (!gurl_.has_scheme()) {
    return NULL;
  }
  url_parse::Parsed parsed = gurl_.parsed_for_possibly_invalid_spec();
  return StringPiece(gurl_.spec().data() + parsed.scheme.begin,
                     parsed.scheme.len);
}

StringPiece GoogleUrl::Spec() const {
  const std::string& spec = gurl_.spec();
  return StringPiece(spec.data(), spec.size());
}

StringPiece GoogleUrl::UncheckedSpec() const {
  const std::string& spec = gurl_.possibly_invalid_spec();
  return StringPiece(spec.data(), spec.size());
}

}  // namespace net_instaweb
