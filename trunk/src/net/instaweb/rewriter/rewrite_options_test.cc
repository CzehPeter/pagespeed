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

// Author: bmcquade@google.com (Bryan McQuade)

#include "net/instaweb/rewriter/public/rewrite_options.h"

#include <set>

#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/null_message_handler.h"
#include "net/instaweb/util/public/string.h"

namespace {

using net_instaweb::MessageHandler;
using net_instaweb::NullMessageHandler;
using net_instaweb::RewriteOptions;

class RewriteOptionsTest : public ::testing::Test {
 protected:
  typedef std::set<RewriteOptions::Filter> FilterSet;
  bool NoneEnabled() {
    FilterSet s;
    return OnlyEnabled(s);
  }

  bool OnlyEnabled(const FilterSet& filters) {
    bool ret = true;
    for (RewriteOptions::Filter f = RewriteOptions::kFirstFilter;
         ret && (f < RewriteOptions::kEndOfFilters);
         f = static_cast<RewriteOptions::Filter>(f + 1)) {
      if (filters.find(f) != filters.end()) {
        if (!options_.Enabled(f)) {
          ret = false;
        }
      } else {
        if (options_.Enabled(f)) {
          ret = false;
        }
      }
    }
    return ret;
  }

  bool OnlyEnabled(RewriteOptions::Filter filter) {
    FilterSet s;
    s.insert(filter);
    return OnlyEnabled(s);
  }

  void MergeOptions(const RewriteOptions& one, const RewriteOptions& two) {
    options_.Merge(one);
    options_.Merge(two);
  }

  // Tests either SetOptionFromName or SetOptionFromNameAndLog depending
  // on 'test_log_variant'
  void TestNameSet(RewriteOptions::OptionSettingResult expected_result,
                   bool test_log_variant,
                   const StringPiece& name,
                   const StringPiece& value,
                   MessageHandler* handler) {
    if (test_log_variant) {
      bool expected = (expected_result == RewriteOptions::kOptionOk);
      EXPECT_EQ(
          expected,
          options_.SetOptionFromNameAndLog(name, value.as_string(), handler));
    } else {
      GoogleString msg;
      EXPECT_EQ(expected_result,
                options_.SetOptionFromName(name, value.as_string(), &msg));
      // Should produce a message exactly when not OK.
      EXPECT_EQ(expected_result != RewriteOptions::kOptionOk, !msg.empty())
          << msg;
    }
  }

  void TestSetOptionFromName(bool test_log_variant);

  RewriteOptions options_;
};

TEST_F(RewriteOptionsTest, BotDetectEnabledByDefault) {
  ASSERT_FALSE(options_.botdetect_enabled());
}

TEST_F(RewriteOptionsTest, BotDetectEnable) {
  options_.set_botdetect_enabled(true);
  ASSERT_TRUE(options_.botdetect_enabled());
}

TEST_F(RewriteOptionsTest, BotDetectDisable) {
  options_.set_botdetect_enabled(false);
  ASSERT_FALSE(options_.botdetect_enabled());
}

TEST_F(RewriteOptionsTest, DefaultEnabledFilters) {
  ASSERT_TRUE(OnlyEnabled(RewriteOptions::kHtmlWriterFilter));
}

TEST_F(RewriteOptionsTest, InstrumentationDisabled) {
  // Make sure the kCoreFilters enables some filters.
  options_.SetRewriteLevel(RewriteOptions::kCoreFilters);
  ASSERT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheCss));
  ASSERT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheImages));

  // Now disable all filters and make sure none are enabled.
  for (RewriteOptions::Filter f = RewriteOptions::kFirstFilter;
       f < RewriteOptions::kEndOfFilters;
       f = static_cast<RewriteOptions::Filter>(f + 1)) {
    options_.DisableFilter(f);
  }
  ASSERT_TRUE(NoneEnabled());
}

TEST_F(RewriteOptionsTest, DisableTrumpsEnable) {
  // Disable the default filter.
  options_.DisableFilter(RewriteOptions::kHtmlWriterFilter);
  for (RewriteOptions::Filter f = RewriteOptions::kFirstFilter;
       f < RewriteOptions::kEndOfFilters;
       f = static_cast<RewriteOptions::Filter>(f + 1)) {
    options_.DisableFilter(f);
    options_.EnableFilter(f);
  }
}

TEST_F(RewriteOptionsTest, ForceEnableFilter) {
  options_.DisableFilter(RewriteOptions::kHtmlWriterFilter);
  options_.EnableFilter(RewriteOptions::kHtmlWriterFilter);
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kHtmlWriterFilter));

  options_.ForceEnableFilter(RewriteOptions::kHtmlWriterFilter);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kHtmlWriterFilter));
}

TEST_F(RewriteOptionsTest, CoreFilters) {
  options_.SetRewriteLevel(RewriteOptions::kCoreFilters);
  FilterSet s;
  for (RewriteOptions::Filter f = RewriteOptions::kFirstFilter;
       f < RewriteOptions::kEndOfFilters;
       f = static_cast<RewriteOptions::Filter>(f + 1)) {
    if (options_.Enabled(f)) {
      s.insert(f);
    }
  }

  // Make sure that more than one filter is enabled in the core filter
  // set.
  ASSERT_GT(s.size(), 1);
}

TEST_F(RewriteOptionsTest, Enable) {
  FilterSet s;
  for (RewriteOptions::Filter f = RewriteOptions::kFirstFilter;
       f < RewriteOptions::kEndOfFilters;
       f = static_cast<RewriteOptions::Filter>(f + 1)) {
    s.insert(f);
    s.insert(RewriteOptions::kHtmlWriterFilter);  // enabled by default
    options_.EnableFilter(f);
    ASSERT_TRUE(OnlyEnabled(s));
  }
}

TEST_F(RewriteOptionsTest, CommaSeparatedList) {
  FilterSet s;
  s.insert(RewriteOptions::kAddInstrumentation);
  s.insert(RewriteOptions::kLeftTrimUrls);
  s.insert(RewriteOptions::kHtmlWriterFilter);  // enabled by default
  const char* kList = "add_instrumentation,trim_urls";
  NullMessageHandler handler;
  ASSERT_TRUE(
      options_.EnableFiltersByCommaSeparatedList(kList, &handler));
  ASSERT_TRUE(OnlyEnabled(s));
  ASSERT_TRUE(
      options_.DisableFiltersByCommaSeparatedList(kList, &handler));
  ASSERT_TRUE(OnlyEnabled(RewriteOptions::kHtmlWriterFilter));  // default
}

TEST_F(RewriteOptionsTest, CompoundFlag) {
  FilterSet s;
  // TODO(jmaessen): add kConvertJpegToWebp here when it becomes part of
  // rewrite_images.
  s.insert(RewriteOptions::kInlineImages);
  s.insert(RewriteOptions::kRecompressImages);
  s.insert(RewriteOptions::kResizeImages);
  s.insert(RewriteOptions::kHtmlWriterFilter);  // enabled by default
  const char* kList = "rewrite_images";
  NullMessageHandler handler;
  ASSERT_TRUE(
      options_.EnableFiltersByCommaSeparatedList(kList, &handler));
  ASSERT_TRUE(OnlyEnabled(s));
  ASSERT_TRUE(
      options_.DisableFiltersByCommaSeparatedList(kList, &handler));
  ASSERT_TRUE(OnlyEnabled(RewriteOptions::kHtmlWriterFilter));  // default
}

TEST_F(RewriteOptionsTest, ParseRewriteLevel) {
  RewriteOptions::RewriteLevel level;
  ASSERT_TRUE(RewriteOptions::ParseRewriteLevel("PassThrough", &level));
  ASSERT_EQ(RewriteOptions::kPassThrough, level);

  ASSERT_TRUE(RewriteOptions::ParseRewriteLevel("CoreFilters", &level));
  ASSERT_EQ(RewriteOptions::kCoreFilters, level);

  ASSERT_FALSE(RewriteOptions::ParseRewriteLevel(NULL, &level));
  ASSERT_FALSE(RewriteOptions::ParseRewriteLevel("", &level));
  ASSERT_FALSE(RewriteOptions::ParseRewriteLevel("Garbage", &level));
}

TEST_F(RewriteOptionsTest, MergeLevelsDefault) {
  RewriteOptions one, two;
  MergeOptions(one, two);
  EXPECT_EQ(RewriteOptions::kPassThrough, options_.level());
}

TEST_F(RewriteOptionsTest, MergeLevelsOneCore) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kCoreFilters);
  MergeOptions(one, two);
  EXPECT_EQ(RewriteOptions::kCoreFilters, options_.level());
}

TEST_F(RewriteOptionsTest, MergeLevelsOneCoreTwoPass) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kCoreFilters);
  two.SetRewriteLevel(RewriteOptions::kPassThrough);  // overrides default
  MergeOptions(one, two);
  EXPECT_EQ(RewriteOptions::kPassThrough, options_.level());
}

TEST_F(RewriteOptionsTest, MergeLevelsOnePassTwoCore) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kPassThrough);  // overrides default
  two.SetRewriteLevel(RewriteOptions::kCoreFilters);  // overrides one
  MergeOptions(one, two);
  EXPECT_EQ(RewriteOptions::kCoreFilters, options_.level());
}

TEST_F(RewriteOptionsTest, MergeLevelsBothCore) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kCoreFilters);
  two.SetRewriteLevel(RewriteOptions::kCoreFilters);
  MergeOptions(one, two);
  EXPECT_EQ(RewriteOptions::kCoreFilters, options_.level());
}

TEST_F(RewriteOptionsTest, MergeFilterPassThrough) {
  RewriteOptions one, two;
  MergeOptions(one, two);
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kAddHead));
}

TEST_F(RewriteOptionsTest, MergeFilterEnaOne) {
  RewriteOptions one, two;
  one.EnableFilter(RewriteOptions::kAddHead);
  MergeOptions(one, two);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kAddHead));
}

TEST_F(RewriteOptionsTest, MergeFilterEnaTwo) {
  RewriteOptions one, two;
  two.EnableFilter(RewriteOptions::kAddHead);
  MergeOptions(one, two);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kAddHead));
}

TEST_F(RewriteOptionsTest, MergeFilterEnaOneDisTwo) {
  RewriteOptions one, two;
  one.EnableFilter(RewriteOptions::kAddHead);
  two.DisableFilter(RewriteOptions::kAddHead);
  MergeOptions(one, two);
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kAddHead));
}

TEST_F(RewriteOptionsTest, MergeFilterDisOneEnaTwo) {
  RewriteOptions one, two;
  one.DisableFilter(RewriteOptions::kAddHead);
  two.EnableFilter(RewriteOptions::kAddHead);
  MergeOptions(one, two);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kAddHead));
}

TEST_F(RewriteOptionsTest, MergeCoreFilter) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kCoreFilters);
  MergeOptions(one, two);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheCss));
}

TEST_F(RewriteOptionsTest, MergeCoreFilterEnaOne) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kCoreFilters);
  one.EnableFilter(RewriteOptions::kExtendCacheCss);
  MergeOptions(one, two);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheCss));
}

TEST_F(RewriteOptionsTest, MergeCoreFilterEnaTwo) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kCoreFilters);
  two.EnableFilter(RewriteOptions::kExtendCacheCss);
  MergeOptions(one, two);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheCss));
}

TEST_F(RewriteOptionsTest, MergeCoreFilterEnaOneDisTwo) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kCoreFilters);
  one.EnableFilter(RewriteOptions::kExtendCacheImages);
  two.DisableFilter(RewriteOptions::kExtendCacheImages);
  MergeOptions(one, two);
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kExtendCacheImages));
}

TEST_F(RewriteOptionsTest, MergeCoreFilterDisOne) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kCoreFilters);
  one.DisableFilter(RewriteOptions::kExtendCacheCss);
  MergeOptions(one, two);
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kExtendCacheCss));
}

TEST_F(RewriteOptionsTest, MergeCoreFilterDisOneEnaTwo) {
  RewriteOptions one, two;
  one.SetRewriteLevel(RewriteOptions::kCoreFilters);
  one.DisableFilter(RewriteOptions::kExtendCacheScripts);
  two.EnableFilter(RewriteOptions::kExtendCacheScripts);
  MergeOptions(one, two);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheScripts));
}

TEST_F(RewriteOptionsTest, MergeThresholdDefault) {
  RewriteOptions one, two;
  MergeOptions(one, two);
  EXPECT_EQ(RewriteOptions::kDefaultCssInlineMaxBytes,
            options_.css_inline_max_bytes());
}

TEST_F(RewriteOptionsTest, MergeThresholdOne) {
  RewriteOptions one, two;
  one.set_css_inline_max_bytes(5);
  MergeOptions(one, two);
  EXPECT_EQ(5, options_.css_inline_max_bytes());
}

TEST_F(RewriteOptionsTest, MergeThresholdTwo) {
  RewriteOptions one, two;
  two.set_css_inline_max_bytes(6);
  MergeOptions(one, two);
  EXPECT_EQ(6, options_.css_inline_max_bytes());
}

TEST_F(RewriteOptionsTest, MergeThresholdOverride) {
  RewriteOptions one, two;
  one.set_css_inline_max_bytes(5);
  two.set_css_inline_max_bytes(6);
  MergeOptions(one, two);
  EXPECT_EQ(6, options_.css_inline_max_bytes());
}

TEST_F(RewriteOptionsTest, MergeCacheInvalidationTimeStampDefault) {
  RewriteOptions one, two;
  MergeOptions(one, two);
  EXPECT_EQ(RewriteOptions::kDefaultCacheInvalidationTimestamp,
            options_.cache_invalidation_timestamp());
}

TEST_F(RewriteOptionsTest, MergeCacheInvalidationTimeStampOne) {
  RewriteOptions one, two;
  one.set_cache_invalidation_timestamp(11111111);
  MergeOptions(one, two);
  EXPECT_EQ(11111111, options_.cache_invalidation_timestamp());
}

TEST_F(RewriteOptionsTest, MergeCacheInvalidationTimeStampTwo) {
  RewriteOptions one, two;
  two.set_cache_invalidation_timestamp(22222222);
  MergeOptions(one, two);
  EXPECT_EQ(22222222, options_.cache_invalidation_timestamp());
}

TEST_F(RewriteOptionsTest, MergeCacheInvalidationTimeStampOneLarger) {
  RewriteOptions one, two;
  one.set_cache_invalidation_timestamp(33333333);
  two.set_cache_invalidation_timestamp(22222222);
  MergeOptions(one, two);
  EXPECT_EQ(33333333, options_.cache_invalidation_timestamp());
}

TEST_F(RewriteOptionsTest, MergeCacheInvalidationTimeStampTwoLarger) {
  RewriteOptions one, two;
  one.set_cache_invalidation_timestamp(11111111);
  two.set_cache_invalidation_timestamp(22222222);
  MergeOptions(one, two);
  EXPECT_EQ(22222222, options_.cache_invalidation_timestamp());
}

TEST_F(RewriteOptionsTest, Allow) {
  options_.Allow("*.css");
  EXPECT_TRUE(options_.IsAllowed("abcd.css"));
  options_.Disallow("a*.css");
  EXPECT_FALSE(options_.IsAllowed("abcd.css"));
  options_.Allow("ab*.css");
  EXPECT_TRUE(options_.IsAllowed("abcd.css"));
  options_.Disallow("abc*.css");
  EXPECT_FALSE(options_.IsAllowed("abcd.css"));
}

TEST_F(RewriteOptionsTest, MergeAllow) {
  RewriteOptions one, two;
  one.Allow("*.css");
  EXPECT_TRUE(one.IsAllowed("abcd.css"));
  one.Disallow("a*.css");
  EXPECT_FALSE(one.IsAllowed("abcd.css"));

  two.Allow("ab*.css");
  EXPECT_TRUE(two.IsAllowed("abcd.css"));
  two.Disallow("abc*.css");
  EXPECT_FALSE(two.IsAllowed("abcd.css"));

  MergeOptions(one, two);
  EXPECT_FALSE(options_.IsAllowed("abcd.css"));
  EXPECT_FALSE(options_.IsAllowed("abc.css"));
  EXPECT_TRUE(options_.IsAllowed("ab.css"));
  EXPECT_FALSE(options_.IsAllowed("a.css"));
}

TEST_F(RewriteOptionsTest, DisableAllFiltersNotExplicitlyEnabled) {
  RewriteOptions one, two;
  one.EnableFilter(RewriteOptions::kAddHead);
  two.EnableFilter(RewriteOptions::kExtendCacheCss);
  two.DisableAllFiltersNotExplicitlyEnabled();  // Should disable AddHead.
  MergeOptions(one, two);

  // Make sure AddHead enabling didn't leak through.
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kAddHead));
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheCss));
}

TEST_F(RewriteOptionsTest, DisableAllFiltersOverrideFilterLevel) {
  // Disable the default enabled filter.
  options_.DisableFilter(RewriteOptions::kHtmlWriterFilter);

  options_.SetRewriteLevel(RewriteOptions::kCoreFilters);
  options_.EnableFilter(RewriteOptions::kAddHead);
  options_.DisableAllFiltersNotExplicitlyEnabled();

  // Check that *only* AddHead is enabled, even though we have CoreFilters
  // level set.
  EXPECT_TRUE(OnlyEnabled(RewriteOptions::kAddHead));
}

TEST_F(RewriteOptionsTest, AllDoesNotImplyStripScrips) {
  options_.SetRewriteLevel(RewriteOptions::kAllFilters);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kStripScripts));
}

TEST_F(RewriteOptionsTest, ExplicitlyEnabledDangerousFilters) {
  options_.SetRewriteLevel(RewriteOptions::kAllFilters);
  options_.EnableFilter(RewriteOptions::kStripScripts);
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kDivStructure));
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kStripScripts));
  options_.EnableFilter(RewriteOptions::kDivStructure);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kDivStructure));
}

TEST_F(RewriteOptionsTest, CoreAndNotDangerous) {
  options_.SetRewriteLevel(RewriteOptions::kCoreFilters);
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kAddInstrumentation));
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kCombineCss));
}

TEST_F(RewriteOptionsTest, CoreByNameNotLevel) {
  NullMessageHandler handler;
  options_.SetRewriteLevel(RewriteOptions::kPassThrough);
  ASSERT_TRUE(options_.EnableFiltersByCommaSeparatedList("core", &handler));

  // Test the same ones as tested in InstrumentationDisabled.
  ASSERT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheCss));
  ASSERT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheImages));

  // Test these for PlusAndMinus validation.
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kDivStructure));
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kInlineCss));
}

TEST_F(RewriteOptionsTest, PlusAndMinus) {
  const char* kList = "core,+div_structure,-inline_css,+extend_cache_css";
  NullMessageHandler handler;
  options_.SetRewriteLevel(RewriteOptions::kPassThrough);
  ASSERT_TRUE(options_.AdjustFiltersByCommaSeparatedList(kList, &handler));

  // Test the same ones as tested in InstrumentationDisabled.
  ASSERT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheCss));
  ASSERT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheImages));

  // These should be opposite from normal.
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kDivStructure));
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kInlineCss));
}

TEST_F(RewriteOptionsTest, SetDefaultRewriteLevel) {
  NullMessageHandler handler;
  RewriteOptions new_options;
  new_options.SetDefaultRewriteLevel(RewriteOptions::kCoreFilters);

  EXPECT_FALSE(options_.Enabled(RewriteOptions::kExtendCacheCss));
  options_.Merge(new_options);
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kExtendCacheCss));
}

void RewriteOptionsTest::TestSetOptionFromName(bool test_log_variant) {
  NullMessageHandler handler;

  TestNameSet(RewriteOptions::kOptionOk,
              test_log_variant,
              "CssInlineMaxBytes",
              "1024",
              &handler);
  // Default for this is 2048.
  EXPECT_EQ(1024L, options_.css_inline_max_bytes());

  TestNameSet(RewriteOptions::kOptionOk,
              test_log_variant,
              "JpegRecompressionQuality",
              "1",
              &handler);
  // Default is -1.
  EXPECT_EQ(1, options_.image_jpeg_recompress_quality());

  TestNameSet(RewriteOptions::kOptionOk,
              test_log_variant,
              "CombineAcrossPaths",
              "false",
              &handler);
  // Default is true
  EXPECT_EQ(false, options_.combine_across_paths());

  TestNameSet(RewriteOptions::kOptionOk,
              test_log_variant,
              "BeaconUrl",
              "example",
              &handler);
  EXPECT_EQ("example", options_.beacon_url());

  RewriteOptions::RewriteLevel old_level = options_.level();
  TestNameSet(RewriteOptions::kOptionValueInvalid,
              test_log_variant,
              "RewriteLevel",
              "does_not_work",
              &handler);
  EXPECT_EQ(old_level, options_.level());

  TestNameSet(RewriteOptions::kOptionNameUnknown,
              test_log_variant,
              "InvalidName",
              "example",
              &handler);

  TestNameSet(RewriteOptions::kOptionValueInvalid,
              test_log_variant,
              "JsInlineMaxBytes",
              "NOT_INT",
              &handler);
  EXPECT_EQ(RewriteOptions::kDefaultJsInlineMaxBytes,
            options_.js_inline_max_bytes());  // unchanged from default.
}

TEST_F(RewriteOptionsTest, SetOptionFromName) {
  TestSetOptionFromName(false);
}

TEST_F(RewriteOptionsTest, SetOptionFromNameAndLog) {
  TestSetOptionFromName(true);
}

// TODO(bharathbhushan): Figure out a better way to test all enum lookups.
TEST_F(RewriteOptionsTest, LookupOptionEnumTest) {
  RewriteOptions::Initialize();
  EXPECT_EQ(StringPiece("CssInlineMaxBytes"),
            RewriteOptions::LookupOptionEnum(
                RewriteOptions::kCssInlineMaxBytes));
  EXPECT_EQ(StringPiece("JsInlineMaxBytes"),
            RewriteOptions::LookupOptionEnum(
                RewriteOptions::kJsInlineMaxBytes));
  EXPECT_EQ(StringPiece("ImageInlineMaxBytes"),
            RewriteOptions::LookupOptionEnum(
                RewriteOptions::kImageInlineMaxBytes));
  EXPECT_EQ(StringPiece("CssImageInlineMaxBytes"),
            RewriteOptions::LookupOptionEnum(
                RewriteOptions::kCssImageInlineMaxBytes));
  EXPECT_EQ(StringPiece("MinImageSizeLowResolutionBytes"),
            RewriteOptions::LookupOptionEnum(
                RewriteOptions::kMinImageSizeLowResolutionBytes));
}

TEST_F(RewriteOptionsTest, PrioritizeCacheableFamilies1) {
  // Default matches nothing.
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "MatchesNothing"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(""));

  // Set explicitly.
  options_.AddToPrioritizeVisibleContentCacheableFamilies("zero*?");
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "MatchesNothing"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(""));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "zero1"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "zerooooo1"));

  // Merge in an options with default cacheable families.  This should not
  // affect options_.
  RewriteOptions options1;
  options_.Merge(options1);
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "MatchesNothing"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(""));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "zero1"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "zerooooo1"));

  // Merge in an options with explicit options.
  options1.AddToPrioritizeVisibleContentCacheableFamilies("one?");
  options1.AddToPrioritizeVisibleContentCacheableFamilies("?two*");
  options1.AddToPrioritizeVisibleContentCacheableFamilies("three");
  options_.Merge(options1);
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "MatchesNothing"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(""));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "zero1"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "zerooooo1"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "one1"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "one"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "2two"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "2twoANYTHING"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "twoANYTHING"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "three"));
}

TEST_F(RewriteOptionsTest, PrioritizeCacheableFamilies2) {
  // Default matches nothing.
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "MatchesNothing"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(""));

  // Merge in an options with default cacheable families.  This should not
  // affect options_.
  RewriteOptions options1;
  options_.Merge(options1);
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "MatchesNothing"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(""));

  // Merge in an options with explicit options.
  options1.AddToPrioritizeVisibleContentCacheableFamilies("one?");
  options1.AddToPrioritizeVisibleContentCacheableFamilies("?two*");
  options1.AddToPrioritizeVisibleContentCacheableFamilies("three");
  options_.Merge(options1);
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "MatchesNothing"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(""));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "one1"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "one"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "2two"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "2twoANYTHING"));
  EXPECT_FALSE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "twoANYTHING"));
  EXPECT_TRUE(options_.MatchesPrioritizeVisibleContentCacheableFamilies(
      "three"));
}

TEST_F(RewriteOptionsTest, FuriousSpecTest) {
  NullMessageHandler handler;
  options_.SetRewriteLevel(RewriteOptions::kCoreFilters);
  // 0 is reserved for the no-experiment state.
  EXPECT_FALSE(options_.AddFuriousSpec("id=0", &handler));

  EXPECT_TRUE(options_.AddFuriousSpec(
      "id=7;level=CoreFilters;enabled=sprite_images;disabled=inline_css;"
      "inline_js=600000", &handler));
  EXPECT_TRUE(options_.AddFuriousSpec("id=2;disabled=insert_ga ", &handler));
  options_.SetFuriousState(7);
  EXPECT_EQ(RewriteOptions::kCoreFilters, options_.level());
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kSpriteImages));
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kInlineCss));

  // insert_ga can not be disabled in any furious experiment because
  // that filter injects the instrumentation we use to collect the data.
  options_.SetFuriousState(2);
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kInlineCss));
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kSpriteImages));
  EXPECT_FALSE(options_.Enabled(RewriteOptions::kLeftTrimUrls));
  EXPECT_TRUE(options_.Enabled(RewriteOptions::kInsertGA));
}

TEST_F(RewriteOptionsTest, FuriousPrintTest) {
  NullMessageHandler handler;
  options_.SetRewriteLevel(RewriteOptions::kCoreFilters);
  EXPECT_TRUE(options_.AddFuriousSpec("id=7;level=AllFilters;", &handler));
  EXPECT_TRUE(options_.AddFuriousSpec("id=2;enabled=rewrite_css;", &handler));
  options_.SetFuriousState(-7);
  EXPECT_EQ(options_.ToString(), options_.ToExperimentString());
  options_.SetFuriousState(7);
  GoogleString output = "Experiment: 7; ";
  output += options_.ToString();
  EXPECT_EQ(output, options_.ToExperimentString());
}

}  // namespace
