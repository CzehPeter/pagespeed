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

// Author: nikhilmadan@google.com (Nikhil Madan)

#include "net/instaweb/rewriter/public/critical_images_finder.h"
#include "net/instaweb/rewriter/public/lazyload_images_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class Statistics;

// By default, CriticalImagesFinder does not return meaningful results. However,
// this test manually manages the critical image set, so CriticalImagesFinder
// can return useful information for testing this filter.
class MeaningfulCriticalImagesFinder : public CriticalImagesFinder {
 public:
  explicit MeaningfulCriticalImagesFinder(Statistics* stats)
      : CriticalImagesFinder(stats) {}
  virtual ~MeaningfulCriticalImagesFinder() {}
  virtual bool IsMeaningful(const RewriteDriver* driver) const {
    return true;
  }
  virtual void ComputeCriticalImages(StringPiece url,
                                     RewriteDriver* driver) {}
  virtual const char* GetCriticalImagesCohort() const {
    return kCriticalImagesCohort;
  }
 private:
  static const char kCriticalImagesCohort[];
};

const char MeaningfulCriticalImagesFinder::kCriticalImagesCohort[] =
    "critical_images";

class LazyloadImagesFilterTest : public RewriteTestBase {
 protected:
  LazyloadImagesFilterTest()
      : blank_image_src_("/psajs/1.0.gif") {}

  // TODO(matterbury): Delete this method as it should be redundant.
  virtual void SetUp() {
    RewriteTestBase::SetUp();
    SetHtmlMimetype();  // Prevent insertion of CDATA tags to static JS.
  }

  virtual void InitLazyloadImagesFilter(bool debug) {
    if (debug) {
      options()->EnableFilter(RewriteOptions::kDebug);
    }
    options()->DisallowTroublesomeResources();
    lazyload_images_filter_.reset(
        new LazyloadImagesFilter(rewrite_driver()));
    rewrite_driver()->AddFilter(lazyload_images_filter_.get());
  }

  GoogleString GetScriptHtml(const StringPiece& script, bool add_no_defer) {
    return StrCat("<script type=\"text/javascript\"",
                  add_no_defer ? " pagespeed_no_defer=\"\"" : "",
                  ">", script, "</script>");
  }

  GoogleString GetLazyloadScriptHtml() {
    return GetScriptHtml(
        LazyloadImagesFilter::GetLazyloadJsSnippet(
            options(), server_context()->static_asset_manager()),
        false);
  }

  GoogleString GetOverrideAttributesScriptHtml() {
    return GetScriptHtml(
        LazyloadImagesFilter::kOverrideAttributeFunctions, true);
  }

  GoogleString GenerateRewrittenImageTag(
      const StringPiece& tag,
      const StringPiece& url,
      const StringPiece& additional_attributes) {
    return StrCat("<", tag, " pagespeed_lazy_src=\"", url, "\" ",
                  additional_attributes,
                  StrCat("src=\"",
                         blank_image_src_,
                         "\" onload=\"", LazyloadImagesFilter::kImageOnloadCode,
                         "\"/>"));
  }

  GoogleString blank_image_src_;
  scoped_ptr<LazyloadImagesFilter> lazyload_images_filter_;
};

TEST_F(LazyloadImagesFilterTest, SingleHead) {
  InitLazyloadImagesFilter(false);

  ValidateExpected("lazyload_images",
      "<head></head>"
      "<body>"
      "<img />"
      "<img src=\"\" />"
      "<noscript>"
      "<img src=\"noscript.jpg\" />"
      "</noscript>"
      "<noembed>"
      "<img src=\"noembed.jpg\" />"
      "</noembed>"
      "<marquee>"
      "<img src=\"marquee.jpg\" />"
      "</marquee>"
      "<img src=\"1.jpg\" />"
      "<img src=\"1.jpg\" pagespeed_no_defer/>"
      "<img src=\"1.jpg\" data-src=\"2.jpg\"/>"
      "<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhE\"/>"
      "<img src=\"2's.jpg\" height=\"300\" width=\"123\" />"
      "<input src=\"12.jpg\"type=\"image\" />"
      "<input src=\"12.jpg\" />"
      "<img src=\"1.jpg\" onload=\"blah();\" />"
      "<img src=\"1.jpg\" class=\"123 dfcg-metabox\" />"
      "</body>",
      StrCat("<head></head><body><img/>"
             "<img src=\"\"/>"
             "<noscript>"
             "<img src=\"noscript.jpg\"/>"
             "</noscript>",
             "<noembed>"
             "<img src=\"noembed.jpg\"/>"
             "</noembed>"
             "<marquee>"
             "<img src=\"marquee.jpg\"/>"
             "</marquee>",
             GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "1.jpg", ""),
             "<img src=\"1.jpg\"/>"
             "<img src=\"1.jpg\" data-src=\"2.jpg\"/>",
             StrCat("<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhE\"/>",
                    GenerateRewrittenImageTag("img", "2's.jpg",
                                              "height=\"300\" width=\"123\" "),
                    "<input src=\"12.jpg\" type=\"image\"/>"
                    "<input src=\"12.jpg\"/>"
                    "<img src=\"1.jpg\" onload=\"blah();\"/>"
                    "<img src=\"1.jpg\" class=\"123 dfcg-metabox\"/>",
                    GetOverrideAttributesScriptHtml(),
                    "</body>")));
}

TEST_F(LazyloadImagesFilterTest, Blacklist) {
  options()->Disallow("*blacklist*");
  InitLazyloadImagesFilter(false);

  GoogleString input_html= "<head></head>"
      "<body>"
      "<img src=\"http://www.1.com/blacklist.jpg\"/>"
      "<img src=\"http://www.1.com/img1\"/>"
      "<img src=\"img2\"/>"
      "</body>";

  ValidateExpected(
      "lazyload_images",
      input_html,
      StrCat("<head></head><body>"
             "<img src=\"http://www.1.com/blacklist.jpg\"/>",
             GetLazyloadScriptHtml(),
             StrCat(
                 GenerateRewrittenImageTag(
                     "img", "http://www.1.com/img1", ""),
                 GenerateRewrittenImageTag(
                     "img", "img2", ""),
                 GetOverrideAttributesScriptHtml(),
                 "</body>")));
}

TEST_F(LazyloadImagesFilterTest, CriticalImages) {
  InitLazyloadImagesFilter(false);
  StringSet* critical_images = new StringSet;
  critical_images->insert("http://www.1.com/critical");
  critical_images->insert("www.1.com/critical2");
  critical_images->insert("http://test.com/critical3");
  critical_images->insert("http://test.com/critical4.jpg");

  rewrite_driver()->set_critical_images(critical_images);
  server_context()->set_critical_images_finder(
      new MeaningfulCriticalImagesFinder(statistics()));

  GoogleString rewritten_url = Encode(
      "http://test.com/", "ce", "HASH", "critical4.jpg", "jpg");

  GoogleString input_html= StrCat(
      "<head></head>"
      "<body>"
      "<img src=\"http://www.1.com/critical\"/>"
      "<img src=\"http://www.1.com/critical2\"/>"
      "<img src=\"critical3\"/>"
      "<img src=\"", rewritten_url, "\"/>"
      "</body>");

  ValidateExpected(
      "lazyload_images",
      input_html,
      StrCat("<head></head><body>"
             "<img src=\"http://www.1.com/critical\"/>",
             GetLazyloadScriptHtml(),
             StrCat(
                 GenerateRewrittenImageTag(
                     "img", "http://www.1.com/critical2", ""),
                 "<img src=\"critical3\"/>"
                 "<img src=\"", rewritten_url, "\"/>",
                 GetOverrideAttributesScriptHtml(),
                 "</body>")));
}

TEST_F(LazyloadImagesFilterTest, SingleHeadLoadOnOnload) {
  options()->set_lazyload_images_after_onload(true);
  InitLazyloadImagesFilter(false);
  ValidateExpected("lazyload_images",
      "<head></head>"
      "<body>"
      "<img src=\"1.jpg\" />"
      "</body>",
      StrCat("<head></head>"
             "<body>",
             GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "1.jpg", ""),
             GetOverrideAttributesScriptHtml(),
             "</body>"));
}

TEST_F(LazyloadImagesFilterTest, MultipleBodies) {
  InitLazyloadImagesFilter(false);
  ValidateExpected("lazyload_images",
      "<body><img src=\"1.jpg\" /></body>"
      "<body></body>"
      "<body>"
      "<script></script>"
      "<img src=\"2.jpg\" />"
      "<script></script>"
      "<img src=\"3.jpg\" />"
      "<script></script>"
      "</body>",
      StrCat(
          "<body>",
          GetLazyloadScriptHtml(),
          GenerateRewrittenImageTag("img", "1.jpg", ""),
          GetOverrideAttributesScriptHtml(),
          StrCat(
              "</body><body></body><body>"
              "<script></script>",
              GenerateRewrittenImageTag("img", "2.jpg", ""),
              GetOverrideAttributesScriptHtml()),
          StrCat(
              "<script></script>",
              GenerateRewrittenImageTag("img", "3.jpg", ""),
              GetOverrideAttributesScriptHtml(),
              "<script></script>",
              "</body>")));
}

TEST_F(LazyloadImagesFilterTest, NoHeadTag) {
  InitLazyloadImagesFilter(false);
  ValidateExpected("lazyload_images",
      "<body>"
      "<img src=\"1.jpg\" />"
      "</body>",
      StrCat("<body>",
             GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "1.jpg", ""),
             GetOverrideAttributesScriptHtml(),
             "</body>"));
}

TEST_F(LazyloadImagesFilterTest, LazyloadImagesPreserveURLsOn) {
  // Make sure that we do not lazyload images when preserve urls is off.
  // This is a modification of the NoHeadTag test.
  options()->set_image_preserve_urls(true);
  options()->set_support_noscript_enabled(false);
  AddFilter(RewriteOptions::kLazyloadImages);

  ValidateNoChanges("lazyload_images",
      "<body>"
      "<img src=\"1.jpg\"/>"
      "</body>");
}

TEST_F(LazyloadImagesFilterTest, CustomImageUrl) {
  GoogleString blank_image_url = "http://blank.com/1.gif";
  options()->set_lazyload_images_blank_url(blank_image_url);
  blank_image_src_ = blank_image_url;
  InitLazyloadImagesFilter(false);
  ValidateExpected("lazyload_images",
      "<body>"
      "<img src=\"1.jpg\" />"
      "</body>",
      StrCat("<body>",
             GetLazyloadScriptHtml(),
             GenerateRewrittenImageTag("img", "1.jpg", ""),
             GetOverrideAttributesScriptHtml(),
             "</body>"));
}

TEST_F(LazyloadImagesFilterTest, DfcgClass) {
  InitLazyloadImagesFilter(false);
  GoogleString input_html = "<body class=\"dfcg-slideshow\">"
      "<img src=\"1.jpg\"/>"
      "<div class=\"dfcg\">"
      "<img src=\"1.jpg\"/>"
      "</div>"
      "</body>";
  ValidateNoChanges("lazyload_images", input_html);
}

TEST_F(LazyloadImagesFilterTest, NivoClass) {
  InitLazyloadImagesFilter(false);
  GoogleString input_html = "<body>"
      "<div class=\"nivo_sl\">"
      "<img src=\"1.jpg\"/>"
      "</div>"
      "<img class=\"nivo\" src=\"1.jpg\"/>"
      "</body>";
  ValidateNoChanges("lazyload_images", input_html);
}

TEST_F(LazyloadImagesFilterTest, ClassContainsSlider) {
  InitLazyloadImagesFilter(false);
  GoogleString input_html = "<body>"
      "<div class=\"SliderName2\">"
      "<img src=\"1.jpg\"/>"
      "</div>"
      "<img class=\"my_sLiDer\" src=\"1.jpg\"/>"
      "</body>";
  ValidateNoChanges("lazyload_images", input_html);
}

TEST_F(LazyloadImagesFilterTest, NoImages) {
  InitLazyloadImagesFilter(false);
  GoogleString input_html = "<head></head><body></body>";
  ValidateNoChanges("lazyload_images", input_html);
}

TEST_F(LazyloadImagesFilterTest, LazyloadScriptOptimized) {
  InitLazyloadImagesFilter(false);
  Parse("optimized",
        "<head></head><body><img src=\"1.jpg\"></body>");
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("/*"))
      << "There should be no comments in the optimized code";
}

TEST_F(LazyloadImagesFilterTest, LazyloadScriptDebug) {
  InitLazyloadImagesFilter(true);
  Parse("debug",
        "<head></head><body><img src=\"1.jpg\"></body>");
  EXPECT_NE(GoogleString::npos, output_buffer_.find("/*"))
      << "There should still be some comments in the debug code";
}

TEST_F(LazyloadImagesFilterTest, LazyloadDisabledWithJquerySlider) {
  InitLazyloadImagesFilter(false);
  GoogleString input_html = "<body>"
      "<head>"
      "<script src=\"jquery.sexyslider.js\"/>"
      "</head>"
      "<body>"
      "<img src=\"1.jpg\"/>"
      "</body>";
  // No change in the html.
  ValidateNoChanges("lazyload_images", input_html);
}

TEST_F(LazyloadImagesFilterTest, LazyloadDisabledWithJquerySliderAfterHead) {
  InitLazyloadImagesFilter(false);
  GoogleString input_html = "<head>"
      "</head>"
      "<body>"
      "<script src=\"jquery.sexyslider.js\"/>"
      "<img src=\"1.jpg\"/>"
      "</body>";
  ValidateNoChanges("abort_script_inserted", input_html);
}

}  // namespace net_instaweb
