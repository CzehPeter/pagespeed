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

// Author: mdsteele@google.com (Matthew D. Steele)

#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/public/resource_manager_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/string.h"

namespace net_instaweb {

namespace {

class JsInlineFilterTest : public ResourceManagerTestBase,
                           public ::testing::WithParamInterface<bool> {
 public:
  JsInlineFilterTest() : filters_added_(false) {}

 protected:
  virtual void SetUp() {
    ResourceManagerTestBase::SetUp();
    SetAsynchronousRewrites(GetParam());
  }

  void TestInlineJavascript(const GoogleString& html_url,
                            const GoogleString& js_url,
                            const GoogleString& js_original_inline_body,
                            const GoogleString& js_outline_body,
                            bool expect_inline) {
    TestInlineJavascriptGeneral(
        html_url,
        "",  // don't use a doctype for these tests
        js_url,
        js_original_inline_body,
        js_outline_body,
        js_outline_body,  // expect ouline body to be inlined verbatim
        expect_inline);
  }

  void TestInlineJavascriptXhtml(const GoogleString& html_url,
                                 const GoogleString& js_url,
                                 const GoogleString& js_outline_body,
                                 bool expect_inline) {
    TestInlineJavascriptGeneral(
        html_url,
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
        "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">",
        js_url,
        "",  // use an empty original inline body for these tests
        js_outline_body,
        // Expect outline body to get surrounded by a CDATA block:
        "//<![CDATA[\n" + js_outline_body + "\n//]]>",
        expect_inline);
  }

  void TestInlineJavascriptGeneral(const GoogleString& html_url,
                                   const GoogleString& doctype,
                                   const GoogleString& js_url,
                                   const GoogleString& js_original_inline_body,
                                   const GoogleString& js_outline_body,
                                   const GoogleString& js_expected_inline_body,
                                   bool expect_inline) {
    if (!filters_added_) {
      AddFilter(RewriteOptions::kInlineJavascript);
      filters_added_ = true;
    }

    // Specify the input and expected output.
    if (!doctype.empty()) {
      SetDoctype(doctype);
    }
    const GoogleString html_input =
        "<head>\n"
        "  <script src=\"" + js_url + "\">" +
          js_original_inline_body + "</script>\n"
        "</head>\n"
        "<body>Hello, world!</body>\n";
    const GoogleString expected_output =
        (!expect_inline ? html_input :
         "<head>\n"
         "  <script>" + js_expected_inline_body + "</script>\n"
         "</head>\n"
         "<body>Hello, world!</body>\n");

    // Put original Javascript file into our fetcher.
    ResponseHeaders default_js_header;
    SetDefaultLongCacheHeaders(&kContentTypeJavascript, &default_js_header);
    SetFetchResponse(js_url, default_js_header, js_outline_body);

    // Rewrite the HTML page.
    ValidateExpectedUrl(html_url, html_input, expected_output);
  }

 private:
  bool filters_added_;
};

TEST_P(JsInlineFilterTest, DoInlineJavascriptSimple) {
  // Simple case:
  TestInlineJavascript("http://www.example.com/index.html",
                       "http://www.example.com/script.js",
                       "",
                       "function id(x) { return x; }\n",
                       true);
}

TEST_P(JsInlineFilterTest, DoInlineJavascriptWhitespace) {
  // Whitespace between <script> and </script>:
  TestInlineJavascript("http://www.example.com/index2.html",
                       "http://www.example.com/script2.js",
                       "\n    \n  ",
                       "function id(x) { return x; }\n",
                       true);
}

TEST_P(JsInlineFilterTest, DoNotInlineJavascriptDifferentDomain) {
  // Different domains:
  TestInlineJavascript("http://www.example.net/index.html",
                       "http://scripts.example.org/script.js",
                       "",
                       "function id(x) { return x; }\n",
                       false);
}

TEST_P(JsInlineFilterTest, DoNotInlineJavascriptInlineContents) {
  // Inline contents:
  TestInlineJavascript("http://www.example.com/index.html",
                       "http://www.example.com/script.js",
                       "{\"json\": true}",
                       "function id(x) { return x; }\n",
                       false);
}

TEST_P(JsInlineFilterTest, DoNotInlineJavascriptTooBig) {
  // Javascript too long:
  const int64 length = 2 * RewriteOptions::kDefaultJsInlineMaxBytes;
  TestInlineJavascript("http://www.example.com/index.html",
                       "http://www.example.com/script.js",
                       "",
                       ("function longstr() { return '" +
                        GoogleString(length, 'z') + "'; }\n"),
                       false);
}

TEST_P(JsInlineFilterTest, DoNotInlineJavascriptWithCloseTag) {
  // External script contains "</script>":
  TestInlineJavascript("http://www.example.com/index.html",
                       "http://www.example.com/script.js",
                       "",
                       "function close() { return '</script>'; }\n",
                       false);
}

TEST_P(JsInlineFilterTest, DoInlineJavascriptXhtml) {
  // Simple case:
  TestInlineJavascriptXhtml("http://www.example.com/index.html",
                            "http://www.example.com/script.js",
                            "function id(x) { return x; }\n",
                            true);
}

TEST_P(JsInlineFilterTest, DoNotInlineJavascriptXhtmlWithCdataEnd) {
  // External script contains "]]>":
  TestInlineJavascriptXhtml("http://www.example.com/index.html",
                            "http://www.example.com/script.js",
                            "function end(x) { return ']]>'; }\n",
                            false);
}

TEST_P(JsInlineFilterTest, CachedRewrite) {
  // Make sure we work fine when result is cached.
  const char kPageUrl[] = "http://www.example.com/index.html";
  const char kJsUrl[] = "http://www.example.com/script.js";
  const char kJs[] = "function id(x) { return x; }\n";
  const char kNothingInsideScript[] = "";
  TestInlineJavascript(kPageUrl, kJsUrl, kNothingInsideScript, kJs, true);
  TestInlineJavascript(kPageUrl, kJsUrl, kNothingInsideScript, kJs, true);
}

TEST_P(JsInlineFilterTest, InlineJs404) {
  // Test to make sure that a missing input is handled well.
  SetFetchResponse404("404.js");
  AddFilter(RewriteOptions::kInlineJavascript);
  ValidateNoChanges("404", "<script src='404.js'></script>");

  // Second time, to make sure caching doesn't break it.
  ValidateNoChanges("404", "<script src='404.js'></script>");
}

// We test with asynchronous_rewrites() == GetParam() as both true and false.
INSTANTIATE_TEST_CASE_P(JsInlineFilterTestInstance,
                        JsInlineFilterTest,
                        ::testing::Bool());

}  // namespace

}  // namespace net_instaweb
