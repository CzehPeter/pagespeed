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

#include "net/instaweb/rewriter/public/rewrite_options.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>

#include "base/logging.h"
#include "net/instaweb/rewriter/panel_config.pb.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/file_load_policy.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/wildcard_group.h"

namespace {

// This version index serves as global signature key.  Much of the
// data emitted in signatures is based on the option ordering, which
// can change as we add new options.  So every time there is a
// binary-incompatible change to the option ordering, we bump this
// version.
//
// Note: we now use a two-letter code for identifying enabled filters, so
// there is no need bump the option version when changing the filter enum.
//
// Updating this value will have the indirect effect of flushing the metadata
// cache.
//
// This version number should be incremented if any default-values are changed,
// either in the add_option() call or via options->set_default.
const int kOptionsVersion = 11;

}  // namespace

namespace net_instaweb {

// RewriteFilter prefixes
const char RewriteOptions::kAjaxRewriteId[] = "aj";
const char RewriteOptions::kCssCombinerId[] = "cc";
const char RewriteOptions::kCssFilterId[] = "cf";
const char RewriteOptions::kCssInlineId[] = "ci";
const char RewriteOptions::kCacheExtenderId[] = "ce";
const char RewriteOptions::kImageCombineId[] = "is";
const char RewriteOptions::kImageCompressionId[] = "ic";
const char RewriteOptions::kJavascriptCombinerId[] = "jc";
const char RewriteOptions::kJavascriptMinId[] = "jm";
const char RewriteOptions::kJavascriptInlineId[] = "ji";
const char RewriteOptions::kPanelCommentPrefix[] = "GooglePanel";

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
// merged, is in image_rewrite_filter you could apply the
// base64-bloat-factor before comparing against the threshold.  Then
// we could use one number if we like that idea.
//
// jmaessen: For the moment, there's a separate threshold for image inline.
const int64 RewriteOptions::kDefaultCssInlineMaxBytes = 2048;
// TODO(jmaessen): Adjust these thresholds in a subsequent CL
// (Will require re-golding tests.)
const int64 RewriteOptions::kDefaultImageInlineMaxBytes = 2048;
const int64 RewriteOptions::kDefaultCssImageInlineMaxBytes = 2048;
const int64 RewriteOptions::kDefaultJsInlineMaxBytes = 2048;
const int64 RewriteOptions::kDefaultCssOutlineMinBytes = 3000;
const int64 RewriteOptions::kDefaultJsOutlineMinBytes = 3000;
const int64 RewriteOptions::kDefaultProgressiveJpegMinBytes = 10240;

const int64 RewriteOptions::kDefaultMaxHtmlCacheTimeMs = 0;
const int64 RewriteOptions::kDefaultMinResourceCacheTimeToRewriteMs = 0;

const int64 RewriteOptions::kDefaultCacheInvalidationTimestamp = -1;
const int64 RewriteOptions::kDefaultIdleFlushTimeMs = 10;

// Limit on concurrent ongoing image rewrites.
// TODO(jmaessen): Determine a sane default for this value.
const int RewriteOptions::kDefaultImageMaxRewritesAtOnce = 8;

// IE limits URL size overall to about 2k characters.  See
// http://support.microsoft.com/kb/208427/EN-US
const int RewriteOptions::kMaxUrlSize = 2083;

// Jpeg quality that needs to be used while recompressing. If set to -1, we
// use source image quality parameters, and is lossless.
const int RewriteOptions::kDefaultImageJpegRecompressQuality = -1;

// Percentage savings in order to retain rewritten images; these default
// to 100% so that we always attempt to resize downsized images, and
// unconditionally retain images if they save any bytes at all.
const int RewriteOptions::kDefaultImageLimitOptimizedPercent = 100;
const int RewriteOptions::kDefaultImageLimitResizeAreaPercent = 100;

// WebP quality that needs to be used while recompressing. If set to -1, we
// use source image quality parameters.
const int RewriteOptions::kDefaultImageWebpRecompressQuality = -1;

// See http://code.google.com/p/modpagespeed/issues/detail?id=9.  By
// default, Apache evidently limits each URL path segment (between /)
// to about 256 characters.  This is not a fundamental URL limitation
// but is Apache specific.  Ben Noordhuis has provided a workaround
// of hooking map_to_storage to skip the directory-mapping phase in
// Apache.  See http://code.google.com/p/modpagespeed/issues/detail?id=176
const int RewriteOptions::kDefaultMaxUrlSegmentSize = 1024;

const GoogleString RewriteOptions::kDefaultBeaconUrl =
    "/mod_pagespeed_beacon?ets=";

const char RewriteOptions::kClassName[] = "RewriteOptions";

namespace {

const RewriteOptions::Filter kCoreFilterSet[] = {
  RewriteOptions::kAddHead,
  RewriteOptions::kCombineCss,
  RewriteOptions::kConvertMetaTags,
  RewriteOptions::kExtendCacheCss,
  RewriteOptions::kExtendCacheImages,
  RewriteOptions::kExtendCacheScripts,
  RewriteOptions::kHtmlWriterFilter,
  RewriteOptions::kInlineCss,
  RewriteOptions::kInlineImages,
  RewriteOptions::kInlineImportToLink,
  RewriteOptions::kInlineJavascript,
  RewriteOptions::kLeftTrimUrls,
  RewriteOptions::kRecompressImages,
  RewriteOptions::kResizeImages,
  RewriteOptions::kRewriteCss,
  RewriteOptions::kRewriteJavascript,
  RewriteOptions::kRewriteStyleAttributesWithUrl,
};

// Note: all Core filters are Test filters as well.  For maintainability,
// this is managed in the c++ switch statement.
const RewriteOptions::Filter kTestFilterSet[] = {
  RewriteOptions::kConvertJpegToWebp,
  RewriteOptions::kInsertImageDimensions,
  RewriteOptions::kMakeGoogleAnalyticsAsync,
  RewriteOptions::kRewriteDomains,
  RewriteOptions::kSpriteImages,
};

// Note: These filters should not be included even if the level is "All".
const RewriteOptions::Filter kDangerousFilterSet[] = {
  RewriteOptions::kDeferJavascript,
  RewriteOptions::kDisableJavascript,
  RewriteOptions::kDivStructure,
  RewriteOptions::kExplicitCloseTags,
  RewriteOptions::kLazyloadImages,
  RewriteOptions::kStripScripts,
};

#ifndef NDEBUG
void CheckFilterSetOrdering(const RewriteOptions::Filter* filters, int num) {
  for (int i = 1; i < num; ++i) {
    DCHECK_GT(filters[i], filters[i - 1]);
  }
}
#endif

bool IsInSet(const RewriteOptions::Filter* filters, int num,
             RewriteOptions::Filter filter) {
  const RewriteOptions::Filter* end = filters + num;
  return std::binary_search(filters, end, filter);
}

}  // namespace

const char* RewriteOptions::FilterName(Filter filter) {
  switch (filter) {
    case kAddHead:                         return "Add Head";
    case kAddInstrumentation:              return "Add Instrumentation";
    case kCollapseWhitespace:              return "Collapse Whitespace";
    case kCombineCss:                      return "Combine Css";
    case kCombineHeads:                    return "Combine Heads";
    case kCombineJavascript:               return "Combine Javascript";
    case kComputeLayout:                   return "Computes layout";
    case kComputePanelJson:                return "Computes panel json";
    case kConvertJpegToProgressive:        return "Convert Jpeg to Progressive";
    case kConvertJpegToWebp:               return "Convert Jpeg To Webp";
    case kConvertMetaTags:                 return "Convert Meta Tags";
    case kConvertPngToJpeg:                return "Convert Png to Jpeg";
    case kDeferJavascript:                 return "Defer Javascript";
    case kDelayImages:                     return "Delay Images";
    case kDisableJavascript:
        return "Disables scripts by placing them inside noscript tags";
    case kDivStructure:                    return "Div Structure";
    case kElideAttributes:                 return "Elide Attributes";
    case kExplicitCloseTags:               return "Explicit Close Tags";
    case kExtendCacheCss:                  return "Cache Extend Css";
    case kExtendCacheImages:               return "Cache Extend Images";
    case kExtendCacheScripts:              return "Cache Extend Scripts";
    case kHtmlWriterFilter:                return "Flushes html";
    case kInlineCss:                       return "Inline Css";
    case kInlineImages:                    return "Inline Images";
    case kInlineImportToLink:              return "Inline @import to Link";
    case kInlineJavascript:                return "Inline Javascript";
    case kInsertImageDimensions:           return "Insert Image Dimensions";
    case kLazyloadImages:                  return "Lazyload Images";
    case kLeftTrimUrls:                    return "Left Trim Urls";
    case kMakeGoogleAnalyticsAsync:        return "Make Google Analytics Async";
    case kMoveCssToHead:                   return "Move Css To Head";
    case kOutlineCss:                      return "Outline Css";
    case kOutlineJavascript:               return "Outline Javascript";
    case kRecompressImages:                return "Recompress Images";
    case kRemoveComments:                  return "Remove Comments";
    case kRemoveQuotes:                    return "Remove Quotes";
    case kResizeImages:                    return "Resize Images";
    case kRewriteCss:                      return "Rewrite Css";
    case kRewriteDomains:                  return "Rewrite Domains";
    case kRewriteJavascript:               return "Rewrite Javascript";
    case kRewriteStyleAttributes:          return "Rewrite Style Attributes";
    case kRewriteStyleAttributesWithUrl:
      return "Rewrite Style Attributes With Url";
    case kSpriteImages:                    return "Sprite Images";
    case kStripScripts:                    return "Strip Scripts";
    case kEndOfFilters:                    return "End of Filters";
  }
  return "Unknown Filter";
}

const char* RewriteOptions::FilterId(Filter filter) {
  switch (filter) {
    case kAddHead:                         return "ah";
    case kAddInstrumentation:              return "ai";
    case kCollapseWhitespace:              return "cw";
    case kCombineCss:                      return kCssCombinerId;
    case kCombineHeads:                    return "ch";
    case kCombineJavascript:               return kJavascriptCombinerId;
    case kComputeLayout:                   return "bl";
    case kComputePanelJson:                return "bp";
    case kConvertJpegToProgressive:        return "jp";
    case kConvertJpegToWebp:               return "jw";
    case kConvertMetaTags:                 return "mc";
    case kConvertPngToJpeg:                return "pj";
    case kDeferJavascript:                 return "dj";
    case kDelayImages:                     return "di";
    case kDisableJavascript:               return "jd";
    case kDivStructure:                    return "ds";
    case kElideAttributes:                 return "ea";
    case kExplicitCloseTags:               return "xc";
    case kExtendCacheCss:                  return "ec";
    case kExtendCacheImages:               return "ei";
    case kExtendCacheScripts:              return "es";
    case kHtmlWriterFilter:                return "hw";
    case kInlineCss:                       return kCssInlineId;
    case kInlineImages:                    return "ii";
    case kInlineImportToLink:              return "il";
    case kInlineJavascript:                return kJavascriptInlineId;
    case kInsertImageDimensions:           return "id";
    case kLazyloadImages:                  return "ll";
    case kLeftTrimUrls:                    return "tu";
    case kMakeGoogleAnalyticsAsync:        return "ga";
    case kMoveCssToHead:                   return "cm";
    case kOutlineCss:                      return "co";
    case kOutlineJavascript:               return "jo";
    case kRecompressImages:                return "ir";
    case kRemoveComments:                  return "rc";
    case kRemoveQuotes:                    return "rq";
    case kResizeImages:                    return "ri";
    case kRewriteCss:                      return kCssFilterId;
    case kRewriteDomains:                  return "rd";
    case kRewriteJavascript:               return kJavascriptMinId;
    case kRewriteStyleAttributes:          return "cs";
    case kRewriteStyleAttributesWithUrl:   return "cu";
    case kSpriteImages:                    return kImageCombineId;
    case kStripScripts:                    return "ss";
    case kEndOfFilters:
      LOG(DFATAL) << "EndOfFilters passed as code: " << filter;
      return "EF";
  }
  LOG(DFATAL) << "Unknown filter code: " << filter;
  return "UF";
}

bool RewriteOptions::ParseRewriteLevel(
    const StringPiece& in, RewriteLevel* out) {
  bool ret = false;
  if (in != NULL) {
    if (StringCaseEqual(in, "CoreFilters")) {
      *out = kCoreFilters;
      ret = true;
    } else if (StringCaseEqual(in, "PassThrough")) {
      *out = kPassThrough;
      ret = true;
    } else if (StringCaseEqual(in, "TestingCoreFilters")) {
      *out = kTestingCoreFilters;
      ret = true;
    } else if (StringCaseEqual(in, "AllFilters")) {
      *out = kAllFilters;
      ret = true;
    }
  }
  return ret;
}

RewriteOptions::RewriteOptions()
    : modified_(false),
      frozen_(false),
      options_uniqueness_checked_(false) {
  // Sanity-checks -- will be active only when compiled for debug.
#ifndef NDEBUG
  CheckFilterSetOrdering(kCoreFilterSet, arraysize(kCoreFilterSet));
  CheckFilterSetOrdering(kTestFilterSet, arraysize(kTestFilterSet));
  CheckFilterSetOrdering(kDangerousFilterSet, arraysize(kDangerousFilterSet));

  // Ensure that all filters have unique IDs.
  StringSet id_set;
  for (int i = 0; i < static_cast<int>(kEndOfFilters); ++i) {
    Filter filter = static_cast<Filter>(i);
    const char* id = FilterId(filter);
    std::pair<StringSet::iterator, bool> insertion = id_set.insert(id);
    DCHECK(insertion.second) << "Duplicate RewriteOption filter id: " << id;
  }

  // We can't check options uniqueness until additional extra
  // options are added by subclasses.  We could do this in the
  // destructor I suppose, but we defer it till ComputeSignature.
#endif

  // TODO(jmarantz): consider adding these on demand so that the cost of
  // initializing an empty RewriteOptions object is closer to zero.
  add_option(kPassThrough, &level_, "l");
  add_option(kDefaultCssInlineMaxBytes, &css_inline_max_bytes_, "ci");
  add_option(kDefaultImageInlineMaxBytes, &image_inline_max_bytes_, "ii");
  add_option(kDefaultCssImageInlineMaxBytes, &css_image_inline_max_bytes_,
             "cii");
  add_option(kDefaultJsInlineMaxBytes, &js_inline_max_bytes_, "ji");
  add_option(kDefaultCssOutlineMinBytes, &css_outline_min_bytes_, "co");
  add_option(kDefaultJsOutlineMinBytes, &js_outline_min_bytes_, "jo");
  add_option(kDefaultProgressiveJpegMinBytes,
             &progressive_jpeg_min_bytes_, "jp");
  add_option(kDefaultMaxHtmlCacheTimeMs, &max_html_cache_time_ms_, "hc");
  add_option(kDefaultMinResourceCacheTimeToRewriteMs,
             &min_resource_cache_time_to_rewrite_ms_, "rc");
  add_option(kDefaultCacheInvalidationTimestamp,
             &cache_invalidation_timestamp_, "it");
  add_option(kDefaultIdleFlushTimeMs, &idle_flush_time_ms_, "if");
  add_option(kDefaultImageMaxRewritesAtOnce, &image_max_rewrites_at_once_,
             "im");
  add_option(kDefaultMaxUrlSegmentSize, &max_url_segment_size_, "uss");
  add_option(kMaxUrlSize, &max_url_size_, "us");
  add_option(true, &enabled_, "e");
  add_option(false, &ajax_rewriting_enabled_, "ar");
  add_option(false, &botdetect_enabled_, "be");
  add_option(true, &combine_across_paths_, "cp");
  add_option(false, &log_rewrite_timing_, "lr");
  add_option(false, &lowercase_html_names_, "lh");
  add_option(false, &always_rewrite_css_, "arc");
  add_option(false, &respect_vary_, "rv");
  add_option(false, &flush_html_, "fh");
  add_option(true, &serve_stale_if_fetch_error_, "ss");
  add_option(false, &enable_blink_, "eb");
  add_option(kDefaultBeaconUrl, &beacon_url_, "bu");
  add_option(kDefaultImageJpegRecompressQuality,
             &image_jpeg_recompress_quality_, "iq");
  add_option(kDefaultImageLimitOptimizedPercent,
             &image_limit_optimized_percent_, "ip");
  add_option(kDefaultImageLimitResizeAreaPercent,
             &image_limit_resize_area_percent_, "ia");
  add_option(kDefaultImageWebpRecompressQuality,
             &image_webp_recompress_quality_, "iw");

  // Enable HtmlWriterFilter by default.
  EnableFilter(kHtmlWriterFilter);
}

RewriteOptions::~RewriteOptions() {
}

RewriteOptions::OptionBase::~OptionBase() {
}

void RewriteOptions::set_panel_config(
    PublisherConfig* panel_config) {
  panel_config_.reset(panel_config);
}

const PublisherConfig* RewriteOptions::panel_config() const {
  return panel_config_.get();
}

void RewriteOptions::DisallowTroublesomeResources() {
  // http://code.google.com/p/modpagespeed/issues/detail?id=38
  Disallow("*js_tinyMCE*");  // js_tinyMCE.js
  // Official tinyMCE URLs: tiny_mce.js, tiny_mce_src.js, tiny_mce_gzip.php, ...
  Disallow("*tiny_mce*");
  // I've also seen tinymce.js
  Disallow("*tinymce*");

  // http://code.google.com/p/modpagespeed/issues/detail?id=352
  Disallow("*scriptaculous.js*");

  // Breaks some sites.
  Disallow("*connect.facebook.net/*");

  // http://code.google.com/p/modpagespeed/issues/detail?id=186
  // ckeditor.js, ckeditor_basic.js, ckeditor_basic_source.js, ...
  Disallow("*ckeditor*");

  // http://code.google.com/p/modpagespeed/issues/detail?id=207
  // jquery-ui-1.8.2.custom.min.js, jquery-1.4.4.min.js, jquery.fancybox-...
  //
  // TODO(sligocki): Is jquery actually a problem? Perhaps specific
  // jquery libraries (like tiny MCE). Investigate before disabling.
  // Disallow("*jquery*");

  // http://code.google.com/p/modpagespeed/issues/detail?id=216
  // Appears to be an issue with old version of jsminify.
  // Disallow("*swfobject*");  // swfobject.js

  // TODO(sligocki): Add disallow for the JS broken in:
  // http://code.google.com/p/modpagespeed/issues/detail?id=142
  // Not clear which JS file is broken and proxying is not working correctly.

  if (Enabled(kComputeLayout) || Enabled(kComputePanelJson)) {
    RetainComment(StrCat(kPanelCommentPrefix, "*"));
  }
}

bool RewriteOptions::AdjustFiltersByCommaSeparatedList(
    const StringPiece& filters, MessageHandler* handler) {
  return AddCommaSeparatedListToPlusAndMinusFilterSets(
      filters, handler, &enabled_filters_, &disabled_filters_);
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

void RewriteOptions::DisableAllFiltersNotExplicitlyEnabled() {
  for (int f = kFirstFilter; f != kEndOfFilters; ++f) {
    Filter filter = static_cast<Filter>(f);
    if (enabled_filters_.find(filter) == enabled_filters_.end()) {
      DisableFilter(filter);
    }
  }
}

void RewriteOptions::EnableFilter(Filter filter) {
  DCHECK(!frozen_);
  std::pair<FilterSet::iterator, bool> inserted =
      enabled_filters_.insert(filter);
  modified_ |= inserted.second;
}

void RewriteOptions::ForceEnableFilter(Filter filter) {
  DCHECK(!frozen_);

  // insert into set of enabled filters.
  std::pair<FilterSet::iterator, bool> inserted =
      enabled_filters_.insert(filter);
  modified_ |= inserted.second;

  // remove from set of disabled filters.
  modified_ |= disabled_filters_.erase(filter);
}

void RewriteOptions::EnableExtendCacheFilters() {
  EnableFilter(kExtendCacheCss);
  EnableFilter(kExtendCacheImages);
  EnableFilter(kExtendCacheScripts);
}

void RewriteOptions::DisableFilter(Filter filter) {
  DCHECK(!frozen_);
  std::pair<FilterSet::iterator, bool> inserted =
      disabled_filters_.insert(filter);
  modified_ |= inserted.second;
}

void RewriteOptions::EnableFilters(
    const RewriteOptions::FilterSet& filter_set) {
  for (RewriteOptions::FilterSet::const_iterator iter = filter_set.begin();
       iter != filter_set.end(); ++iter) {
    EnableFilter(*iter);
  }
}

void RewriteOptions::DisableFilters(
    const RewriteOptions::FilterSet& filter_set) {
  for (RewriteOptions::FilterSet::const_iterator iter = filter_set.begin();
       iter != filter_set.end(); ++iter) {
    DisableFilter(*iter);
  }
}

bool RewriteOptions::AddCommaSeparatedListToFilterSet(
    const StringPiece& filters, MessageHandler* handler, FilterSet* set) {
  DCHECK(!frozen_);
  StringPieceVector names;
  SplitStringPieceToVector(filters, ",", &names, true);
  bool ret = true;
  size_t prev_set_size = set->size();
  for (int i = 0, n = names.size(); i < n; ++i) {
    ret = AddOptionToFilterSet(names[i], handler, set);
  }
  modified_ |= (set->size() != prev_set_size);
  return ret;
}

bool RewriteOptions::AddCommaSeparatedListToPlusAndMinusFilterSets(
    const StringPiece& filters, MessageHandler* handler,
    FilterSet* plus_set, FilterSet* minus_set) {
  DCHECK(!frozen_);
  StringPieceVector names;
  SplitStringPieceToVector(filters, ",", &names, true);
  bool ret = true;
  size_t sets_size_sum_before = (plus_set->size() + minus_set->size());
  for (int i = 0, n = names.size(); i < n; ++i) {
    StringPiece& option = names[i];
    if (!option.empty()) {
      if (option[0] == '-') {
        option.remove_prefix(1);
        ret = AddOptionToFilterSet(names[i], handler, minus_set);
      } else if (option[0] == '+') {
        option.remove_prefix(1);
        ret = AddOptionToFilterSet(names[i], handler, plus_set);
      } else {
        // No prefix is treated the same as '+'. Arbitrary but reasonable.
        ret = AddOptionToFilterSet(names[i], handler, plus_set);
      }
    }
  }
  size_t sets_size_sum_after = (plus_set->size() + minus_set->size());
  modified_ |= (sets_size_sum_before != sets_size_sum_after);
  return ret;
}

bool RewriteOptions::AddOptionToFilterSet(
    const StringPiece& option, MessageHandler* handler, FilterSet* set) {
  bool ret = true;
  Filter filter = Lookup(option);
  if (filter == kEndOfFilters) {
    // Handle a compound filter name.  This is much less common, so we don't
    // have any special infrastructure for it; just code.
    if (option == "rewrite_images") {
      set->insert(kInlineImages);
      set->insert(kRecompressImages);
      set->insert(kResizeImages);
    } else if (option == "extend_cache") {
      set->insert(kExtendCacheCss);
      set->insert(kExtendCacheImages);
      set->insert(kExtendCacheScripts);
    } else if (option == "testing") {
      for (int i = 0, n = arraysize(kTestFilterSet); i < n; ++i) {
        set->insert(kTestFilterSet[i]);
      }
    } else if (option == "core") {
      for (int i = 0, n = arraysize(kCoreFilterSet); i < n; ++i) {
        set->insert(kCoreFilterSet[i]);
      }
    } else if (option == "dangerous") {
      for (int i = 0, n = arraysize(kDangerousFilterSet); i < n; ++i) {
        set->insert(kDangerousFilterSet[i]);
      }
    } else {
      handler->Message(kWarning, "Invalid filter name: %s",
                       option.as_string().c_str());
      ret = false;
    }
  } else {
    set->insert(filter);
  }
  return ret;
}

bool RewriteOptions::Enabled(Filter filter) const {
  if (disabled_filters_.find(filter) != disabled_filters_.end()) {
    return false;
  }
  switch (level_.value()) {
    case kTestingCoreFilters:
      if (IsInSet(kTestFilterSet, arraysize(kTestFilterSet), filter)) {
        return true;
      }
      // fall through
    case kCoreFilters:
      if (IsInSet(kCoreFilterSet, arraysize(kCoreFilterSet), filter)) {
        return true;
      }
      break;
    case kAllFilters:
      if (!IsInSet(kDangerousFilterSet, arraysize(kDangerousFilterSet),
                   filter)) {
        return true;
      }
      break;
    case kPassThrough:
      break;
  }
  return enabled_filters_.find(filter) != enabled_filters_.end();
}

int64 RewriteOptions::ImageInlineMaxBytes() const {
  if (Enabled(kInlineImages)) {
    return image_inline_max_bytes_.value();
  } else {
    return 0;
  }
}

void RewriteOptions::set_image_inline_max_bytes(int64 x) {
  set_option(x, &image_inline_max_bytes_);
  if (!css_image_inline_max_bytes_.was_set() &&
      x > css_image_inline_max_bytes_.value()) {
    // Make sure css_image_inline_max_bytes is at least image_inline_max_bytes
    // if it has not been explicitly configured.
    css_image_inline_max_bytes_.set(x);
  }
}

int64 RewriteOptions::CssImageInlineMaxBytes() const {
  if (Enabled(kInlineImages)) {
    return css_image_inline_max_bytes_.value();
  } else {
    return 0;
  }
}

int64 RewriteOptions::MaxImageInlineMaxBytes() const {
  return std::max(ImageInlineMaxBytes(),
                  CssImageInlineMaxBytes());
}

void RewriteOptions::Merge(const RewriteOptions& first,
                           const RewriteOptions& second) {
  DCHECK(!frozen_);
  modified_ = first.modified_ || second.modified_;
  enabled_filters_ = first.enabled_filters_;
  disabled_filters_ = first.disabled_filters_;
  for (FilterSet::const_iterator p = second.enabled_filters_.begin(),
           e = second.enabled_filters_.end(); p != e; ++p) {
    Filter filter = *p;
    // Enabling in 'second' trumps Disabling in first.
    disabled_filters_.erase(filter);
    enabled_filters_.insert(filter);
  }

  for (FilterSet::const_iterator p = second.disabled_filters_.begin(),
           e = second.disabled_filters_.end(); p != e; ++p) {
    Filter filter = *p;
    // Disabling in 'second' trumps enabling in anything.
    disabled_filters_.insert(filter);
    enabled_filters_.erase(filter);
  }

  // Note that from the perspective of this class, we can be merging
  // RewriteOptions subclasses & superclasses, so don't read anything
  // that doesn't exist.  However this is almost certainly the wrong
  // thing to do -- we should ensure that within a system all the
  // RewriteOptions that are instantiated are the same sublcass, so
  // DCHECK that they have the same number of options.
  size_t options_to_read = std::max(first.all_options_.size(),
                                    second.all_options_.size());
  DCHECK_EQ(first.all_options_.size(), second.all_options_.size());
  DCHECK_EQ(options_to_read, all_options_.size());
  size_t options_to_merge = std::min(options_to_read, all_options_.size());
  for (size_t i = 0; i < options_to_merge; ++i) {
    // Be careful to merge only options that exist in all three.
    // TODO(jmarantz): this logic is not 100% sound if there are two
    // different subclasses in play.  We should resolve this at a higher
    // level and assert that the option subclasses are the same.
    if (i >= first.all_options_.size()) {
      all_options_[i]->Merge(second.all_options_[i], second.all_options_[i]);
    } else if (i >= second.all_options_.size()) {
      all_options_[i]->Merge(first.all_options_[i], first.all_options_[i]);
    } else {
      all_options_[i]->Merge(first.all_options_[i], second.all_options_[i]);
    }
  }

  // Pick the larger of the two cache invalidation timestamps. Following
  // calculation assumes the default value of cache invalidation timestamp
  // to be -1.
  //
  // Note: this gets merged by order in the above loop, and then this
  // block of code overrides the merged value.
  //
  // TODO(jmarantz): fold this logic into a new OptionBase subclass whose
  // Merge method does the right thing.
  if (first.cache_invalidation_timestamp_.value() !=
      RewriteOptions::kDefaultCacheInvalidationTimestamp ||
      second.cache_invalidation_timestamp_.value() !=
          RewriteOptions::kDefaultCacheInvalidationTimestamp) {
    cache_invalidation_timestamp_.set(
        std::max(first.cache_invalidation_timestamp_.value(),
                 second.cache_invalidation_timestamp_.value()));
  }

  // Note that the domain-lawyer merge works one-at-a-time, which is easier
  // to unit test.  So we have to call it twice.
  domain_lawyer_.Merge(first.domain_lawyer_);
  domain_lawyer_.Merge(second.domain_lawyer_);

  file_load_policy_.Merge(first.file_load_policy_);
  file_load_policy_.Merge(second.file_load_policy_);

  allow_resources_.CopyFrom(first.allow_resources_);
  allow_resources_.AppendFrom(second.allow_resources_);

  retain_comments_.CopyFrom(first.retain_comments_);
  retain_comments_.AppendFrom(second.retain_comments_);

  if (second.panel_config() != NULL) {
    set_panel_config(new PublisherConfig(*(second.panel_config())));
  } else if (first.panel_config() != NULL) {
    set_panel_config(new PublisherConfig(*(first.panel_config())));
  }
}

RewriteOptions* RewriteOptions::Clone() const {
  RewriteOptions* options = new RewriteOptions;
  options->CopyFrom(*this);

  const PublisherConfig* config = panel_config();
  if (config != NULL) {
    options->set_panel_config(new PublisherConfig(*config));
  }
  return options;
}

GoogleString RewriteOptions::OptionSignature(const GoogleString& x,
                                             const Hasher* hasher) {
  return hasher->Hash(x);
}

GoogleString RewriteOptions::OptionSignature(RewriteLevel level,
                                             const Hasher* hasher) {
  switch (level) {
    case kPassThrough: return "p";
    case kCoreFilters: return "c";
    case kTestingCoreFilters: return "t";
    case kAllFilters: return "a";
  }
  return "?";
}

void RewriteOptions::ComputeSignature(const Hasher* hasher) {
  if (frozen_) {
    return;
  }

#ifndef NDEBUG
  if (!options_uniqueness_checked_) {
    options_uniqueness_checked_ = true;
    StringSet id_set;
    for (int i = 0, n = all_options_.size(); i < n; ++i) {
      const char* id = all_options_[i]->id();
      std::pair<StringSet::iterator, bool> insertion = id_set.insert(id);
      DCHECK(insertion.second) << "Duplicate RewriteOption option id: " << id;
    }
  }
#endif

  signature_ = IntegerToString(kOptionsVersion);
  for (int i = kFirstFilter; i != kEndOfFilters; ++i) {
    Filter filter = static_cast<Filter>(i);
    if (Enabled(filter)) {
      StrAppend(&signature_, "_", FilterId(filter));
    }
  }
  signature_ += "O";
  for (int i = 0, n = all_options_.size(); i < n; ++i) {
    // Keep the signature relatively short by only including options
    // with values overridden from the default.
    OptionBase* option = all_options_[i];
    if (option->was_set()) {
      StrAppend(&signature_, option->id(), ":", option->Signature(hasher), "_");
    }
  }
  StrAppend(&signature_, domain_lawyer_.Signature(), "_");
  frozen_ = true;

  // TODO(jmarantz): Incorporate signatures from the domain_lawyer,
  // file_load_policy, allow_resources, and retain_comments.  However,
  // the changes made here make our system strictly more correct than
  // it was before, using an ad-hoc signature in css_filter.cc.
}

GoogleString RewriteOptions::ToString(RewriteLevel level) {
  switch (level) {
    case kPassThrough: return "Pass Through";
    case kCoreFilters: return "Core Filters";
    case kTestingCoreFilters: return "Testing Core Filters";
    case kAllFilters: return "All Filters";
  }
  return "?";
}

GoogleString RewriteOptions::ToString() const {
  GoogleString output;
  StrAppend(&output, "Version: ", IntegerToString(kOptionsVersion), "\n\n");
  output += "Filters\n";
  for (int i = kFirstFilter; i != kEndOfFilters; ++i) {
    Filter filter = static_cast<Filter>(i);
    if (Enabled(filter)) {
      StrAppend(&output, FilterId(filter), "\t", FilterName(filter), "\n");
    }
  }
  output += "\nOptions\n";
  for (int i = 0, n = all_options_.size(); i < n; ++i) {
    // Only including options with values overridden from the default.
    OptionBase* option = all_options_[i];
    if (option->was_set()) {
      StrAppend(&output, "  ", option->id(), "\t", option->ToString(), "\n");
    }
  }
  // TODO(mmohabey): Incorporate ToString() from the domain_lawyer,
  // file_load_policy, allow_resources, and retain_comments.
  return output;
}

void RewriteOptions::Modify() {
  DCHECK(!frozen_);
  modified_ = true;
}

const char* RewriteOptions::class_name() const {
  return RewriteOptions::kClassName;
}

}  // namespace net_instaweb
