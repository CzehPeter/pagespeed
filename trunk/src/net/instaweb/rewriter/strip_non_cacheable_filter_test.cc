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

#include "net/instaweb/rewriter/public/strip_non_cacheable_filter.h"

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/rewriter/public/test_rewrite_driver_factory.h"
#include "pagespeed/kernel/base/gtest.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

const char kRequestUrl[] = "http://www.test.com";

const char kHtmlInput[] =
    "<html>"
    "<body>"
    "<noscript>This should not get removed</noscript>"
    "<div id=\"header\"> This is the header </div>"
    "<div id=\"container\" class>"
      "<h2 id=\"beforeItems\"> This is before Items </h2>"
      "<div class=\"Item\">"
         "<img src=\"image1\">"
         "<img src=\"image2\">"
      "</div>"
      "<div class=\"item lots of classes here for testing\">"
         "<img src=\"image3\">"
          "<div class=\"item\">"
             "<img src=\"image4\">"
          "</div>"
      "</div>"
      "<div class=\"itema itemb others are ok\">"
        "<img src=\"image5\">"
      "</div>"
      "<div class=\"itemb before itema\">"
        "<img src=\"image6\">"
      "</div>"
      "<div class=\"itemb only\">"
        "<img src=\"image7\">"
      "</div>"
    "</body></html>";

const char kBlinkUrlHandler[] = "/psajs/blink.js";
const char kBlinkUrlGstatic[] = "http://www.gstatic.com/psa/static/1-blink.js";
const char kPsaHeadScriptNodesStart[] =
    "<script type=\"text/javascript\" pagespeed_no_defer=\"\" src=\"";

const char kPsaHeadScriptNodesEnd[] =
    "\"></script>";

}  // namespace


class StripNonCacheableFilterTest : public RewriteTestBase {
 public:
  StripNonCacheableFilterTest() {}

  virtual ~StripNonCacheableFilterTest() {}

  virtual void SetUp() {
    delete options_;
    options_ = new RewriteOptions(factory()->thread_system());
    options_->EnableFilter(RewriteOptions::kStripNonCacheable);

    options_->set_non_cacheables_for_cache_partial_html(
        "class= \"item \" , id\t =beforeItems \t , class=\"itema itemb\"");

    SetUseManagedRewriteDrivers(true);
    RewriteTestBase::SetUp();
  }

  virtual bool AddHtmlTags() const { return false; }

 protected:
  GoogleString GetExpectedOutput(const StringPiece blink_js) {
    GoogleString psa_head_script_nodes = StrCat(
        kPsaHeadScriptNodesStart, blink_js, kPsaHeadScriptNodesEnd);
    return StrCat(
        "<html><body>",
        "<noscript>This should not get removed</noscript>"
        "<div id=\"header\"> This is the header </div>"
        "<div id=\"container\" class>"
        "<!--GooglePanel begin panel-id-1.0-->"
        "<!--GooglePanel end panel-id-1.0-->"
        "<!--GooglePanel begin panel-id-0.0-->"
        "<!--GooglePanel end panel-id-0.0-->"
        "<!--GooglePanel begin panel-id-0.1-->"
        "<!--GooglePanel end panel-id-0.1-->"
        "<!--GooglePanel begin panel-id-2.0-->"
        "<!--GooglePanel end panel-id-2.0-->"
        "<!--GooglePanel begin panel-id-2.1-->"
        "<!--GooglePanel end panel-id-2.1-->"
        "<div class=\"itemb only\"><img src=\"image7\"></div>"
        "</body></html>");
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(StripNonCacheableFilterTest);
};

TEST_F(StripNonCacheableFilterTest, StripNonCacheable) {
  ValidateExpectedUrl(kRequestUrl, kHtmlInput,
                      GetExpectedOutput(kBlinkUrlHandler));
}

TEST_F(StripNonCacheableFilterTest, TestGstatic) {
  StaticAssetManager static_asset_manager(
      "", server_context()->thread_system(), server_context()->hasher(),
      server_context()->message_handler());
  static_asset_manager.ServeAssetsFromGStatic(StaticAssetManager::kGStaticBase);
  static_asset_manager.SetGStaticHashForTest(StaticAssetEnum::BLINK_JS, "1");
  server_context()->set_static_asset_manager(&static_asset_manager);
  ValidateExpectedUrl(kRequestUrl, kHtmlInput,
                      GetExpectedOutput(kBlinkUrlGstatic));
}

}  // namespace net_instaweb
