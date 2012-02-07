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

// Author: guptaa@google.com (Ashish Gupta)

#include "net/instaweb/rewriter/public/javascript_url_manager.h"

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/gtest.h"

namespace net_instaweb {

namespace {

class JavascriptUrlManagerTest : public ::testing::Test {
 protected:

  JavascriptUrlManagerTest() {
    url_namer_.set_proxy_domain("http://proxy-domain");
  }

  UrlNamer url_namer_;
  RewriteOptions options_;
};

TEST_F(JavascriptUrlManagerTest, TestBlinkHandler) {
  JavascriptUrlManager manager(&url_namer_, false, "");
  const char blink_url[] = "http://proxy-domain/psajs/blink.js";
  EXPECT_STREQ(blink_url, manager.GetBlinkJsUrl(&options_));
}

TEST_F(JavascriptUrlManagerTest, TestBlinkGstatic) {
  JavascriptUrlManager manager(&url_namer_, true, "1");
  const char blink_url[] = "http://www.gstatic.com/psa/static/1-blink.js";
  EXPECT_STREQ(blink_url, manager.GetBlinkJsUrl(&options_));
}

TEST_F(JavascriptUrlManagerTest, TestBlinkDebug) {
  JavascriptUrlManager manager(&url_namer_, true, "1");
  options_.EnableFilter(RewriteOptions::kDebug);
  const char blink_url[] = "http://proxy-domain/psajs/blink.js";
  EXPECT_STREQ(blink_url, manager.GetBlinkJsUrl(&options_));
}

}  // namespace

} // namespace net_instaweb
