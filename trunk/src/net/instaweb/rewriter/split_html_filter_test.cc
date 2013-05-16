/*
 * Copyright 2012 Google Inc.
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

// Author: rahulbansal@google.com (Rahul Bansal)

#include "net/instaweb/rewriter/public/split_html_filter.h"

#include "net/instaweb/htmlparse/public/html_writer_filter.h"
#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/critical_line_info.pb.h"
#include "net/instaweb/rewriter/flush_early.pb.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/rewriter/public/test_rewrite_driver_factory.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/string_writer.h"

namespace net_instaweb {

namespace {

const char kRequestUrl[] = "http://www.test.com";

const char kHtmlInputPart1[] =
    "<html>"
    "<head>\n"
    "<script>blah</script>"
    "</head>\n"
    "<body>\n"
    "<div id=\"header\"> This is the header </div>"
    "<div id=\"container\" class>"
      "<h2 id=\"beforeItems\"> This is before Items </h2>"
      "<div id=\"item\">"
         "<img src=\"image1\" pagespeed_high_res_src=\"image1_high_res\""
           " onload=\"func\">"
         "<img src=\"image2\" pagespeed_high_res_src=\"image2_high_res\">"
      "</div>"
      "<span id=\"between\"> This is in between </span>"
      "<div id=\"inspiration\">"
         "<img src=\"image11\">"
      "</div>";

const char kHtmlInputPart2[] =
      "<h3 id=\"afterInspirations\"> This is after Inspirations </h3>"
    "</div>"
    "<img id=\"image\" src=\"image_panel.1\">"
    "<script pagespeed_no_defer=\"\"></script>"
    "<h1 id=\"footer\" name style>"
      "This is the footer"
    "</h1>"
    "</body></html>";

const char kSplitHtmlPrefix[] =
    "<html><head>"
    "\n<script>blah</script>";

const char kSplitHtmlMiddle[] =
    "</head>\n"
    "<body>\n"
    "<div id=\"header\"> This is the header </div>"
    "<div id=\"container\" class>"
      "<h2 id=\"beforeItems\"> This is before Items </h2>"
      "<div id=\"item\">"
        "<img src=\"image1\" pagespeed_high_res_src=\"image1_high_res\""
          " onload=\"pagespeed.splitOnload();func\">"
        "<img src=\"image2\" pagespeed_high_res_src=\"image2_high_res\">"
      "</div>"
      "<span id=\"between\"> This is in between </span>"
      "<!--GooglePanel begin panel-id.0--><!--GooglePanel end panel-id.0-->"
    "</div>"
    "<!--GooglePanel begin panel-id.1--><!--GooglePanel end panel-id.1-->"
    "<script pagespeed_no_defer=\"\"></script>"
    "<h1 id=\"footer\" name style>"
      "This is the footer"
    "</h1>"
    "</body></html>";

const char kSplitHtmlMiddleWithoutPanelStubs[] =
    "</head>\n"
    "<body>\n"
    "<div id=\"header\"> This is the header </div>"
    "<div id=\"container\" class>"
      "<h2 id=\"beforeItems\"> This is before Items </h2>"
      "<div id=\"item\">"
         "<img src=\"image1\" pagespeed_high_res_src=\"image1_high_res\""
           " onload=\"pagespeed.splitOnload();func\">"
         "<img src=\"image2\" pagespeed_high_res_src=\"image2_high_res\">"
      "</div>"
      "<span id=\"between\"> This is in between </span>"
      "<div id=\"inspiration\">"
         "<img src=\"image11\">"
      "</div>";

const char kSplitHtmlBelowTheFoldData[] =
       "{\"panel-id.0\":[{\"instance_html\":\"__psa_lt;div id=\\\"inspiration\\\" panel-id=\\\"panel-id.0\\\"__psa_gt;__psa_lt;img src=\\\"image11\\\"__psa_gt;__psa_lt;/div__psa_gt;__psa_lt;h3 id=\\\"afterInspirations\\\" panel-id=\\\"panel-id.0\\\"__psa_gt; This is after Inspirations __psa_lt;/h3__psa_gt;\"}],"
       "\"panel-id.1\":[{\"instance_html\":\"__psa_lt;img id=\\\"image\\\" src=\\\"image_panel.1\\\" panel-id=\\\"panel-id.1\\\"__psa_gt;\"}]}";

const char kHtmlInputForLazyload[] = "<html><head></head><body></body></html>";

const char kHtmlInputForIgnoreScript[] =
    "<html><body>%s<h1></h1>%s<h1></h1></body></html>";

const char kHtmlExpectedOutputForIgnoreScript1[] =
    "<html><head>%s</head><body>%s<h1></h1>%s"
    "<!--GooglePanel begin panel-id.0--><!--GooglePanel end panel-id.0-->"
    "</body></html>%s";

const char kHtmlExpectedOutputForIgnoreScript2[] =
    "<html><head>%s</head><body>%s"
    "<!--GooglePanel begin panel-id.0--><!--GooglePanel end panel-id.0-->"
    "</body></html>%s";

class SplitHtmlFilterTest : public RewriteTestBase {
 public:
  SplitHtmlFilterTest(): writer_(&output_) {}

  virtual bool AddHtmlTags() const { return false; }

 protected:
  virtual void SetUp() {
    delete options_;
    options_ = new RewriteOptions(factory()->thread_system());
    options_->DisableFilter(RewriteOptions::kHtmlWriterFilter);
    RewriteTestBase::SetUp();

    rewrite_driver()->SetRequestHeaders(request_headers_);
    rewrite_driver()->SetUserAgent("");
    rewrite_driver()->SetWriter(&writer_);
    SplitHtmlFilter* filter = new SplitHtmlFilter(rewrite_driver());
    html_writer_filter_.reset(filter);
    html_writer_filter_->set_writer(&writer_);
    rewrite_driver()->AddFilter(html_writer_filter_.get());

    response_headers_.set_status_code(HttpStatus::kOK);
    response_headers_.SetDateAndCaching(MockTimer::kApr_5_2010_ms, 0);
    rewrite_driver_->set_response_headers_ptr(&response_headers_);
    output_.clear();
    StaticAssetManager* static_asset_manager =
        rewrite_driver_->server_context()->static_asset_manager();
    blink_js_url_ = static_asset_manager->GetAssetUrl(
        StaticAssetManager::kBlinkJs, options_).c_str();
  }

  // TODO(marq): This looks reusable enough to go into RewriteTestBase. Perhaps
  // it should also know the rewriter-under-test's ID so there's less
  // boilerplate?
  void VerifyAppliedRewriters(GoogleString expected_rewriters) {
    EXPECT_STREQ(expected_rewriters, AppliedRewriterStringFromLog());
  }

  void VerifyJsonSize(int64 expected_size) {
    int64 actual_size = 0;
    if (logging_info()->has_split_html_info()) {
      actual_size = logging_info()->split_html_info().json_size();
    }
    EXPECT_EQ(expected_size, actual_size);
  }

  GoogleString output_;
  const char* blink_js_url_;

 private:
  StringWriter writer_;
  RequestHeaders request_headers_;
  ResponseHeaders response_headers_;
  SplitHtmlFilter* split_html_filter_;
};

TEST_F(SplitHtmlFilterTest, SplitHtmlWithDriverHavingCriticalLineInfo) {
  CriticalLineInfo* config = new CriticalLineInfo;
  Panel* panel = config->add_panels();
  panel->set_start_xpath("div[@id = \"container\"]/div[4]");
  panel = config->add_panels();
  panel->set_start_xpath("img[3]");
  panel->set_end_marker_xpath("h1[@id = \"footer\"]");
  rewrite_driver()->set_critical_line_info(config);

  Parse("split_with_pcache", StrCat(kHtmlInputPart1, kHtmlInputPart2));
  GoogleString suffix(StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                                   1, blink_js_url_,
                                   kSplitHtmlBelowTheFoldData, "false"));
  EXPECT_EQ(StrCat(kSplitHtmlPrefix, SplitHtmlFilter::kSplitInit,
                   kSplitHtmlMiddle, suffix),
            output_);
  VerifyAppliedRewriters(
      RewriteOptions::FilterId(RewriteOptions::kSplitHtml));
  VerifyJsonSize(strlen(kSplitHtmlBelowTheFoldData));
}

TEST_F(SplitHtmlFilterTest, SplitHtmlWithFlushingCachedHtml) {
  CriticalLineInfo* config = new CriticalLineInfo;
  Panel* panel = config->add_panels();
  panel->set_start_xpath("div[@id = \"container\"]/div[4]");
  panel = config->add_panels();
  panel->set_start_xpath("img[3]");
  panel->set_end_marker_xpath("h1[@id = \"footer\"]");
  rewrite_driver()->set_critical_line_info(config);
  rewrite_driver()->set_flushing_cached_html(true);

  Parse("split_with_pcache", StrCat(kHtmlInputPart1, kHtmlInputPart2));
  GoogleString suffix(StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                                   1, blink_js_url_,
                                   kSplitHtmlBelowTheFoldData, "true"));
  EXPECT_EQ(StrCat(kSplitHtmlPrefix, SplitHtmlFilter::kSplitInit,
                   kSplitHtmlMiddle, suffix),
            output_);
  VerifyAppliedRewriters(
      RewriteOptions::FilterId(RewriteOptions::kSplitHtml));
  VerifyJsonSize(strlen(kSplitHtmlBelowTheFoldData));
}

TEST_F(SplitHtmlFilterTest, SplitHtmlWithOptions) {
  options_->set_critical_line_config(
      "div[@id = \"container\"]/div[4],"
      "img[3]:h1[@id = \"footer\"]");
  Parse("split_with_options", StrCat(kHtmlInputPart1, kHtmlInputPart2));
  GoogleString suffix(StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                                   1, blink_js_url_,
                                   kSplitHtmlBelowTheFoldData, "false"));
  EXPECT_EQ(StrCat(kSplitHtmlPrefix, SplitHtmlFilter::kSplitInit,
                   kSplitHtmlMiddle, suffix),
            output_);
  VerifyAppliedRewriters(
      RewriteOptions::FilterId(RewriteOptions::kSplitHtml));
  VerifyJsonSize(strlen(kSplitHtmlBelowTheFoldData));
}

TEST_F(SplitHtmlFilterTest, SplitHtmlWithFlushes) {
  options_->set_critical_line_config(
      "div[@id = \"container\"]/div[4],"
      "img[3]:h1[@id = \"footer\"]");
  html_parse()->StartParse("http://test.com/");
  html_parse()->ParseText(kHtmlInputPart1);
  html_parse()->Flush();
  html_parse()->ParseText(kHtmlInputPart2);
  html_parse()->FinishParse();
  GoogleString suffix(StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                                   1, blink_js_url_,
                                   kSplitHtmlBelowTheFoldData, "false"));
  EXPECT_EQ(StrCat(kSplitHtmlPrefix, SplitHtmlFilter::kSplitInit,
                   kSplitHtmlMiddle, suffix),
            output_);
  VerifyAppliedRewriters(
      RewriteOptions::FilterId(RewriteOptions::kSplitHtml));
  VerifyJsonSize(strlen(kSplitHtmlBelowTheFoldData));
}

TEST_F(SplitHtmlFilterTest, FlushEarlyHeadSuppress) {
  options_->ForceEnableFilter(
      RewriteOptions::RewriteOptions::kFlushSubresources);
  options_->set_critical_line_config(
      "div[@id = \"container\"]/div[4],"
      "img[3]:h1[@id = \"footer\"]");

  GoogleString pre_head_input = "<!DOCTYPE html><html><head>";
  GoogleString post_head_input =
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"a.css\"/>"
      "<script src=\"b.js\"></script>"
      "</head>"
      "<body></body></html>";
  GoogleString suffix(StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                                   0, blink_js_url_, "{}", "false"));
  GoogleString post_head_output = StrCat(
      "<link type=\"text/css\" rel=\"stylesheet\" href=\"a.css\"/>"
      "<script src=\"b.js\"></script>",
      SplitHtmlFilter::kSplitInit,
      "</head><body></body></html>", suffix);
  GoogleString html_input = StrCat(pre_head_input, post_head_input);

  Parse("not_flushed_early", html_input);
  EXPECT_EQ(StrCat(pre_head_input, post_head_output), output_);
  VerifyAppliedRewriters("");
  VerifyJsonSize(0);

  // SuppressPreheadFilter should have populated the flush_early_proto with the
  // appropriate pre head information.
  EXPECT_EQ(pre_head_input,
            rewrite_driver()->flush_early_info()->pre_head());

  // pre head is suppressed if the dummy head was flushed early.
  output_.clear();
  rewrite_driver()->set_flushed_early(true);
  Parse("flushed_early", html_input);
  EXPECT_EQ(post_head_output, output_);
  VerifyAppliedRewriters("");
  VerifyJsonSize(0);
}

TEST_F(SplitHtmlFilterTest, FlushEarlyDisabled) {
  options_->set_critical_line_config(
      "div[@id = \"container\"]/div[4],"
      "img[3]:h1[@id = \"footer\"]");

  const char pre_head_input[] = "<!DOCTYPE html><html>";
  const char post_head_input[] =
      "<head>"
        "<link type=\"text/css\" rel=\"stylesheet\" href=\"a.css\"/>"
        "<script src=\"b.js\"></script>"
      "</head>"
      "<body></body></html>";
  GoogleString html_input = StrCat(pre_head_input, post_head_input);

  Parse("not_flushed_early", html_input);

  // SuppressPreheadFilter should not have populated the flush_early_proto.
  EXPECT_EQ("", rewrite_driver()->flush_early_info()->pre_head());
  VerifyAppliedRewriters("");
  VerifyJsonSize(0);
}

TEST_F(SplitHtmlFilterTest, SplitHtmlNoXpaths) {
  CriticalLineInfo* info = new CriticalLineInfo;
  rewrite_driver()->set_critical_line_info(info);
  options_->set_critical_line_config("");
  Parse("split_without_xpaths", StrCat(kHtmlInputPart1, kHtmlInputPart2));
  GoogleString expected_output(kSplitHtmlPrefix);
  GoogleString suffix(StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                                   1, blink_js_url_, "{}", "false"));
  StrAppend(&expected_output, SplitHtmlFilter::kSplitInit,
            kSplitHtmlMiddleWithoutPanelStubs,
            kHtmlInputPart2, suffix);
  EXPECT_EQ(expected_output, output_);
  VerifyAppliedRewriters("");
  VerifyJsonSize(0);
}

TEST_F(SplitHtmlFilterTest, SplitHtmlWithUnsupportedUserAgent) {
  options_->set_critical_line_config(
      "div[@id = \"container\"]/div[4],"
      "img[3]:h1[@id = \"footer\"]");
  rewrite_driver()->SetUserAgent("BlackListUserAgent");
  Parse("split_with_options", StrCat(kHtmlInputPart1, kHtmlInputPart2));
  EXPECT_EQ(StrCat(kHtmlInputPart1, kHtmlInputPart2), output_);
  VerifyAppliedRewriters("");
  VerifyJsonSize(0);
}

TEST_F(SplitHtmlFilterTest, SplitHtmlIgnoreScriptNoscript1) {
  options_->set_critical_line_config("h1[2]");
  GoogleString expected_output_suffix(
      StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                   0, blink_js_url_, "{\"panel-id.0\":[{\"instance_html\":"
                   "\"__psa_lt;h1 panel-id=\\\"panel-id.0\\\"__psa_gt;"
                   "__psa_lt;/h1__psa_gt;\"}]}", "false"));
  GoogleString input(StringPrintf(kHtmlInputForIgnoreScript, "", ""));
  Parse("split_ignore_script1", input);
  EXPECT_EQ(StringPrintf(kHtmlExpectedOutputForIgnoreScript1,
                         SplitHtmlFilter::kSplitInit, "", "",
                         expected_output_suffix.c_str()).c_str(), output_);
  VerifyAppliedRewriters("sh");
}

TEST_F(SplitHtmlFilterTest, SplitHtmlIgnoreScriptNoscript2) {
  options_->set_critical_line_config("h1[2]");
  GoogleString expected_output_suffix(
      StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                   0, blink_js_url_, "{\"panel-id.0\":[{\"instance_html\":"
                   "\"__psa_lt;h1 panel-id=\\\"panel-id.0\\\"__psa_gt;"
                   "__psa_lt;/h1__psa_gt;\"}]}", "false"));
  GoogleString input = StringPrintf(kHtmlInputForIgnoreScript, "",
                                    "<script></script><noscript></noscript>");
  Parse("split_ignore_script2", input);
  EXPECT_EQ(StringPrintf(kHtmlExpectedOutputForIgnoreScript1,
                         SplitHtmlFilter::kSplitInit, "",
                         "<script></script><noscript></noscript>",
                         expected_output_suffix.c_str()).c_str(), output_);
  VerifyAppliedRewriters("sh");
}

TEST_F(SplitHtmlFilterTest, SplitHtmlIgnoreScriptNoscript3) {
  options_->set_critical_line_config("h1[2]");
  GoogleString expected_output_suffix(
      StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                   0, blink_js_url_, "{\"panel-id.0\":[{\"instance_html\":"
                   "\"__psa_lt;h1 panel-id=\\\"panel-id.0\\\"__psa_gt;"
                   "__psa_lt;/h1__psa_gt;\"}]}", "false"));
  GoogleString input = StringPrintf(kHtmlInputForIgnoreScript,
                                    "<script></script><noscript></noscript>",
                                    "<script></script><noscript></noscript>");
  Parse("split_ignore_script3", input);
  EXPECT_EQ(StringPrintf(kHtmlExpectedOutputForIgnoreScript1,
                         SplitHtmlFilter::kSplitInit,
                         "<script></script><noscript></noscript>",
                         "<script></script><noscript></noscript>",
                         expected_output_suffix.c_str()).c_str(), output_);
  VerifyAppliedRewriters("sh");
}

TEST_F(SplitHtmlFilterTest, SplitHtmlIgnoreScriptNoscript4) {
  options_->set_critical_line_config("h1[1]");
  GoogleString expected_output_suffix(
      StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                   0, blink_js_url_, "{\"panel-id.0\":[{\"instance_html\":"
                   "\"__psa_lt;h1 panel-id=\\\"panel-id.0\\\"__psa_gt;"
                   "__psa_lt;/h1__psa_gt;"
                   "__psa_lt;h1 panel-id=\\\"panel-id.0\\\"__psa_gt;"
                   "__psa_lt;/h1__psa_gt;\"}]}", "false"));
  GoogleString input = StringPrintf(kHtmlInputForIgnoreScript, "", "");
  Parse("split_ignore_script4", input);
  EXPECT_EQ(StringPrintf(kHtmlExpectedOutputForIgnoreScript2,
                         SplitHtmlFilter::kSplitInit, "",
                         expected_output_suffix.c_str()).c_str(), output_);
  VerifyAppliedRewriters("sh");
}

TEST_F(SplitHtmlFilterTest, SplitHtmlIgnoreScriptNoscript5) {
  options_->set_critical_line_config("h1[1]");
  GoogleString expected_output_suffix(
      StringPrintf(SplitHtmlFilter::kSplitSuffixJsFormatString,
                   0, blink_js_url_, "{\"panel-id.0\":[{\"instance_html\":"
                   "\"__psa_lt;h1 panel-id=\\\"panel-id.0\\\"__psa_gt;"
                   "__psa_lt;/h1__psa_gt;"
                   "__psa_lt;h1 panel-id=\\\"panel-id.0\\\"__psa_gt;"
                   "__psa_lt;/h1__psa_gt;\"}]}", "false"));
  GoogleString input = StringPrintf(
      kHtmlInputForIgnoreScript,
      "<script></script><noscript></noscript>"
      "<style></style><link href=\"http://a.com/\">", "");
      Parse("split_ignore_script5", input);
  EXPECT_EQ(StringPrintf(kHtmlExpectedOutputForIgnoreScript2,
                         SplitHtmlFilter::kSplitInit,
                         "<script></script><noscript></noscript>"
                         "<style></style><link href=\"http://a.com/\">",
                         expected_output_suffix.c_str()).c_str(), output_);
  VerifyAppliedRewriters("sh");
}

}  // namespace

}  // namespace net_instaweb
