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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_OPTIONS_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_OPTIONS_H_

#include <map>
#include <set>
#include "base/basictypes.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class MessageHandler;

class RewriteOptions {
 public:
  enum Filter {
    kAddBaseTag,
    kAddHead,
    kAddInstrumentation,
    kCollapseWhitespace,
    kCombineCss,
    kCombineHeads,
    kDebugLogImgTags,
    kElideAttributes,
    kExtendCache,
    kInlineCss,
    kInlineJavascript,
    kInsertImgDimensions,
    kLeftTrimUrls,
    kMoveCssToHead,
    kOutlineCss,
    kOutlineJavascript,
    kRemoveComments,
    kRemoveQuotes,
    kRewriteCss,
    kRewriteImages,
    kRewriteJavascript,
    kStripScripts,
  };

  enum RewriteLevel {
    // Run in pass-through mode. Parse HTML but do not perform any
    // transformations. This is the default value. Most users should
    // explcitly enable the kCoreFilters level by calling
    // SetRewriteLevel(kCoreFilters).
    kPassThrough,

    // Enable the core set of filters. These filters are considered
    // generally safe for most sites, though even safe filters can
    // break some sites. Most users should specify this option, and
    // then optionally add or remove specific filters based on
    // specific needs.
    kCoreFilters,
  };

  // Used for enumerating over all entries in the Filter enum.
  static const Filter kFirstFilter = kAddBaseTag;
  static const Filter kLastFilter = kStripScripts;

  static const int64 kDefaultCssInlineMaxBytes;
  static const int64 kDefaultImgInlineMaxBytes;
  static const int64 kDefaultJsInlineMaxBytes;
  static const int64 kDefaultCssOutlineMinBytes;
  static const int64 kDefaultJsOutlineMinBytes;
  static const std::string kDefaultBeaconUrl;

  // IE limits URL size overall to about 2k characters.  See
  // http://support.microsoft.com/kb/208427/EN-US
  static const int kMaxUrlSize;

  // See http://code.google.com/p/modpagespeed/issues/detail?id=9
  // Apache evidently limits each URL path segment (between /) to
  // about 256 characters.  This is not fundamental URL limitation
  // but is Apache specific.
  static const int64 kMaxUrlSegmentSize;

  static bool ParseRewriteLevel(const StringPiece& in, RewriteLevel* out);

  RewriteOptions();

  void SetRewriteLevel(RewriteLevel level) { level_ = level; }

  // Adds a set of filters to the enabled set.  Returns false if any
  // of the filter names are invalid, but all the valid ones will be
  // added anyway.
  bool EnableFiltersByCommaSeparatedList(const StringPiece& filters,
                                         MessageHandler* handler);

  // Adds a set of filters to the disabled set.  Returns false if any
  // of the filter names are invalid, but all the valid ones will be
  // added anyway.
  bool DisableFiltersByCommaSeparatedList(const StringPiece& filters,
                                          MessageHandler* handler);
  void Reset();
  void EnableFilter(Filter filter) { enabled_filters_.insert(filter); }
  void DisableFilter(Filter filter) { disabled_filters_.insert(filter); }

  bool Enabled(Filter filter) const;

  int64 css_outline_min_bytes() const { return css_outline_min_bytes_; }
  void set_css_outline_min_bytes(int64 x) { css_outline_min_bytes_ = x; }
  int64 js_outline_min_bytes() const { return js_outline_min_bytes_; }
  void set_js_outline_min_bytes(int64 x) { js_outline_min_bytes_ = x; }
  int64 img_inline_max_bytes() const { return img_inline_max_bytes_; }
  void set_img_inline_max_bytes(int64 x) { img_inline_max_bytes_ = x; }
  int64 css_inline_max_bytes() const { return css_inline_max_bytes_; }
  void set_css_inline_max_bytes(int64 x) { css_inline_max_bytes_ = x; }
  int64 js_inline_max_bytes() const { return js_inline_max_bytes_; }
  void set_js_inline_max_bytes(int64 x) { js_inline_max_bytes_ = x; }
  int num_shards() const { return num_shards_; }
  void set_num_shards(int x) { num_shards_ = x; }
  const std::string& beacon_url() const { return beacon_url_; }
  void set_beacon_url(const StringPiece& p) { p.CopyToString(&beacon_url_); }

  // The maximum length of a URL segment.
  // for http://a/b/c.d, this is == strlen("c.d")
  int max_url_segment_size() const { return max_url_segment_size_; }
  void set_max_url_segment_size(int x) { max_url_segment_size_ = x; }

  // The maximum size of the entire URL.  If '0', this is left unlimited.
  int max_url_size() const { return max_url_size_; }
  void set_max_url_size(int x) { max_url_size_ = x; }

 private:
  typedef std::set<Filter> FilterSet;
  typedef std::map<std::string, Filter> NameToFilterMap;
  typedef std::map<RewriteLevel, FilterSet> RewriteLevelToFilterSetMap;

  void SetUp();
  bool AddCommaSeparatedListToFilterSet(
      const StringPiece& filters, MessageHandler* handler, FilterSet* set);

  NameToFilterMap name_filter_map_;
  RewriteLevelToFilterSetMap level_filter_set_map_;
  FilterSet enabled_filters_;
  FilterSet disabled_filters_;
  RewriteLevel level_;
  int64 css_inline_max_bytes_;
  int64 img_inline_max_bytes_;
  int64 js_inline_max_bytes_;
  int64 css_outline_min_bytes_;
  int64 js_outline_min_bytes_;
  int num_shards_;
  std::string beacon_url_;
  int max_url_segment_size_;  // for http://a/b/c.d, this is == strlen("c.d")
  int max_url_size_;          // but this is strlen("http://a/b/c.d")

  DISALLOW_COPY_AND_ASSIGN(RewriteOptions);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_OPTIONS_H_
