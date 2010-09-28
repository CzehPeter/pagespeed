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
// Author: jmaessen@google.com (Jan Maessen)

#include "net/instaweb/rewriter/public/javascript_code_block.h"

#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include <string>

namespace net_instaweb {

// This sample code comes from Douglas Crockford's jsmin example.
// The same code is used to test jsmin in pagespeed.
std::string kBeforeCompilation =
    "// is.js\n"
    "\n"
    "// (c) 2001 Douglas Crockford\n"
    "// 2001 June 3\n"
    "\n"
    "\n"
    "// is\n"
    "\n"
    "// The -is- object is used to identify the browser.  "
    "Every browser edition\n"
    "// identifies itself, but there is no standard way of doing it, "
    "and some of\n"
    "// the identification is deceptive. This is because the authors of web\n"
    "// browsers are liars. For example, Microsoft's IE browsers claim to be\n"
    "// Mozilla 4. Netscape 6 claims to be version 5.\n"
    "\n"
    "var is = {\n"
    "    ie:      navigator.appName == 'Microsoft Internet Explorer',\n"
    "    java:    navigator.javaEnabled(),\n"
    "    ns:      navigator.appName == 'Netscape',\n"
    "    ua:      navigator.userAgent.toLowerCase(),\n"
    "    version: parseFloat(navigator.appVersion.substr(21)) ||\n"
    "             parseFloat(navigator.appVersion),\n"
    "    win:     navigator.platform == 'Win32'\n"
    "}\n"
    "is.mac = is.ua.indexOf('mac') >= 0;\n"
    "if (is.ua.indexOf('opera') >= 0) {\n"
    "    is.ie = is.ns = false;\n"
    "    is.opera = true;\n"
    "}\n"
    "if (is.ua.indexOf('gecko') >= 0) {\n"
    "    is.ie = is.ns = false;\n"
    "    is.gecko = true;\n"
    "}\n";

std::string kTruncatedComment =
    "// is.js\n"
    "\n"
    "// (c) 2001 Douglas Crockford\n"
    "// 2001 June 3\n"
    "\n"
    "\n"
    "// is\n"
    "\n"
    "/* The -is- object is used to identify the browser.  "
    "Every browser edition\n"
    "   identifies itself, but there is no standard way of doing it, "
    "and some of\n";

std::string kTruncatedRewritten =
    "// is.js\n"
    "\n"
    "// (c) 2001 Douglas Crockford\n"
    "// 2001 June 3\n"
    "\n"
    "\n"
    "// is\n"
    "\n"
    "/* The -is- object is used to identify the browser.  "
    "Every browser edition\n"
    "   identifies itself, but there is no standard way of doing it, "
    "and some of";

std::string kTruncatedString =
    "var is = {\n"
    "    ie:      navigator.appName == 'Microsoft Internet Explo";

std::string kAfterCompilation =
    "var is={ie:navigator.appName=='Microsoft Internet Explorer',"
    "java:navigator.javaEnabled(),ns:navigator.appName=='Netscape',"
    "ua:navigator.userAgent.toLowerCase(),version:parseFloat("
    "navigator.appVersion.substr(21))||parseFloat(navigator.appVersion)"
    ",win:navigator.platform=='Win32'}\n"
    "is.mac=is.ua.indexOf('mac')>=0;if(is.ua.indexOf('opera')>=0){"
    "is.ie=is.ns=false;is.opera=true;}\n"
    "if(is.ua.indexOf('gecko')>=0){is.ie=is.ns=false;is.gecko=true;}";

TEST(JsCodeBlockTest, Config) {
  JavascriptRewriteConfig config;
  EXPECT_TRUE(config.minify());
  config.set_minify(false);
  EXPECT_FALSE(config.minify());
  config.set_minify(true);
  EXPECT_TRUE(config.minify());
}

TEST(JsCodeBlockTest, Rewrite) {
  JavascriptRewriteConfig config;
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kBeforeCompilation, config, &handler);
  EXPECT_TRUE(block.ProfitableToRewrite());
  EXPECT_EQ(kAfterCompilation, block.Rewritten());
}

TEST(JsCodeBlockTest, NoRewrite) {
  JavascriptRewriteConfig config;
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kAfterCompilation, config, &handler);
  EXPECT_FALSE(block.ProfitableToRewrite());
  EXPECT_EQ(kAfterCompilation, block.Rewritten());
}

TEST(JsCodeBlockTest, TruncatedComment) {
  JavascriptRewriteConfig config;
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kTruncatedComment, config, &handler);
  EXPECT_TRUE(block.ProfitableToRewrite());
  EXPECT_EQ(kTruncatedRewritten, block.Rewritten());
}

TEST(JsCodeBlockTest, TruncatedString) {
  JavascriptRewriteConfig config;
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kTruncatedString, config, &handler);
  EXPECT_FALSE(block.ProfitableToRewrite());
  EXPECT_EQ(kTruncatedString, block.Rewritten());
}

TEST(JsCodeBlockTest, NoMinification) {
  JavascriptRewriteConfig config;
  config.set_minify(false);
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kBeforeCompilation, config, &handler);
  EXPECT_FALSE(block.ProfitableToRewrite());
  EXPECT_EQ(kBeforeCompilation, block.Rewritten());
}

}  // namespace net_instaweb
