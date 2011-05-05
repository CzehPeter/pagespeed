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
// Author: jmaessen@google.com (Jan Maessen)

#include "net/instaweb/rewriter/public/javascript_code_block.h"

#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/simple_stats.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

namespace {

// This sample code comes from Douglas Crockford's jsmin example.
// The same code is used to test jsminify in pagespeed.
const GoogleString kBeforeCompilation =
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

const GoogleString kTruncatedComment =
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

const GoogleString kTruncatedRewritten =
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

const GoogleString kTruncatedString =
    "var is = {\n"
    "    ie:      navigator.appName == 'Microsoft Internet Explo";

const GoogleString kAfterCompilation =
    "var is={ie:navigator.appName=='Microsoft Internet Explorer',"
    "java:navigator.javaEnabled(),ns:navigator.appName=='Netscape',"
    "ua:navigator.userAgent.toLowerCase(),version:parseFloat("
    "navigator.appVersion.substr(21))||parseFloat(navigator.appVersion)"
    ",win:navigator.platform=='Win32'}\n"
    "is.mac=is.ua.indexOf('mac')>=0;if(is.ua.indexOf('opera')>=0){"
    "is.ie=is.ns=false;is.opera=true;}\n"
    "if(is.ua.indexOf('gecko')>=0){is.ie=is.ns=false;is.gecko=true;}";

const char kJavascriptBlocksMinified[] = "javascript_blocks_minified";
const char kJavascriptBytesSaved[] = "javascript_bytes_saved";
const char kJavascriptMinificationFailures[] =
    "javascript_minification_failures";
const char kJavascriptTotalBlocks[] = "javascript_total_blocks";

void ExpectStats(const SimpleStats& stats,
                 int total_blocks, int minified_blocks, int failures,
                 int saved_bytes) {
  EXPECT_EQ(minified_blocks,
            stats.GetVariable(kJavascriptBlocksMinified)->Get());
  EXPECT_EQ(total_blocks,
            stats.GetVariable(kJavascriptTotalBlocks)->Get());
  EXPECT_EQ(failures,
            stats.GetVariable(kJavascriptMinificationFailures)->Get());
  EXPECT_EQ(saved_bytes,
            stats.GetVariable(kJavascriptBytesSaved)->Get());
}

TEST(JsCodeBlockTest, Config) {
  SimpleStats stats;
  JavascriptRewriteConfig::Initialize(&stats);
  JavascriptRewriteConfig config(&stats);
  EXPECT_TRUE(config.minify());
  config.set_minify(false);
  EXPECT_FALSE(config.minify());
  config.set_minify(true);
  EXPECT_TRUE(config.minify());
  ExpectStats(stats, 0, 0, 0, 0);
}

TEST(JsCodeBlockTest, Rewrite) {
  SimpleStats stats;
  JavascriptRewriteConfig::Initialize(&stats);
  JavascriptRewriteConfig config(&stats);
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kBeforeCompilation, &config, "Test", &handler);
  EXPECT_TRUE(block.ProfitableToRewrite());
  EXPECT_EQ(kAfterCompilation, block.Rewritten());
  ExpectStats(stats, 1, 1, 0,
              kBeforeCompilation.size() - kAfterCompilation.size());
}

TEST(JsCodeBlockTest, NoRewrite) {
  SimpleStats stats;
  JavascriptRewriteConfig::Initialize(&stats);
  JavascriptRewriteConfig config(&stats);
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kAfterCompilation, &config, "Test", &handler);
  EXPECT_FALSE(block.ProfitableToRewrite());
  EXPECT_EQ(kAfterCompilation, block.Rewritten());
  ExpectStats(stats, 1, 0, 0, 0);
}

TEST(JsCodeBlockTest, TruncatedComment) {
  SimpleStats stats;
  JavascriptRewriteConfig::Initialize(&stats);
  JavascriptRewriteConfig config(&stats);
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kTruncatedComment, &config, "Test", &handler);
  EXPECT_TRUE(block.ProfitableToRewrite());
  EXPECT_EQ(kTruncatedRewritten, block.Rewritten());
  ExpectStats(stats, 1, 1, 1,
              kTruncatedComment.size() - kTruncatedRewritten.size());
}

TEST(JsCodeBlockTest, TruncatedString) {
  SimpleStats stats;
  JavascriptRewriteConfig::Initialize(&stats);
  JavascriptRewriteConfig config(&stats);
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kTruncatedString, &config, "Test", &handler);
  EXPECT_FALSE(block.ProfitableToRewrite());
  EXPECT_EQ(kTruncatedString, block.Rewritten());
  ExpectStats(stats, 1, 0, 1, 0);
}

TEST(JsCodeBlockTest, NoMinification) {
  SimpleStats stats;
  JavascriptRewriteConfig::Initialize(&stats);
  JavascriptRewriteConfig config(&stats);
  config.set_minify(false);
  GoogleMessageHandler handler;
  JavascriptCodeBlock block(kBeforeCompilation, &config, "Test", &handler);
  EXPECT_FALSE(block.ProfitableToRewrite());
  EXPECT_EQ(kBeforeCompilation, block.Rewritten());
  ExpectStats(stats, 1, 0, 0, 0);
}

TEST(JsCodeBlockTest, DealWithSgmlComment) {
  SimpleStats stats;
  JavascriptRewriteConfig::Initialize(&stats);
  JavascriptRewriteConfig config(&stats);
  GoogleMessageHandler handler;
  const GoogleString original = "  <!--  \nvar x = 1;\n  //-->  ";
  const GoogleString expected = "var x=1;";
  JavascriptCodeBlock block(original, &config, "Test", &handler);
  EXPECT_TRUE(block.ProfitableToRewrite());
  EXPECT_EQ(expected, block.Rewritten());
  ExpectStats(stats, 1, 1, 0, original.size() - expected.size());
}

}  // namespace

}  // namespace net_instaweb
