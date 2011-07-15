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

// Author: sligocki@google.com (Shawn Ligocki)

#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/css_filter.h"
#include "net/instaweb/rewriter/public/css_rewrite_test_base.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/mem_file_system.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/timer.h"

namespace net_instaweb {

namespace {

// Filenames of resource files.
const char kBikePngFile[] = "BikeCrashIcn.png";
const char kCuppaPngFile[] = "Cuppa.png";
const char kPuzzleJpgFile[] = "Puzzle.jpg";

const char kImageData[] = "Invalid PNG but it does not matter for this test";

class CssImageRewriterTest : public CssRewriteTestBase {
 protected:
  virtual void SetUp() {
    // We setup the options before the upcall so that the
    // CSS filter is created aware of these.
    options()->EnableFilter(RewriteOptions::kExtendCache);
    CssRewriteTestBase::SetUp();
  }
};

TEST_P(CssImageRewriterTest, CacheExtendsImagesSimple) {
  // Simplified version of CacheExtendsImages, which doesn't have many copies of
  // the same URL.
  InitResponseHeaders("foo.png", kContentTypePng, kImageData, 100);

  static const char css_before[] =
      "body {\n"
      "  background-image: url(foo.png);\n"
      "}\n";
  static const char css_after[] =
      "body{background-image:url(http://test.com/foo.png.pagespeed.ce.0.png)}";

  ValidateRewriteInlineCss("cache_extends_images-inline",
                           css_before, css_after,
                           kExpectChange | kExpectSuccess);
  ValidateRewriteExternalCss("cache_extends_images-external",
                             css_before, css_after,
                             kExpectChange | kExpectSuccess |
                             kNoOtherContexts | kNoClearFetcher);
}

TEST_P(CssImageRewriterTest, CacheExtendsWhenCssGrows) {
  // We run most tests with set_always_rewrite_css(true) which bypasses
  // checks on whether rewriting is worthwhile or not. Test to make sure we make
  // the right decision when we do do the check in the case where the produced
  // CSS is actually larger, but contains rewritten resources.
  // (We want to rewrite the CSS in that case)
  options()->set_always_rewrite_css(false);
  InitResponseHeaders("foo.png", kContentTypePng, kImageData, 100);
  static const char css_before[] =
      "body{background-image: url(foo.png)}";
  static const char css_after[] =
      "body{background-image:url(http://test.com/foo.png.pagespeed.ce.0.png)}";

  ValidateRewriteInlineCss("cache_extends_images_growcheck-inline",
                           css_before, css_after,
                           kExpectChange | kExpectSuccess);
  ValidateRewriteExternalCss("cache_extends_images_growcheck-external",
                             css_before, css_after,
                             kExpectChange | kExpectSuccess |
                             kNoOtherContexts | kNoClearFetcher);
}

TEST_P(CssImageRewriterTest, CacheExtendsImages) {
  CSS_XFAIL_ASYNC();
  InitResponseHeaders("foo.png", kContentTypePng, kImageData, 100);
  InitResponseHeaders("bar.png", kContentTypePng, kImageData, 100);
  InitResponseHeaders("baz.png", kContentTypePng, kImageData, 100);

  static const char css_before[] =
      "body {\n"
      "  background-image: url(foo.png);\n"
      "  list-style-image: url('bar.png');\n"
      "}\n"
      ".titlebar p.cfoo, #end p {\n"
      "  background: url(\"baz.png\");\n"
      "  list-style: url('foo.png');\n"
      "}\n"
      ".other {\n"
      "  background-image:url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAA"
      "AUAAAAFCAYAAACNbyblAAAAHElEQVQI12P4//8/w38GIAXDIBKE0DHxgljNBAAO9TXL0Y4"
      "OHwAAAABJRU5ErkJggg==);"
      "  -proprietary-background-property: url(foo.png);\n"
      "}";
  static const char css_after[] =
      "body{background-image:url(http://test.com/foo.png.pagespeed.ce.0.png);"
      "list-style-image:url(http://test.com/bar.png.pagespeed.ce.0.png)}"
      ".titlebar p.cfoo,#end p{"
      "background:url(http://test.com/baz.png.pagespeed.ce.0.png);"
      "list-style:url(http://test.com/foo.png.pagespeed.ce.0.png)}"
      ".other{"  // data: URLs and unknown properties are not rewritten.
      "background-image:url(data:image/png;base64\\,iVBORw0KGgoAAAANSUhEUgAAA"
      "AUAAAAFCAYAAACNbyblAAAAHElEQVQI12P4//8/w38GIAXDIBKE0DHxgljNBAAO9TXL0Y4"
      "OHwAAAABJRU5ErkJggg==);"
      "-proprietary-background-property:url(foo.png)}";

  // Can't serve from new contexts yet, because we're using mock_fetcher_.
  // TODO(sligocki): Resolve that and the just have:
  // ValidateRewriteInlineCss("cache_extends_images", css_before, css_after);
  ValidateRewriteInlineCss("cache_extends_images-inline",
                           css_before, css_after,
                           kExpectChange | kExpectSuccess);
  ValidateRewriteExternalCss("cache_extends_images-external",
                             css_before, css_after,
                             kExpectChange | kExpectSuccess |
                             kNoOtherContexts | kNoClearFetcher);
}

TEST_P(CssImageRewriterTest, TrimsImageUrls) {
  options()->EnableFilter(RewriteOptions::kLeftTrimUrls);
  InitResponseHeaders("foo.png", kContentTypePng, kImageData, 100);
  static const char kCss[] =
      "body {\n"
      "  background-image: url(foo.png);\n"
      "}\n";

  static const char kCssAfter[] =
      "body{background-image:url(foo.png.pagespeed.ce.0.png)}";

  ValidateRewriteExternalCss("trims_css_urls", kCss, kCssAfter,
                              kExpectChange | kExpectSuccess |
                              kNoOtherContexts | kNoClearFetcher);
}

TEST_P(CssImageRewriterTest, InlinePaths) {
  // Make sure we properly handle CSS relative references when we have the same
  // inline CSS in different places. This is also a regression test for a bug
  // during development of async + inline case which caused us to do
  // null rewrites from cache.
  options()->EnableFilter(RewriteOptions::kLeftTrimUrls);
  InitResponseHeaders("dir/foo.png", kContentTypePng, kImageData, 100);

  static const char kCssBefore[] =
      "body {\n"
      "  background-image: url(http://test.com/dir/foo.png);\n"
      "}\n";

  static const char kCssAfter[] =
      "body{background-image:url(dir/foo.png.pagespeed.ce.0.png)}";
  ValidateRewriteInlineCss("nosubdir",
                           kCssBefore, kCssAfter,
                           kExpectChange | kExpectSuccess);

  static const char kCssAfterRel[] =
      "body{background-image:url(foo.png.pagespeed.ce.0.png)}";
  ValidateRewriteInlineCss("dir/yessubdir",
                           kCssBefore, kCssAfterRel,
                           kExpectChange | kExpectSuccess);
}

TEST_P(CssImageRewriterTest, RewriteCached) {
  // Make sure we produce the same output from cache.
  options()->EnableFilter(RewriteOptions::kLeftTrimUrls);
  InitResponseHeaders("dir/foo.png", kContentTypePng, kImageData, 100);

  static const char kCssBefore[] =
      "body {\n"
      "  background-image: url(http://test.com/dir/foo.png);\n"
      "}\n";

  static const char kCssAfter[] =
      "body{background-image:url(dir/foo.png.pagespeed.ce.0.png)}";
  ValidateRewriteInlineCss("nosubdir",
                           kCssBefore, kCssAfter,
                           kExpectChange | kExpectSuccess);

  statistics()->Clear();
  ValidateRewriteInlineCss("nosubdir2",
                           kCssBefore, kCssAfter,
                           kExpectChange | kExpectSuccess | kNoStatCheck);
  // Should not re-serialize. Works only under the new flow...
  if (rewrite_driver()->asynchronous_rewrites()) {
    EXPECT_EQ(
        0, statistics()->GetVariable(CssFilter::kMinifiedBytesSaved)->Get());
  }
}

TEST_P(CssImageRewriterTest, CacheInlineParseFailures) {
  const char kInvalidCss[] = " div{";

  Variable* num_parse_failures =
      statistics()->GetVariable(CssFilter::kParseFailures);

  ValidateRewriteInlineCss("inline-invalid", kInvalidCss, kInvalidCss,
                           kExpectNoChange | kExpectFailure | kNoOtherContexts);
  EXPECT_EQ(1, num_parse_failures->Get());

  // This works properly only under new flow...
  if (rewrite_driver()->asynchronous_rewrites()) {
    ValidateRewriteInlineCss(
        "inline-invalid2", kInvalidCss, kInvalidCss,
        kExpectNoChange | kExpectFailure | kNoOtherContexts | kNoStatCheck);
    // Shouldn't reparse -- and stats are reset between runs.
    EXPECT_EQ(0, num_parse_failures->Get());
  }
}

TEST_P(CssImageRewriterTest, RecompressImages) {
  options()->EnableFilter(RewriteOptions::kRecompressImages);
  AddFileToMockFetcher(StrCat(kTestDomain, "foo.png"), kBikePngFile,
                        kContentTypePng, 100);
  static const char kCss[] =
      "body {\n"
      "  background-image: url(foo.png);\n"
      "}\n";

  static const char kCssAfter[] =
      "body{background-image:url(http://test.com/xfoo.png.pagespeed.ic.0.png)}";

  ValidateRewriteExternalCss("recompress_css_images", kCss, kCssAfter,
                              kExpectChange | kExpectSuccess |
                              kNoOtherContexts | kNoClearFetcher);
}

TEST_P(CssImageRewriterTest, UseCorrectBaseUrl) {
  // Initialize resources.
  static const char css_url[] = "http://www.example.com/bar/style.css";
  static const char css_before[] = "body { background: url(image.png); }";
  InitResponseHeaders(css_url, kContentTypeCss, css_before, 100);
  static const char image_url[] = "http://www.example.com/bar/image.png";
  InitResponseHeaders(image_url, kContentTypePng, kImageData, 100);

  // Construct URL for rewritten image.
  GoogleString expected_image_url = ExpectedRewrittenUrl(
      image_url, kImageData, RewriteDriver::kCacheExtenderId,
      kContentTypePng);

  GoogleString css_after = StrCat(
      "body{background:url(", expected_image_url, ")}");

  // Construct URL for rewritten CSS.
  GoogleString expected_css_url = ExpectedRewrittenUrl(
      css_url, css_after, RewriteDriver::kCssFilterId, kContentTypeCss);

  static const char html_before[] =
      "<head>\n"
      "  <link rel='stylesheet' href='bar/style.css'>\n"
      "</head>";
  GoogleString html_after = StrCat(
      "<head>\n"
      "  <link rel='stylesheet' href='", expected_css_url, "'>\n"
      "</head>");

  // Make sure that image.png uses http://www.example.com/bar/style.css as
  // base URL instead of http://www.example.com/.
  ValidateExpectedUrl("http://www.example.com/", html_before, html_after);

  GoogleString actual_css_after;
  ServeResourceUrl(expected_css_url, &actual_css_after);
  EXPECT_EQ(css_after, actual_css_after);
}

// We test with asynchronous_rewrites() == GetParam() as both true and false.
INSTANTIATE_TEST_CASE_P(CssImageRewriterTestInstance,
                        CssImageRewriterTest,
                        ::testing::Bool());

// Note that these values of "10" and "20" are very tight.  This is a
// feature.  It serves as an early warning system because extra cache
// lookups will induce time-advancement from
// MemFileSystem::UpdateAtime, which can make these resources expire
// before they are used.  So if you find tests in this module failing
// unexpectedly, you may be tempted to bump up these values.  Don't.
// Figure out how to make fewer cache lookups.
static const int kMinExpirationTimeMs = 10 * Timer::kSecondMs;
static const int kExpireAPngSec = 10;
static const int kExpireBPngSec = 20;

// These tests are to make sure our TTL considers that of subresources.
class CssFilterSubresourceTest : public CssRewriteTestBase {
 public:

  virtual void SetUp() {
    // We setup the options before the upcall so that the
    // CSS filter is created aware of these.
    options()->EnableFilter(RewriteOptions::kExtendCache);
    options()->EnableFilter(RewriteOptions::kRecompressImages);
    CssRewriteTestBase::SetUp();

    // As we use invalid payloads, we expect image rewriting to
    // fail but cache extension to succeed.
    InitResponseHeaders("a.png", kContentTypePng, "notapng", kExpireAPngSec);
    InitResponseHeaders("b.png", kContentTypePng, "notbpng", kExpireBPngSec);
  }

  void ValidateExpirationTime(const char* id, const char* output,
                              int64 expected_expire_ms) {
    GoogleString css_url = ExpectedUrlForCss(id, output);

    // See what cache information we have
    bool use_async_flow = false;
    OutputResourcePtr output_resource(
        rewrite_driver()->CreateOutputResourceWithPath(
            kTestDomain, RewriteDriver::kCssFilterId, StrCat(id, ".css"),
            &kContentTypeCss, kRewrittenResource, use_async_flow));
    ASSERT_TRUE(output_resource.get() != NULL);
    EXPECT_EQ(css_url, output_resource->url());
    ASSERT_TRUE(output_resource->cached_result() != NULL);

    EXPECT_EQ(expected_expire_ms,
              output_resource->cached_result()->origin_expiration_time_ms());
  }

  GoogleString ExpectedUrlForPng(const StringPiece& name,
                                 const GoogleString& expected_output) {
    return Encode(kTestDomain, RewriteDriver::kCacheExtenderId,
                  hasher()->Hash(expected_output),
                  name, "png");
  }
};

// Test to make sure expiration time for cached result is the
// smallest of subresource and CSS times, not just CSS time.
TEST_P(CssFilterSubresourceTest, SubResourceDepends) {
  // These tests rely on the guts of the old expiration machinery.
  CSS_XFAIL_ASYNC();

  const char kInput[] = "div { background-image: url(a.png); }"
                        "span { background-image: url(b.png); }";

  // Figure out where cache-extended PNGs will go.
  GoogleString image_url1 = ExpectedUrlForPng("a.png", "notapng");
  GoogleString image_url2 = ExpectedUrlForPng("b.png", "notbpng");
  GoogleString output = StrCat("div{background-image:url(", image_url1, ")}",
                               "span{background-image:url(", image_url2, ")}");

  // Here we don't use the other contexts since it has different
  // synchronicity, and we presently do best-effort for loaded subresources
  // even in Fetch.
  ValidateRewriteExternalCss(
      "ext", kInput, output, kNoOtherContexts | kNoClearFetcher |
                             kExpectChange | kExpectSuccess);

  // 10 is the smaller of expiration times of a.png, b.png and ext.css
  ValidateExpirationTime("ext", output.c_str(), kMinExpirationTimeMs);
}

// Test to make sure we don't cache for long if the rewrite was based
// on not-yet-loaded resources.
TEST_P(CssFilterSubresourceTest, SubResourceDependsNotYetLoaded) {
  // These tests rely on the guts of the old expiration machinery.
  CSS_XFAIL_ASYNC();

  SetupWaitFetcher();

  // Disable atime simulation so that the clock doesn't move on us.
  file_system()->set_atime_enabled(false);

  const char kInput[] = "div { background-image: url(a.png); }"
                        "span { background-image: url(b.png); }";
  const char kOutput[] = "div{background-image:url(a.png)}"
                        "span{background-image:url(b.png)}";

  // At first try, not even the CSS gets loaded, so nothing gets
  // changed at all.
  ValidateRewriteExternalCss(
      "wip", kInput, kInput, kNoOtherContexts | kNoClearFetcher |
                             kExpectNoChange | kExpectSuccess);

  // Get the CSS to load (resources are still unavailable).
  CallFetcherCallbacks();
  ValidateRewriteExternalCss(
      "wip", kInput, kOutput, kNoOtherContexts | kNoClearFetcher |
                              kExpectChange | kExpectSuccess);

  // Since resources haven't loaded, the output cache should have a very small
  // expiration time.
  ValidateExpirationTime("wip", kOutput, Timer::kSecondMs);

  // Make sure the subresource callbacks fire for leak cleanliness
  CallFetcherCallbacks();
}

// We test with asynchronous_rewrites() == GetParam() as both true and false.
INSTANTIATE_TEST_CASE_P(CssFilterSubresourceTestInstance,
                        CssFilterSubresourceTest,
                        ::testing::Bool());


}  // namespace

}  // namespace net_instaweb
