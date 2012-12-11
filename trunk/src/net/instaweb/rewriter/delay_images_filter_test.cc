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

// Author: pulkitg@google.com (Pulkit Goyal)

#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/public/delay_images_filter.h"
#include "net/instaweb/rewriter/public/js_defer_disabled_filter.h"
#include "net/instaweb/rewriter/public/js_disable_filter.h"
#include "net/instaweb/rewriter/public/lazyload_images_filter.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/static_javascript_manager.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/wildcard.h"

namespace {
const char kSampleJpgFile[] = "Sample.jpg";
const char kSampleWebpFile[] = "Sample_webp.webp";
const char kLargeJpgFile[] = "Puzzle.jpg";
const char kSmallPngFile[] = "BikeCrashIcn.png";

// Generated html is matched approximately because different versions of
// libjpeg are yeilding different low_res_image_data.
const char kSampleJpegData[] = "data:image/jpeg;base64*";
const char kSampleWebpData[] = "data:image/webp;base64*";

const char kHeadHtml[] = "<head></head>";

const char kHeadHtmlWithDeferJsTemplate[] =
    "<head><script type=\"text/javascript\" pagespeed_no_defer=\"\">"
    "%s"
    "</script>"
    "</head>";

const char kDeferJsTemplate[] =
    "<script type=\"text/javascript\" src=\"/psajs/js_defer.0.js\"></script>";

const char kLazyloadTemplate[] =
    "<script type=\"text/javascript\">"
    "%s"
    "\npagespeed.lazyLoadInit(false, \"%s\");\n"
    "</script>";

const char kInlineScriptTemplate[] =
    "<script type=\"text/javascript\">%s";

const char kScriptTemplate[] =
    "<script type=\"text/javascript\">%s</script>";

}  // namespace

namespace net_instaweb {

class DelayImagesFilterTest : public RewriteTestBase {
 public:
  DelayImagesFilterTest() {
    options()->set_min_image_size_low_resolution_bytes(1 * 1024);
    options()->set_max_inlined_preview_images_index(-1);
  }

 protected:
  // TODO(matterbury): Delete this method as it should be redundant.
  virtual void SetUp() {
    RewriteTestBase::SetUp();
    SetHtmlMimetype();  // Prevent insertion of CDATA tags to static JS.
  }

  virtual bool AddHtmlTags() const { return false; }
  // Match rewritten html content and return its byte count.
  int MatchOutputAndCountBytes(const GoogleString& html_input,
                               const GoogleString& expected) {
    Parse("inline_preview_images", html_input);
    GoogleString full_html = doctype_string_ + AddHtmlBody(expected);
    EXPECT_TRUE(Wildcard(full_html).Match(output_buffer_)) <<
        "Expected:\n" << full_html << "\n\nGot:\n" << output_buffer_;
    int output_size = output_buffer_.size();
    output_buffer_.clear();
    return output_size;
  }

  GoogleString GetNoscript() const {
    return StringPrintf(
        kNoScriptRedirectFormatter,
        "http://test.com/inline_preview_images.html?ModPagespeed=noscript",
        "http://test.com/inline_preview_images.html?ModPagespeed=noscript");
  }

  GoogleString GenerateAddLowResString(const GoogleString& url,
                                       const GoogleString& image_data) {
    return StrCat("\npagespeed.delayImagesInline.addLowResImages(\'", url,
        "\', \'", image_data, "\');");
  }

  GoogleString GenerateRewrittenImageTag(const StringPiece& url) {
    return StrCat("<img pagespeed_lazy_src=\"", url, "\" src=\"",
                  LazyloadImagesFilter::kBlankImageSrc, "\" onload=\"",
                  LazyloadImagesFilter::kImageOnloadCode, "\"/>");
  }

  GoogleString GetHeadHtmlWithDeferJs() {
    return StringPrintf(kHeadHtmlWithDeferJsTemplate,
                        JsDisableFilter::kDisableJsExperimental);
  }

  GoogleString GetDeferJs() {
    return kDeferJsTemplate;
  }

  GoogleString GetHtmlWithLazyload() {
    return StringPrintf(kLazyloadTemplate,
                        GetLazyloadImagesCode().c_str(),
                        LazyloadImagesFilter::kBlankImageSrc);
  }

  GoogleString GetInlineScript() {
    return StringPrintf(kInlineScriptTemplate,
                        GetDelayImagesInlineCode().c_str());
  }

  GoogleString GetDelayImages() {
    return StringPrintf(kScriptTemplate, GetDelayImagesCode().c_str());
  }

  GoogleString GetDelayImagesCode() {
    return GetJsCode(StaticJavascriptManager::kDelayImagesJs,
                     DelayImagesFilter::kDelayImagesSuffix);
  }

  GoogleString GetDelayImagesInlineCode() {
    return GetJsCode(StaticJavascriptManager::kDelayImagesInlineJs,
                     DelayImagesFilter::kDelayImagesInlineSuffix);
  }

  GoogleString GetLazyloadImagesCode() {
    return server_context()->static_javascript_manager()->GetJsSnippet(
        StaticJavascriptManager::kLazyloadImagesJs, options());
  }

  GoogleString GetJsCode(StaticJavascriptManager::JsModule module,
                         const StringPiece& call) {
    StringPiece code =
        server_context()->static_javascript_manager()->GetJsSnippet(
            module, options());
    return StrCat(code, call);
  }

  void SetupUserAgentTest(StringPiece user_agent) {
    ClearRewriteDriver();
    rewrite_driver()->set_user_agent(user_agent);
    SetHtmlMimetype();  // Prevent insertion of CDATA tags to static JS.
  }
};

TEST_F(DelayImagesFilterTest, DelayImagesAcrossDifferentFlushWindow) {
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                         kContentTypeWebp, 100);
  AddFileToMockFetcher("http://test.com/1.jpeg", kSampleJpgFile,
                       kContentTypeJpeg, 100);
  GoogleString flush1 = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.webp\" />";
  GoogleString flush2="<img src=\"http://test.com/1.jpeg\" />"
      "</body>";
  GoogleString input = StrCat(flush1, flush2);
  SetupWriter();
  html_parse()->StartParse("http://test.com/");
  html_parse()->ParseText(flush1);
  html_parse()->Flush();
  html_parse()->ParseText(flush2);
  html_parse()->FinishParse();
  rewrite_driver()->log_record()->Finalize();

  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      StrCat("<body>",
             StringPrintf(kNoScriptRedirectFormatter,
                          "http://test.com/?ModPagespeed=noscript",
                          "http://test.com/?ModPagespeed=noscript"),
             "<img pagespeed_high_res_src=\"http://test.com/1.webp\"/>"),
      GetInlineScript(),
      GenerateAddLowResString("http://test.com/1.webp", kSampleWebpData),
      "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>",
      GetDelayImages(),
      StrCat("<img pagespeed_high_res_src=\"http://test.com/1.jpeg\"/>",
             "<script type=\"text/javascript\">",
             GenerateAddLowResString("http://test.com/1.jpeg", kSampleJpegData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>"
             "<script type=\"text/javascript\">"
             "\npagespeed.delayImages.replaceWithHighRes();\n</script>"
             "</body>", GetDeferJs()));
  EXPECT_TRUE(Wildcard(output_html).Match(output_buffer_));
  EXPECT_TRUE(logging_info()->applied_rewriters().find("di") !=
              GoogleString::npos);
}

TEST_F(DelayImagesFilterTest, DelayImagesPreserveURLsOn) {
  // Make sure that we don't delay images when preserve urls is on.
  options()->set_image_preserve_urls(true);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.jpeg", kSampleJpgFile,
                       kContentTypeJpeg, 100);
  const char kInputHtml[] =
      "<html><head></head><body>"
      "<img src=\"http://test.com/1.jpeg\"/>"
      "</body></html>";

  // We'll add the noscript code but the image URL shouldn't change.
  GoogleString output_html = StrCat(
      "<html><head></head><body>",
      GetNoscript(),
      "<img src=\"http://test.com/1.jpeg\"/></body></html>");

  MatchOutputAndCountBytes(kInputHtml, output_html);
}

TEST_F(DelayImagesFilterTest, DelayImageWithDeferJavascriptDisabled) {
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                       kContentTypeWebp, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.webp\" />"
      "</body>";
  GoogleString output_html = StrCat(
      "<head></head>",
      StrCat("<body>",
             GetNoscript(),
             "<img pagespeed_high_res_src=\"http://test.com/1.webp\" "),
      "src=\"", kSampleWebpData, "\"/>", GetDelayImages(), "</body>");
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, DelayImageWithQueryParam) {
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  options()->DisableFilter(RewriteOptions::kInlineImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp?a=b&c=d", kSampleWebpFile,
                       kContentTypeWebp, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.webp?a=b&amp;c=d\" />"
      "</body>";
  GoogleString output_html = StrCat(
      "<head></head><body>",
      GetNoscript(),
      "<img pagespeed_high_res_src=\"http://test.com/1.webp?a=b&amp;c=d\" "
      "src=\"", kSampleWebpData, "\"/>", GetDelayImages(), "</body>");
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, DelayImageWithUnescapedQueryParam) {
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  options()->DisableFilter(RewriteOptions::kInlineImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp?a=b&c=d", kSampleWebpFile,
                       kContentTypeWebp, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.webp?a=b&c=d\" />"
      "</body>";
  GoogleString output_html = StrCat(
      "<head></head><body>",
      GetNoscript(),
      "<img pagespeed_high_res_src=\"http://test.com/1.webp?a=b&c=d\" "
      "src=\"", kSampleWebpData, "\"/>", GetDelayImages(), "</body>");
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, DelayImageWithLazyLoadDisabled) {
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                       kContentTypeWebp, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.webp\" />"
      "</body>";
  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      "<body>",
      GetNoscript(),
      StrCat("<img pagespeed_high_res_src=\"http://test.com/1.webp\" ",
             "src=\"", kSampleWebpData, "\"/>"),
      GetDelayImages(), "</body>", GetDeferJs());
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, DelayWebPImage) {
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                       kContentTypeWebp, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.webp\" />"
      "<input src=\"http://test.com/1.webp\" type=\"image\"/>"
      "</body>";
  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      "<body>",
      GetNoscript(),
      "<img pagespeed_high_res_src=\"http://test.com/1.webp\"/>",
      "<input pagespeed_high_res_src=\"http://test.com/1.webp\"",
      " type=\"image\"/>",
      StrCat(GetInlineScript(),
             GenerateAddLowResString("http://test.com/1.webp", kSampleWebpData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>",
             GetDelayImages(), "</body>", GetDeferJs()));
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, DelayJpegImage) {
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.jpeg", kSampleJpgFile,
                       kContentTypeJpeg, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.jpeg\" />"
      "</body>";
  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      "<body>",
      GetNoscript(),
      "<img pagespeed_high_res_src=\"http://test.com/1.jpeg\"/>",
      StrCat(GetInlineScript(),
             GenerateAddLowResString("http://test.com/1.jpeg", kSampleJpegData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>",
             GetDelayImages(), "</body>", GetDeferJs()));
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, DelayJpegImageOnInputElement) {
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.jpeg", kSampleJpgFile,
                       kContentTypeJpeg, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<input type=\"image\" src=\"http://test.com/1.jpeg\" />"
      "</body>";
  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      "<body>",
      GetNoscript(),
      "<input type=\"image\"",
      " pagespeed_high_res_src=\"http://test.com/1.jpeg\"/>",
      StrCat(GetInlineScript(),
             GenerateAddLowResString("http://test.com/1.jpeg", kSampleJpegData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>",
             GetDelayImages(), "</body>", GetDeferJs()));
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, TestMinImageSizeLowResolutionBytesFlag) {
  options()->set_min_image_size_low_resolution_bytes(2 * 1024);
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                       kContentTypeWebp, 100);
  AddFileToMockFetcher("http://test.com/1.jpeg", kSampleJpgFile,
                       kContentTypeJpeg, 100);
  // Size of 1.webp is 1780 and size of 1.jpeg is 6245. As
  // MinImageSizeLowResolutionBytes is set to 2 KB only jpeg low quality image
  // will be generated.
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.webp\" />"
      "<img src=\"http://test.com/1.jpeg\" />"
      "</body>";
  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      "<body>",
      GetNoscript(),
      GetHtmlWithLazyload(),
      GenerateRewrittenImageTag("http://test.com/1.webp"),
      "<img pagespeed_high_res_src=\"http://test.com/1.jpeg\"/>"
      "<script type=\"text/javascript\" pagespeed_no_defer=\"\">"
      "pagespeed.lazyLoadImages.overrideAttributeFunctions();</script>",
      StrCat(GetInlineScript(),
             GenerateAddLowResString("http://test.com/1.jpeg", kSampleJpegData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>",
             GetDelayImages(), "</body>", GetDeferJs()));
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, TestMaxImageSizeLowResolutionBytesFlag) {
  options()->set_max_image_size_low_resolution_bytes(4 * 1024);
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                       kContentTypeWebp, 100);
  AddFileToMockFetcher("http://test.com/1.jpeg", kSampleJpgFile,
                       kContentTypeJpeg, 100);
  // Size of 1.webp is 1780 and size of 1.jpeg is 6245. As
  // MaxImageSizeLowResolutionBytes is set to 4 KB only webp low quality image
  // will be generated.
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.webp\" />"
      "<img src=\"http://test.com/1.jpeg\" />"
      "</body>";
  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      "<body>",
      GetNoscript(),
      "<img pagespeed_high_res_src=\"http://test.com/1.webp\"/>",
      StrCat(GetInlineScript(),
             GenerateAddLowResString("http://test.com/1.webp", kSampleWebpData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>",
             GetDelayImages(),
             GetHtmlWithLazyload(),
             GenerateRewrittenImageTag("http://test.com/1.jpeg"),
             "<script type=\"text/javascript\" pagespeed_no_defer=\"\">"
             "pagespeed.lazyLoadImages.overrideAttributeFunctions();</script>"
             "</body>", GetDeferJs()));
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, TestMaxInlinedPreviewImagesIndexFlag) {
  options()->set_max_inlined_preview_images_index(1);
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                       kContentTypeWebp, 100);
  AddFileToMockFetcher("http://test.com/1.jpeg", kSampleJpgFile,
                       kContentTypeJpeg, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.jpeg\" />"
      "<img src=\"http://test.com/1.webp\" />"
      "</body>";
  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      "<body>",
      GetNoscript(),
      "<img pagespeed_high_res_src=\"http://test.com/1.jpeg\"/>",
      StrCat(GetInlineScript(),
             GenerateAddLowResString("http://test.com/1.jpeg", kSampleJpegData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>",
             GetDelayImages(),
             GetHtmlWithLazyload(),
             GenerateRewrittenImageTag("http://test.com/1.webp"),
             "<script type=\"text/javascript\" pagespeed_no_defer=\"\">"
             "pagespeed.lazyLoadImages.overrideAttributeFunctions();</script>"
             "</body>", GetDeferJs()));
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, DelayMultipleSameImage) {
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                       kContentTypeWebp, 100);

  // pagespeed_inline_map size will be 1. For same images, delay_images_filter
  // make only one entry in pagespeed_inline_map.
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.webp\" />"
      "<img src=\"http://test.com/1.webp\" />"
      "</body>";
  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      "<body>",
      GetNoscript(),
      "<img pagespeed_high_res_src=\"http://test.com/1.webp\"/>"
      "<img pagespeed_high_res_src=\"http://test.com/1.webp\"/>",
      StrCat(GetInlineScript(),
             GenerateAddLowResString("http://test.com/1.webp", kSampleWebpData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>",
             GetDelayImages(), "</body>", GetDeferJs()));
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, NoHeadTag) {
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                       kContentTypeWebp, 100);
  GoogleString input_html = "<body>"
      "<img src=\"http://test.com/1.webp\"/>"
      "</body>";
  GoogleString output_html = StrCat(
      "<body>",
      GetNoscript(),
      "<img pagespeed_high_res_src=\"http://test.com/1.webp\" ",
      "src=\"", kSampleWebpData, "\"/>", GetDelayImages(), "</body>");
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, MultipleBodyTags) {
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kLazyloadImages);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.webp", kSampleWebpFile,
                       kContentTypeWebp, 100);
  AddFileToMockFetcher("http://test.com/2.jpeg", kSampleJpgFile,
                       kContentTypeJpeg, 100);

  // No change in the subsequent body tags.
  GoogleString input_html = "<head></head>"
      "<body><img src=\"http://test.com/1.webp\"/></body>"
      "<body><img src=\"http://test.com/2.jpeg\"/></body>";
  GoogleString output_html = StrCat(GetHeadHtmlWithDeferJs(),
      "<body>",
      GetNoscript(),
      "<img pagespeed_high_res_src=\"http://test.com/1.webp\"/>"
      "</body>",
      StrCat(GetInlineScript(),
             GenerateAddLowResString("http://test.com/1.webp", kSampleWebpData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>",
             GetDelayImages(),
             "<body><img pagespeed_high_res_src=\"http://test.com/2.jpeg\"/>"
             "<script type=\"text/javascript\">",
             GenerateAddLowResString("http://test.com/2.jpeg", kSampleJpegData),
             "\npagespeed.delayImagesInline.replaceWithLowRes();\n</script>"
             "<script type=\"text/javascript\">"
             "\npagespeed.delayImages.replaceWithHighRes();\n</script>"
             "</body>", GetDeferJs()));
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, ResizeForResolution) {
  options()->EnableFilter(RewriteOptions::kDelayImages);
  options()->EnableFilter(RewriteOptions::kResizeMobileImages);
  rewrite_driver()->AddFilters();
  AddFileToMockFetcher("http://test.com/1.jpeg", kLargeJpgFile,
                       kContentTypeJpeg, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.jpeg\"/>"
      "</body>";
  GoogleString output_html = StrCat(
      kHeadHtml,
      StrCat("<body>",
             GetNoscript(),
             "<img pagespeed_high_res_src=\"http://test.com/1.jpeg\" "),
      "src=\"", kSampleJpegData, "\"/>", GetDelayImages(), "</body>");

  // Mobile output should be smaller than desktop because inlined low quality
  // image is resized smaller for mobile.
  // Do desktop and mobile rewriting twice. They should not affect each other.
  SetupUserAgentTest("Safari");
  int byte_count_desktop1 = MatchOutputAndCountBytes(input_html, output_html);

  SetupUserAgentTest("Android 3.1");
  int byte_count_android1 = MatchOutputAndCountBytes(input_html, output_html);
  EXPECT_LT(byte_count_android1, byte_count_desktop1);

  SetupUserAgentTest("MSIE 8.0");
  int byte_count_desktop2 = MatchOutputAndCountBytes(input_html, output_html);

  SetupUserAgentTest("Android 4");
  int byte_count_android2 = MatchOutputAndCountBytes(input_html, output_html);
  EXPECT_EQ(byte_count_android1, byte_count_android2);
  EXPECT_EQ(byte_count_desktop1, byte_count_desktop2);

  SetupUserAgentTest("iPhone OS");
  int byte_count_iphone = MatchOutputAndCountBytes(input_html, output_html);
  EXPECT_EQ(byte_count_iphone, byte_count_android1);
}

TEST_F(DelayImagesFilterTest, ResizeForResolutionWithSmallImage) {
  options()->EnableFilter(RewriteOptions::kDelayImages);
  options()->EnableFilter(RewriteOptions::kResizeMobileImages);
  rewrite_driver()->AddFilters();
  AddFileToMockFetcher("http://test.com/1.png", kSmallPngFile,
                       kContentTypePng, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.png\"/>"
      "</body>";
  GoogleString output_html = StrCat(
      kHeadHtml,
      "<body>",
      GetNoscript(),
      "<img src=\"http://test.com/1.png\"/>"
      "</body>");

  // No low quality data for an image smaller than kDelayImageWidthForMobile
  // (in image_rewrite_filter.cc).
  rewrite_driver()->set_user_agent("Android 3.1");
  MatchOutputAndCountBytes(input_html, output_html);
}

TEST_F(DelayImagesFilterTest, ResizeForResolutionNegative) {
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.jpeg", kLargeJpgFile,
                       kContentTypeJpeg, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.jpeg\"/>"
      "</body>";
  GoogleString output_html = StrCat(
      kHeadHtml,
      StrCat("<body>",
             GetNoscript(),
             "<img pagespeed_high_res_src=\"http://test.com/1.jpeg\" "),
      "src=\"", kSampleJpegData, "\"/>", GetDelayImages(), "</body>");

  // If kResizeMobileImages is not explicitly enabled, desktop and mobile
  // outputs will have the same size.
  SetupUserAgentTest("Safari");
  int byte_count_desktop = MatchOutputAndCountBytes(input_html, output_html);
  SetupUserAgentTest("Android 3.1");
  int byte_count_mobile = MatchOutputAndCountBytes(input_html, output_html);
  EXPECT_EQ(byte_count_mobile, byte_count_desktop);
}

TEST_F(DelayImagesFilterTest, DelayImagesScriptOptimized) {
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.jpeg", kLargeJpgFile,
                       kContentTypeJpeg, 100);
  rewrite_driver()->set_user_agent("Safari");
  Parse("optimized",
        "<head></head><body><img src=\"http://test.com/1.jpeg\"</body>");
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("/*"))
      << "There should be no comments in the optimized code";
}

TEST_F(DelayImagesFilterTest, DelayImagesScriptDebug) {
  options()->EnableFilter(RewriteOptions::kDebug);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.jpeg", kLargeJpgFile,
                       kContentTypeJpeg, 100);
  rewrite_driver()->set_user_agent("Safari");
  Parse("debug",
        "<head></head><body><img src=\"http://test.com/1.jpeg\"</body>");
  EXPECT_NE(GoogleString::npos, output_buffer_.find("/*"))
      << "There should still be some comments in the debug code";
}

TEST_F(DelayImagesFilterTest, ExperimentalIsTrue) {
  options()->set_enable_inline_preview_images_experimental(true);
  AddFilter(RewriteOptions::kDelayImages);
  AddFileToMockFetcher("http://test.com/1.jpeg", kSampleJpgFile,
                       kContentTypeJpeg, 100);
  GoogleString input_html = "<head></head>"
      "<body>"
      "<img src=\"http://test.com/1.jpeg\" onload=\"blah();\"/>"
      "<img src=\"http://test.com/1.jpeg\" />"
      "</body>";
  GoogleString output_html = StrCat("<head></head><body>",
      GetNoscript(),
      "<img src=\"http://test.com/1.jpeg\" onload=\"blah();\"/>"
      "<img pagespeed_high_res_src=\"http://test.com/1.jpeg\" src=\"",
      kSampleJpegData, "\" onload=\"",
      DelayImagesFilter::kOnloadFunction, "\"/></body>");
  MatchOutputAndCountBytes(input_html, output_html);
}

}  // namespace net_instaweb
