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

#include "net/instaweb/rewriter/public/rewrite_options.h"

#include <vector>
#include "net/instaweb/util/public/message_handler.h"

namespace net_instaweb {

// TODO(jmarantz): consider merging this threshold with the image-inlining
// threshold, which is currently defaulting at 2000, so we have a single
// byte-count threshold, above which inlined resources get outlined, and
// below which outlined resources get inlined.
//
// TODO(jmarantz): user-agent-specific selection of inline threshold so that
// mobile phones are more prone to inlining.
//
// Further notes; jmaessen says:
//
// I suspect we do not want these bounds to match, and inlining for
// images is a bit more complicated because base64 encoding inflates
// the byte count of data: urls.  This is a non-issue for other
// resources (there may be some weirdness with iframes I haven't
// thought about...).
//
// jmarantz says:
//
// One thing we could do, if we believe they should be conceptually
// merged, is in img_rewrite_filter you could apply the
// base64-bloat-factor before comparing against the threshold.  Then
// we could use one number if we like that idea.
//

// jmaessen: For the moment, there's a separate threshold for img inline.
const int64 RewriteOptions::kDefaultCssInlineMaxBytes = 2048;
const int64 RewriteOptions::kDefaultImgInlineMaxBytes = 2048;
const int64 RewriteOptions::kDefaultJsInlineMaxBytes = 2048;
const int64 RewriteOptions::kDefaultCssOutlineMinBytes = 3000;
const int64 RewriteOptions::kDefaultJsOutlineMinBytes = 3000;

// Limit on concurrent ongoing img rewrites.
// TODO(jmaessen): Determine a sane default for this value.
const int RewriteOptions::kDefaultImgMaxRewritesAtOnce = 8;

// IE limits URL size overall to about 2k characters.  See
// http://support.microsoft.com/kb/208427/EN-US
const int RewriteOptions::kMaxUrlSize = 2083;

// See http://code.google.com/p/modpagespeed/issues/detail?id=9
// Apache evidently limits each URL path segment (between /) to
// about 256 characters.  This is not a fundamental URL limitation
// but is Apache specific.  For the moment we will impose the
// Apache limitation in general -- there is concern that even an
// nginx server might fail if resources are then served through
// an Apache proxy.  Until then let's just set a reasonable limit.
const int64 RewriteOptions::kMaxUrlSegmentSize = 250;

const std::string RewriteOptions::kDefaultBeaconUrl =
    "/mod_pagespeed_beacon?ets=";

bool RewriteOptions::ParseRewriteLevel(
    const StringPiece& in, RewriteLevel* out) {
  bool ret = false;
  if (in != NULL) {
    if (strcasecmp(in.data(), "CoreFilters") == 0) {
      *out = kCoreFilters;
      ret = true;
    } else if (strcasecmp(in.data(), "PassThrough") == 0) {
      *out = kPassThrough;
      ret = true;
    }
  }
  return ret;
}

RewriteOptions::RewriteOptions()
    : level_(kPassThrough),
      css_inline_max_bytes_(kDefaultCssInlineMaxBytes),
      img_inline_max_bytes_(kDefaultImgInlineMaxBytes),
      img_max_rewrites_at_once_(kDefaultImgMaxRewritesAtOnce),
      js_inline_max_bytes_(kDefaultJsInlineMaxBytes),
      css_outline_min_bytes_(kDefaultCssInlineMaxBytes),
      js_outline_min_bytes_(kDefaultJsInlineMaxBytes),
      num_shards_(0),
      beacon_url_(kDefaultBeaconUrl),
      max_url_segment_size_(kMaxUrlSegmentSize),
      max_url_size_(kMaxUrlSize),
      enabled_(true) {
  // TODO: If we instantiate many RewriteOptions, this should become a
  // public static method called once at startup.
  SetUp();
}

void RewriteOptions::SetUp() {
  name_filter_map_["add_base_tag"] = kAddBaseTag;
  name_filter_map_["add_head"] = kAddHead;
  name_filter_map_["add_instrumentation"] = kAddInstrumentation;
  name_filter_map_["collapse_whitespace"] = kCollapseWhitespace;
  name_filter_map_["combine_css"] = kCombineCss;
  name_filter_map_["combine_heads"] = kCombineHeads;
  name_filter_map_["debug_log_img_tags"] = kDebugLogImgTags;
  name_filter_map_["elide_attributes"] = kElideAttributes;
  name_filter_map_["extend_cache"] = kExtendCache;
  name_filter_map_["inline_css"] = kInlineCss;
  name_filter_map_["inline_javascript"] = kInlineJavascript;
  name_filter_map_["insert_img_dimensions"] = kInsertImgDimensions;
  name_filter_map_["left_trim_urls"] = kLeftTrimUrls;
  name_filter_map_["move_css_to_head"] = kMoveCssToHead;
  name_filter_map_["outline_css"] = kOutlineCss;
  name_filter_map_["outline_javascript"] = kOutlineJavascript;
  name_filter_map_["remove_comments"] = kRemoveComments;
  name_filter_map_["remove_quotes"] = kRemoveQuotes;
  name_filter_map_["rewrite_css"] = kRewriteCss;
  name_filter_map_["rewrite_images"] = kRewriteImages;
  name_filter_map_["rewrite_javascript"] = kRewriteJavascript;
  name_filter_map_["strip_scripts"] = kStripScripts;

  // Create an empty set for the pass-through level.
  level_filter_set_map_[kPassThrough];

  // Core filter level includes the "core" filter set.
  level_filter_set_map_[kCoreFilters].insert(kAddHead);
  level_filter_set_map_[kCoreFilters].insert(kCombineCss);

  // TODO(jmarantz): re-enable javascript and CSS minification in
  // the core set after the reported bugs have been fixed.  They
  // can still be enabled individually.
  // level_filter_set_map_[kCoreFilters].insert(kRewriteJavascript);
  // level_filter_set_map_[kCoreFilters].insert(kRewriteCss);

  level_filter_set_map_[kCoreFilters].insert(kInlineCss);
  level_filter_set_map_[kCoreFilters].insert(kInlineJavascript);
  level_filter_set_map_[kCoreFilters].insert(kRewriteImages);
  level_filter_set_map_[kCoreFilters].insert(kInsertImgDimensions);
  level_filter_set_map_[kCoreFilters].insert(kExtendCache);
}

bool RewriteOptions::EnableFiltersByCommaSeparatedList(
    const StringPiece& filters, MessageHandler* handler) {
  return AddCommaSeparatedListToFilterSet(
      filters, handler, &enabled_filters_);
}

bool RewriteOptions::DisableFiltersByCommaSeparatedList(
    const StringPiece& filters, MessageHandler* handler) {
  return AddCommaSeparatedListToFilterSet(
      filters, handler, &disabled_filters_);
}

bool RewriteOptions::AddCommaSeparatedListToFilterSet(
    const StringPiece& filters, MessageHandler* handler, FilterSet* set) {
  std::vector<StringPiece> names;
  SplitStringPieceToVector(filters, ",", &names, true);
  bool ret = true;
  for (int i = 0, n = names.size(); i < n; ++i) {
    std::string option(names[i].data(), names[i].size());
    NameToFilterMap::iterator p = name_filter_map_.find(option);
    if (p == name_filter_map_.end()) {
      handler->Message(kWarning, "Invalid filter name: %s", option.c_str());
      ret = false;
    } else {
      set->insert(p->second);
    }
  }
  return ret;
}

bool RewriteOptions::Enabled(Filter filter) const {
  if (disabled_filters_.find(filter) != disabled_filters_.end()) {
    return false;
  }

  RewriteLevelToFilterSetMap::const_iterator it =
      level_filter_set_map_.find(level_.value());
  if (it != level_filter_set_map_.end()) {
    const FilterSet& filters = it->second;
    if (filters.find(filter) != filters.end()) {
      return true;
    }
  }

  return (enabled_filters_.find(filter) != enabled_filters_.end());
}

void RewriteOptions::Merge(const RewriteOptions& first,
                           const RewriteOptions& second) {
  enabled_filters_ = first.enabled_filters_;
  disabled_filters_ = first.disabled_filters_;
  for (FilterSet::const_iterator p = second.enabled_filters_.begin(),
           e = second.enabled_filters_.end(); p != e; ++p) {
    Filter filter = *p;
    disabled_filters_.erase(filter);
    enabled_filters_.insert(filter);
  }
  disabled_filters_.insert(second.disabled_filters_.begin(),
                           second.disabled_filters_.end());

  // TODO(jmarantz): Use a virtual base class for Option so we can put
  // this in a loop.  Or something.
  level_.Merge(first.level_,
               second.level_);
  css_inline_max_bytes_.Merge(first.css_inline_max_bytes_,
                              second.css_inline_max_bytes_);
  img_inline_max_bytes_.Merge(first.img_inline_max_bytes_,
                              second.img_inline_max_bytes_);
  img_max_rewrites_at_once_.Merge(first.img_max_rewrites_at_once_,
                                  second.img_max_rewrites_at_once_);
  js_inline_max_bytes_.Merge(first.js_inline_max_bytes_,
                             second.js_inline_max_bytes_);
  css_outline_min_bytes_.Merge(first.css_outline_min_bytes_,
                               second.css_outline_min_bytes_);
  js_outline_min_bytes_.Merge(first.js_outline_min_bytes_,
                              second.js_outline_min_bytes_);
  num_shards_.Merge(first.num_shards_,
                    second.num_shards_);
  beacon_url_.Merge(first.beacon_url_,
                    second.beacon_url_);
  max_url_segment_size_.Merge(first.max_url_segment_size_,
                              second.max_url_segment_size_);
  max_url_size_.Merge(first.max_url_size_,
                      second.max_url_size_);
}

}  // namespace net_instaweb
