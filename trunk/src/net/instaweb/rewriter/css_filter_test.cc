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

// Author: jmarantz@google.com (Joshua Marantz)
//     and sligocki@google.com (Shawn Ligocki)

#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/rewriter/public/css_rewrite_test_base.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

namespace {

const char kInputStyle[] =
    ".background_blue { background-color: #f00; }\n"
    ".foreground_yellow { color: yellow; }\n";
const char kOutputStyle[] =
    ".background_blue{background-color:red}"
    ".foreground_yellow{color:#ff0}";

class CssFilterTest : public CssRewriteTestBase {
};

TEST_P(CssFilterTest, SimpleRewriteCssTest) {
  ValidateRewrite("rewrite_css", kInputStyle, kOutputStyle);
}

TEST_P(CssFilterTest, RewriteCss404) {
  // Test to make sure that a missing input is handled well.
  SetFetchResponse404("404.css");
  ValidateNoChanges("404", "<link rel=stylesheet href='404.css'>");

  // Second time, to make sure caching doesn't break it.
  ValidateNoChanges("404", "<link rel=stylesheet href='404.css'>");
}

TEST_P(CssFilterTest, LinkHrefCaseInsensitive) {
  // Make sure we check rel value case insensitively.
  // http://code.google.com/p/modpagespeed/issues/detail?id=354
  InitResponseHeaders("a.css", kContentTypeCss, kInputStyle, 100);
  ValidateExpected(
      "case_insensitive", "<link rel=StyleSheet href=a.css>",
      StrCat("<link rel=StyleSheet href=",
             ExpectedUrlForCss("a", kOutputStyle),
             ">"));
}

TEST_P(CssFilterTest, UrlTooLong) {
  // Make the filename maximum size, so we cannot rewrite it.
  // -4 because .css will be appended.
  GoogleString filename(options()->max_url_segment_size() - 4, 'z');
  // If filename wasn't too long, this would be rewritten (like in
  // SimpleRewriteCssTest).
  ValidateRewriteExternalCss(filename, kInputStyle, kInputStyle,
                             kExpectNoChange | kExpectSuccess);
}

// Make sure we can deal with 0 character nodes between open and close of style.
TEST_P(CssFilterTest, RewriteEmptyCssTest) {
  ValidateRewriteInlineCss("rewrite_empty_css-inline", "", "",
                           kExpectChange | kExpectSuccess | kNoStatCheck);
  // Note: We must check stats ourselves because, for technical reasons,
  // empty inline styles are not treated as being rewritten at all.
  // EXPECT_EQ(0, num_files_minified_->Get());
  EXPECT_EQ(0, minified_bytes_saved_->Get());
  EXPECT_EQ(0, num_parse_failures_->Get());

  ValidateRewriteExternalCss("rewrite_empty_css-external", "", "",
                             kExpectChange | kExpectSuccess | kNoStatCheck);
  EXPECT_EQ(0, minified_bytes_saved_->Get());
  EXPECT_EQ(0, num_parse_failures_->Get());
}

// Make sure we do not recompute external CSS when re-processing an already
// handled page.
TEST_P(CssFilterTest, RewriteRepeated) {
  ValidateRewriteExternalCss("rep", " div { } ", "div{}",
                             kExpectChange | kExpectSuccess);
  int inserts_before = lru_cache()->num_inserts();
  EXPECT_EQ(2, num_files_minified_->Get());  // for factory_ and new_factory.
  num_files_minified_->Set(0);
  ValidateRewriteExternalCss("rep", " div { } ", "div{}",
                             kExpectChange | kExpectSuccess | kNoStatCheck);
  int inserts_after = lru_cache()->num_inserts();
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());
  EXPECT_EQ(inserts_before, inserts_after);

  // We expect num_files_minified_ to be reset to 0 by
  // ValidateRewriteExternalCss and left there since we should not
  // re-minimize.  But because we don't share lru-cache between
  // factories, and ServeResourceFromNewContext uses a fresh factory
  // each call, we will minify once more.
  EXPECT_EQ(1, num_files_minified_->Get());
}

// Make sure we do not reparse external CSS when we know it already has
// a parse error.
TEST_P(CssFilterTest, RewriteRepeatedParseError) {
  const char kInvalidCss[] = "@media }}";
  // Note: It is important that these both have the same id so that the
  // generated CSS file names are identical.
  // TODO(sligocki): This is sort of annoying for error reporting which
  // is suposed to use id to uniquely distinguish which test was running.
  ValidateRewriteExternalCss("rep_fail", kInvalidCss, "",
                             kExpectNoChange | kExpectFailure);
  // First time, we fail to parse.
  EXPECT_EQ(1, num_parse_failures_->Get());
  ValidateRewriteExternalCss("rep_fail", kInvalidCss, "",
                             kExpectNoChange | kExpectFailure | kNoStatCheck);
  // Second time, we remember failure and so don't try to reparse.
  EXPECT_EQ(0, num_parse_failures_->Get());
}

// Make sure we don't change CSS with errors. Note: We can move these tests
// to expected rewrites if we find safe ways to edit them.
TEST_P(CssFilterTest, NoRewriteParseError) {
  ValidateFailParse("non_unicode_charset",
                    "a { font-family: \"\xCB\xCE\xCC\xE5\"; }");
  // From http://www.baidu.com/
  ValidateFailParse("non_unicode_baidu",
                    "#lk span {font:14px \"\xCB\xCE\xCC\xE5\"}");
}

// Make sure bad requests do not corrupt our extension.
TEST_P(CssFilterTest, NoExtensionCorruption) {
  TestCorruptUrl("%22", false);
}

TEST_P(CssFilterTest, NoQueryCorruption) {
  TestCorruptUrl("?query", true);
}

TEST_P(CssFilterTest, RewriteVariousCss) {
  // TODO(sligocki): Get these tests to pass with setlocale.
  // EXPECT_TRUE(setlocale(LC_ALL, "tr_TR.utf8"));
  // Distilled examples.
  const char* good_examples[] = {
    "a.b #c.d e#d,f:g>h+i>j{color:red}",  // .#,>+: in selectors
    "a{border:solid 1px #ccc}",  // Multiple values declaration
    "a{border:none!important}",  // !important
    "a{background-image:url(foo.png)}",  // url
    "a{background-position:-19px 60%}",  // negative position
    "a{margin:0}",  // 0 w/ no units
    "a{padding:0.01em 0.25em}",  // fractions and em
    "a{-moz-border-radius-topleft:0}",  // Browser-specific (-moz)
    ".ds{display:-moz-inline-box}",
    "a{background:none}",  // CSS Parser used to expand this.
    // http://code.google.com/p/modpagespeed/issues/detail?id=5
    "a{font-family:trebuchet ms}",  // Keep space between trebuchet and ms.
    // http://code.google.com/p/modpagespeed/issues/detail?id=121
    "a{color:inherit}",
    // Added for code coverage.
    // TODO(sligocki): Get rid of the space at end?
    // ";" may be needed for some browsers.
    "@import url(http://www.example.com) ;",
    "@media a,b{a{color:red}}",
    "@charset \"foobar\";",
    "a{content:\"Odd chars: \\(\\)\\,\\\"\\\'\"}",
    "img{clip:rect(0px,60px,200px,0px)}",
    // CSS3-style pseudo-elements.
    "p.normal::selection{background:#c00;color:#fff}",
    "::-moz-focus-inner{border:0}",
    "input::-webkit-input-placeholder{color:#ababab}"
    // http://code.google.com/p/modpagespeed/issues/detail?id=51
    "a{box-shadow:-1px -2px 2px rgba(0,0,0,0.15)}",  // CSS3 rgba
    // http://code.google.com/p/modpagespeed/issues/detail?id=66
    "a{-moz-transform:rotate(7deg)}",
    // Microsoft syntax values.
    "a{filter:progid:DXImageTransform.Microsoft.Alpha(Opacity=80)}",
    // Make sure we keep "\," distinguished from ",".
    "body{font-family:font\\,1,font\\,2}",
    // Found in the wild:
    "a{width:overflow:hidden}",
    // IE hack: \9
    "div{margin:100px\\9 }",
    "div{margin\\9 :100px}",
    "div\\9 {margin:100px}",
    "a{color:red\\9 }",
    "a{background:none\\9 }",

    // Recovered parse errors:
    // Slashes in value list.
    ".border8{border-radius: 36px / 12px }",

    // http://code.google.com/p/modpagespeed/issues/detail?id=220
    // See https://developer.mozilla.org/en/CSS/-moz-transition-property
    // and http://www.webkit.org/blog/138/css-animation/
    "a{-webkit-transition-property:opacity,-webkit-transform }",

    // Parameterized pseudo-selector.
    "div:nth-child(1n) {color:red}",

    // IE8 Hack \0/
    // See http://dimox.net/personal-css-hacks-for-ie6-ie7-ie8/
    "a{color: red\\0/ ;background-color:green}",

    "a{font:bold verdana 10px }",
    "a{foo: +bar }",
    "a{color: rgb(foo,+,) }",

    // Things from Alexa-100 that we get parsing errors for. Most are illegal
    // syntax/typos. Some are CSS3 constructs.

    // kDeclarationError from Alexa-100
    // Comma in values
    "a{webkit-transition-property: color, background-color }",
    // Special chars in property
    "a{//display: inline-block }",
    ".ad_300x250{/margin-top:-120px }",
    // Properties with no value
    "a{background-repeat;no-repeat }",
    // Typos
    "a{margin-right:0;width:113px;*/ }",
    "a{z-i ndex:19 }",
    "a{width:352px;height62px ;display:block}",
    "a{color: #5552 }",
    "a{1font-family:Tahoma, Arial, sans-serif }",
    "a{text align:center }",

    // kSelectorError from Alexa-100
    // Selector list ends in comma
    ".hp .col ul, {display:inline}",
    // Parameters for pseudoclass
    "body:not(:target) {color:red}",
    "a:not(.button):hover {color:red}",
    // Typos
    "# new_results_notification{font-size:12px}",
    ".bold: {font-weight:bold}",

    // kFunctionError from Alexa-100
    // Expression
    "a{_top: expression(0+((e=document.documen))) }",
    "a{width: expression(this.width > 120 ? 120:tr) }",
    // Equals in function
    "a{progid:DXImageTransform.Microsoft.AlphaImageLoader"
    "(src=/images/lb/internet_e) }",
    "a{progid:DXImageTransform.Microsoft.AlphaImageLoader"
    "(src=\"/images/lb/internet_e)\" }",
    "a{progid:DXImageTransform.Microsoft.AlphaImageLoader"
    "(src='/images/lb/internet_e)' }",
  };

  for (int i = 0; i < arraysize(good_examples); ++i) {
    GoogleString id = StringPrintf("distilled_css_good%d", i);
    ValidateRewrite(id, good_examples[i], good_examples[i]);
  }

  const char* fail_examples[] = {
    // CSS3 media "and (max-width: 290px).
    // http://code.google.com/p/modpagespeed/issues/detail?id=50
    "@media screen and (max-width: 290px) { a { color:red } }",

    // Malformed @import statements.
    "@import styles.css; a { color: red; }",
    "@import \"styles.css\", \"other.css\"; a { color: red; }",
    "@import url(styles.css), url(other.css); a { color: red; }",
    "@import \"styles.css\"...; a { color: red; }",

    // Unexpected @-statements
    "@keyframes wiggle { 0% { transform: rotate(6deg); } }",
    "@font-face { font-family: 'Ubuntu'; font-style: normal }",

    // Things from Alexa-100 that we get parsing errors for. Most are illegal
    // syntax/typos. Some are CSS3 constructs.

    // kSelectorError from Alexa-100
    // Typos
    // Note: These fail because of the if (Done()) return NULL call in
    // ParseRuleset
    "a { color: red }\n */",
    "a { color: red }\n // Comment",
    "a { color: red } .foo",

    // Should fail (bad syntax):
    "}}",
    "a { color: red; }}}",
  };

  for (int i = 0; i < arraysize(fail_examples); ++i) {
    GoogleString id = StringPrintf("distilled_css_fail%d", i);
    ValidateFailParse(id, fail_examples[i]);
  }
}

// Things we could be optimizing.
// This test will fail when we start optimizing these thing.
TEST_P(CssFilterTest, ToOptimize) {
  const char* examples[][2] = {
    // Noticed from YUI minification.
    { "td { line-height: 0.8em; }",
      // Could be: "td{line-height:.8em}"
      "td{line-height:0.8em}", },
    { ".gb1, .gb3 {}",
      // Could be: ""
      ".gb1,.gb3{}", },
    { ".lst:focus { outline:none; }",
      // Could be: ".lst:focus{outline:0}"
      ".lst:focus{outline:none}", },
  };

  for (int i = 0; i < arraysize(examples); ++i) {
    GoogleString id = StringPrintf("to_optimize_%d", i);
    ValidateRewrite(id, examples[i][0], examples[i][1]);
  }
}

// Test more complicated CSS.
TEST_P(CssFilterTest, ComplexCssTest) {
  // Real-world examples. Picked out of Wikipedia's CSS.
  const char* examples[][2] = {
    { "#userlogin, #userloginForm {\n"
      "  border: solid 1px #cccccc;\n"
      "  padding: 1.2em;\n"
      "  float: left;\n"
      "}\n",

      "#userlogin,#userloginForm{border:solid 1px #ccc;padding:1.2em;"
      "float:left}"},

    { "h3 .editsection { font-size: 76%; font-weight: normal; }\n",
      "h3 .editsection{font-size:76%;font-weight:normal}"},

    { "div.magnify a, div.magnify img {\n"
      "  display: block;\n"
      "  border: none !important;\n"
      "  background: none !important;\n"
      "}\n",

      "div.magnify a,div.magnify img{display:block;border:none!important;"
      "background:none!important}"},

    { "#ca-watch.icon a:hover {\n"
      "  background-image: url('images/watch-icons.png?1');\n"
      "  background-position: -19px 60%;\n"
      "}\n",

      "#ca-watch.icon a:hover{background-image:url(images/watch-icons.png?1);"
      "background-position:-19px 60%}"},

    { "body {\n"
      "  background: White;\n"
      "  /*font-size: 11pt !important;*/\n"
      "  color: Black;\n"
      "  margin: 0;\n"
      "  padding: 0;\n"
      "}\n",

      "body{background:#fff;color:#000;margin:0;padding:0}"},

    { ".suggestions-result{\n"
      "  color:black;\n"
      "  color:WindowText;\n"
      "  padding:0.01em 0.25em;\n"
      "}\n",

      // TODO(sligocki): Do we care about color:WindowText?
      //".suggestions-result{color:#000;color:WindowText;padding:0.01em 0.25em}"

      ".suggestions-result{color:#000;color:#000;padding:0.01em 0.25em}"},

    { ".ui-corner-tl { -moz-border-radius-topleft: 0; -webkit-border-top-left"
      "-radius: 0; }\n",

      ".ui-corner-tl{-moz-border-radius-topleft:0;-webkit-border-top-left"
      "-radius:0}"},

    { ".ui-tabs .ui-tabs-nav li.ui-tabs-selected a, .ui-tabs .ui-tabs-nav li."
      "ui-state-disabled a, .ui-tabs .ui-tabs-nav li.ui-state-processing a { "
      "cursor: pointer; }\n",

      ".ui-tabs .ui-tabs-nav li.ui-tabs-selected a,.ui-tabs .ui-tabs-nav "
      "li.ui-state-disabled a,.ui-tabs .ui-tabs-nav li.ui-state-processing a{"
      "cursor:pointer}"},

    { ".ui-datepicker-cover {\n"
      "  display: none; /*sorry for IE5*/\n"
      "  display/**/: block; /*sorry for IE5*/\n"
      "  position: absolute; /*must have*/\n"
      "  z-index: -1; /*must have*/\n"
      "  filter: mask(); /*must have*/\n"
      "  top: -4px; /*must have*/\n"
      "  left: -4px; /*must have*/\n"
      "  width: 200px; /*must have*/\n"
      "  height: 200px; /*must have*/\n"
      "}\n",

      // TODO(sligocki): Should we preserve the dispaly/**/:?
      //".ui-datepicker-cover{display:none;display/**/:block;position:absolute;"
      //"z-index:-1;filter:mask();top:-4px;left:-4px;width:200px;height:200px}"

      ".ui-datepicker-cover{display:none;display:block;position:absolute;"
      "z-index:-1;filter:mask();top:-4px;left:-4px;width:200px;height:200px}" },

    { ".shift {\n"
      "  -moz-transform: rotate(7deg);\n"
      "  -webkit-transform: rotate(7deg);\n"
      "  -moz-transform: skew(-25deg);\n"
      "  -webkit-transform: skew(-25deg);\n"
      "  -moz-transform: scale(0.5);\n"
      "  -webkit-transform: scale(0.5);\n"
      "  -moz-transform: translate(3em, 0);\n"
      "  -webkit-transform: translate(3em, 0);\n"
      "}\n",

      ".shift{-moz-transform:rotate(7deg);-webkit-transform:rotate(7deg);"
      "-moz-transform:skew(-25deg);-webkit-transform:skew(-25deg);"
      "-moz-transform:scale(0.5);-webkit-transform:scale(0.5);"
      "-moz-transform:translate(3em,0);-webkit-transform:translate(3em,0)}" },

    // http://code.google.com/p/modpagespeed/issues/detail?id=121
    { "body { font: 2em sans-serif; }", "body{font:2em sans-serif}" },
    { "body { font: 0.75em sans-serif; }", "body{font:0.75em sans-serif}" },

    // http://code.google.com/p/modpagespeed/issues/detail?id=128
    { "#breadcrumbs ul { list-style-type: none; }",
      "#breadcrumbs ul{list-style-type:none}" },

    // http://code.google.com/p/modpagespeed/issues/detail?id=126
    // Extra spaces assure that we actually rewrite the first arg even if
    // font: is expanded by parser.
    { ".menu { font: menu; }               ", ".menu{font:menu}" },

    // http://code.google.com/p/modpagespeed/issues/detail?id=211
    { "#some_id {\n"
      "background: #cccccc url(images/picture.png) 50% 50% repeat-x;\n"
      "}\n",

      "#some_id{background:#ccc url(images/picture.png) 50% 50% repeat-x}" },

    { ".gac_od { border-color: -moz-use-text-color #E7E7E7 #E7E7E7 "
      "-moz-use-text-color; }",

      ".gac_od{border-color:-moz-use-text-color #e7e7e7 #e7e7e7 "
      "-moz-use-text-color}" },

    // Star/Underscore hack
    // See: http://developer.yahoo.com/yui/compressor/css.html
    { "a { *padding-bottom: 0px; }",
      "a{*padding-bottom:0px}" },

    { "#element { width: 1px; _width: 3px; }",
      "#element{width:1px;_width:3px}" },

    // Complex nested functions
    { "body {\n"
      "  background-image:-webkit-gradient(linear, 50% 0%, 50% 100%,"
      " from(rgb(232, 237, 240)), to(rgb(252, 252, 253)));\n"
      "  color: red;\n"
      "}\n"
      ".foo { color: rgba(1, 2, 3, 0.4); }\n",

      "body{background-image:-webkit-gradient(linear,50% 0%,50% 100%,"
      "from(#e8edf0),to(#fcfcfd));color:red}.foo{color:rgba(1,2,3,0.4)}" },

    // Counters
    // http://www.w3schools.com/CSS/tryit.asp?filename=trycss_gen_counter-reset
    { "body {counter-reset:section;}\n"
      "h1 {counter-reset:subsection;}\n"
      "h1:before\n"
      "{\n"
      "counter-increment:section;\n"
      "content:\"Section \" counter(section) \". \";\n"
      "}\n"
      "h2:before \n"
      "{\n"
      "counter-increment:subsection;\n"
      "content:counter(section) \".\" counter(subsection) \" \";\n"
      "}\n",

      "body{counter-reset:section}"
      "h1{counter-reset:subsection}"
      "h1:before{counter-increment:section;"
      "content:\"Section \" counter(section) \". \"}"
      "h2:before{counter-increment:subsection;"
      "content:counter(section) \".\" counter(subsection) \" \"}" },

    // Don't lowercase font names.
    { "a { font-family: Arial; }",
      "a{font-family:Arial}" },

    // Don't drop precision on large integers (this is 2^31 + 1 which is
    // just larger than larges z-index accepted by chrome, 2^31 - 1).
    { "#foo { z-index: 2147483649; }",
      // Not "#foo{z-index:2.14748e+09}"
      "#foo{z-index:2147483649}" },

    { "#foo { z-index: 123456789012345678901234567890; }",
      // TODO(sligocki): "#foo{z-index:12345678901234567890}" },
      "#foo{z-index:1.234567890123457e+29}" },

    // Parse and serialize "\n" correctly as "n" and "\A " correctly as newline.
    { "a { content: \"Special chars: \\n\\r\\t\\A \\D \\9\" }",
      "a{content:\"Special chars: nrt\\A \\D \\9 \"}" },

    // Test some interesting combinations of @media.
    {
      "@media screen {"
      "  body { counter-reset:section }"
      "  h1 { counter-reset:subsection }"
      "}"
      "@media screen,printer { a { color:red } }"
      "@media screen,printer { b { color:green } }"
      "@media screen,printer { c { color:blue } }"
      "@media screen         { d { color:black } }"
      "@media screen,printer { e { color:white } }",

      "@media screen{"
      "body{counter-reset:section}"
      "h1{counter-reset:subsection}"
      "}"
      "@media screen,printer{"
      "a{color:red}"
      "b{color:green}"
      "c{color:#00f}"
      "}"
      "@media screen{d{color:#000}}"
      "@media screen,printer{e{color:#fff}}",
    },

    // Charsets
    { "@charset \"UTF-8\";\n"
      "a { color: red }\n",

      "@charset \"UTF-8\";a{color:red}" },

    // Recovered parse errors:
    // http://code.google.com/p/modpagespeed/issues/detail?id=220
    { ".mui-navbar-wrap, .mui-navbar-clone {"
      "opacity:1;-webkit-transform:translateX(0);"
      "-webkit-transition-property:opacity,-webkit-transform;"
      "-webkit-transition-duration:400ms;}",

      ".mui-navbar-wrap,.mui-navbar-clone{"
      "opacity:1;-webkit-transform:translateX(0);"
      "-webkit-transition-property:opacity,-webkit-transform;"
      "-webkit-transition-duration:400ms}" },

    // IE 8 hack \0/.
    { ".gbxms{background-color:#ccc;display:block;position:absolute;"
      "z-index:1;top:-1px;left:-2px;right:-2px;bottom:-2px;opacity:.4;"
      "-moz-border-radius:3px;"
      "filter:progid:DXImageTransform.Microsoft.Blur(pixelradius=5);"
      "*opacity:1;*top:-2px;*left:-5px;*right:5px;*bottom:4px;"
      "-ms-filter:\"progid:DXImageTransform.Microsoft.Blur(pixelradius=5)\";"
      "opacity:1\\0/;top:-4px\\0/;left:-6px\\0/;right:5px\\0/;bottom:4px\\0/}",

      ".gbxms{background-color:#ccc;display:block;position:absolute;"
      "z-index:1;top:-1px;left:-2px;right:-2px;bottom:-2px;opacity:0.4;"
      "-moz-border-radius:3px;"
      "filter:progid:DXImageTransform.Microsoft.Blur(pixelradius=5);"
      "*opacity:1;*top:-2px;*left:-5px;*right:5px;*bottom:4px;-ms-filter:"
      "\"progid:DXImageTransform.Microsoft.Blur\\(pixelradius=5\\)\";"
      "opacity:1\\0/;top:-4px\\0/;left:-6px\\0/;right:5px\\0/;bottom:4px\\0/}"},

    // Alexa-100 with parse errors (illegal syntax or CSS3).
    // Comma in values
    { ".cnn_html_slideshow_controls > .cnn_html_slideshow_pager_container >"
      " .cnn_html_slideshow_pager > li\n"
      "{\n"
      "  font-size: 16px;\n"
      "  -webkit-transition-property: color, background-color;\n"
      "  -webkit-transition-duration: 0.5s;\n"
      "}\n",

      ".cnn_html_slideshow_controls>.cnn_html_slideshow_pager_container>"
      ".cnn_html_slideshow_pager>li{"
      "font-size:16px;-webkit-transition-property: color, background-color;"
      "-webkit-transition-duration:0.5s}" },

    { "a.login,a.home{position:absolute;right:15px;top:15px;display:block;"
      "float:right;height:29px;line-height:27px;font-size:15px;"
      "font-weight:bold;color:rgba(255,255,255,0.7)!important;color:#fff;"
      "text-shadow:0 -1px 0 rgba(0,0,0,0.2);background:#607890;padding:0 12px;"
      "opacity:.9;text-decoration:none;border:1px solid #2e4459;"
      "-moz-border-radius:6px;-webkit-border-radius:6px;border-radius:6px;"
      "-moz-box-shadow:0 1px 0 rgba(255,255,255,0.15),0 1px 0"
      " rgba(255,255,255,0.15) inset;-webkit-box-shadow:0 1px 0 "
      "rgba(255,255,255,0.15),0 1px 0 rgba(255,255,255,0.15) inset;"
      "box-shadow:0 1px 0 rgba(255,255,255,0.15),0 1px 0 "
      "rgba(255,255,255,0.15) inset}",

      "a.login,a.home{position:absolute;right:15px;top:15px;display:block;"
      "float:right;height:29px;line-height:27px;font-size:15px;"
      "font-weight:bold;color:rgba(255,255,255,0.7)!important;color:#fff;"
      "text-shadow:0 -1px 0 rgba(0,0,0,0.2);background:#607890;padding:0 12px;"
      "opacity:0.9;text-decoration:none;border:1px solid #2e4459;"
      "-moz-border-radius:6px;-webkit-border-radius:6px;border-radius:6px;"
      "-moz-box-shadow:0 1px 0 rgba(255,255,255,0.15),0 1px 0"
      " rgba(255,255,255,0.15) inset;-webkit-box-shadow:0 1px 0 "
      "rgba(255,255,255,0.15),0 1px 0 rgba(255,255,255,0.15) inset;"
      "box-shadow:0 1px 0 rgba(255,255,255,0.15),0 1px 0 "
      "rgba(255,255,255,0.15) inset}" },

    // Special chars in property
    { ".authorization .mail .login input, .authorization .pswd input {"
      "float: left; width: 100%; font-size: 75%; -moz-box-sizing: border-box; "
      "-webkit-box-sizing: border-box; box-sizing: border-box; height: 21px; "
      "padding: 2px; #height: 13px}\n"
      ".authorization .mail .domain select {float: right; width: 97%; "
      "#width: 88%; font-size: 75%; height: 21px; -moz-box-sizing: border-box; "
      "-webkit-box-sizing: border-box; box-sizing: border-box}\n"
      ".weather_review .main img.attention {position: absolute; z-index: 5; "
      "left: -10px; top: 6px; width: 29px; height: 26px; \n"
      "background: url('http://limg3.imgsmail.ru/r/weather_new/ico_attention."
      "png'); \n"
      "//background-image: none; \n"
      "filter: progid:DXImageTransform.Microsoft.AlphaImageLoader("
      "src=\"http://limg3.imgsmail.ru/r/weather_new/ico_attention.png\", "
      "sizingMethod=\"crop\"); \n"
      "} \n"
      ".rb_body {font-size: 12px; padding: 0 0 0 10px; overflow: hidden; "
      "text-align: left; //display: inline-block;}\n"
      ".rb_h4 {border-bottom: 1px solid #0857A6; color: #0857A6; "
      "font-size: 17px; font-weight: bold; text-decoration: none;}\n",

      ".authorization .mail .login input,.authorization .pswd input{"
      "float:left;width:100%;font-size:75%;-moz-box-sizing:border-box;"
      "-webkit-box-sizing:border-box;box-sizing:border-box;height:21px;"
      "padding:2px;#height: 13px}"
      ".authorization .mail .domain select{float:right;width:97%;"
      "#width: 88%;font-size:75%;height:21px;-moz-box-sizing:border-box;"
      "-webkit-box-sizing:border-box;box-sizing:border-box}"
      ".weather_review .main img.attention{position:absolute;z-index:5;"
      "left:-10px;top:6px;width:29px;height:26px;"
      "background:url(http://limg3.imgsmail.ru/r/weather_new/ico_attention."
      "png);"
      "//background-image: none;"
      "filter: progid:DXImageTransform.Microsoft.AlphaImageLoader("
      "src=\"http://limg3.imgsmail.ru/r/weather_new/ico_attention.png\", "
      "sizingMethod=\"crop\")}"
      ".rb_body{font-size:12px;padding:0 0 0 10px;overflow:hidden;"
      "text-align:left;//display: inline-block}"
      ".rb_h4{border-bottom:1px solid #0857a6;color:#0857a6;"
      "font-size:17px;font-weight:bold;text-decoration:none}" },

    // Expression
    { ".file_manager .loading { _position: absolute;_top: expression(0+((e=doc"
      "ument.documentElement.scrollTop)?e:document.body.scrollTop)+'px'); "
      "color: red; }\n"
      ".connect_widget .page_stream img{max-width:120px;"
      "width:expression(this.width > 120 ? 120:true); color: red; }\n",

      ".file_manager .loading{_position:absolute;_top: expression(0+((e=doc"
      "ument.documentElement.scrollTop)?e:document.body.scrollTop)+'px');"
      "color:red}"
      ".connect_widget .page_stream img{max-width:120px;"
      "width:expression(this.width > 120 ? 120:true);color:red}" },

    // Equals in function
    { ".imdb_lb .header{width:726px;width=728px;height:12px;padding:1px;"
      "border-bottom:1px #000000 solid;background:#eeeeee;font-size:10px;"
      "text-align:left;}"
      ".cboxIE #cboxTopLeft{background:transparent;filter:progid:"
      "DXImageTransform.Microsoft.AlphaImageLoader(src=/images/lb/"
      "internet_explorer/borderTopLeft.png, sizingMethod='scale');}",

      ".imdb_lb .header{width:726px;width=728px;height:12px;padding:1px;"
      "border-bottom:1px #000 solid;background:#eee;font-size:10px;"
      "text-align:left}"
      ".cboxIE #cboxTopLeft{background:transparent;filter:progid:"
      "DXImageTransform.Microsoft.AlphaImageLoader(src=/images/lb/"
      "internet_explorer/borderTopLeft.png, sizingMethod='scale')}" },

    // Special chars in values
    { ".login-form .input-text{ width:144px;padding:6px 3px; "
      "background-color:#fff;background-position:0 -170px;"
      "background-repeat;no-repeat}"
      "td.pop_content .dialog_body{padding:10px;border-bottom:1px# solid #ccc}",

      ".login-form .input-text{width:144px;padding:6px 3px;"
      "background-color:#fff;background-position:0 -170px;"
      "background-repeat;no-repeat}"
      "td.pop_content .dialog_body{padding:10px;border-bottom:1px# solid #ccc}"
    },

    // kSelectorError from Alexa-100
    // Selector list ends in comma
    { ".hp .col ul, {\n"
      "  display: inline !important;\n"
      "  zoom: 1;\n"
      "  vertical-align: top;\n"
      "  margin-left: -10px;\n"
      "  position: relative;\n"
      "}\n",

      ".hp .col ul, {display:inline!important;zoom:1;vertical-align:top;"
      "margin-left:-10px;position:relative}" },

    // Invalid comment type ("//").
    { ".ciuNoteEditBox .topLeft\n"
      "{\n"
      "        background-position:left top;\n"
      "\tbackground-repeat:no-repeat;\n"
      "\tfont-size:4px;\n"
      "\t\n"
      "\t\n"
      "\tpadding: 0px 0px 0px 1px; \n"
      "\t\n"
      "\twidth:7px;\n"
      "}\n"
      "\n"
      "// css hack to make font-size 0px in only ff2.0 and older "
      "(http://pornel.net/firefoxhack)\n"
      ".ciuNoteBox .topLeft,\n"
      ".ciuNoteEditBox .topLeft, x:-moz-any-link {\n"
      "\tfont-size: 0px;\n"
      "}\n",

      ".ciuNoteEditBox .topLeft{background-position:left top;"
      "background-repeat:no-repeat;font-size:4px;padding:0px 0px 0px 1px;"
      "width:7px}// css hack to make font-size 0px in only ff2.0 and older "
      "(http://pornel.net/firefoxhack)\n"
      ".ciuNoteBox .topLeft,\n"
      ".ciuNoteEditBox .topLeft, x:-moz-any-link {font-size:0px}" },

    // Parameters for pseudoclass
    { "/* Opera（＋Firefox、Safari） */\n"
      "body:not(:target) .sh_heading_main_b, body:not(:target) "
      ".sh_heading_main_b_wide{\n"
      "  background:url(\"data:image/png;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAAoCAYAAAA/tpB3AAAAQ0lEQVR42k3EMQLAIAg"
      "EMP//WkRQVMB2YLgMae/XMhOLCMzdq3svds7B9t6VmWFrLWzOWakqJiLYGKNiZqz3jh"
      "HR+wBZbpvd95zR6QAAAABJRU5ErkJggg==\") repeat-x left top;\n"
      "}\n"
      "/* Firefox（＋Google Chrome2） */\n"
      "html:not([lang*=""]) .sh_heading_main_b,\n"
      "html:not([lang*=""]) .sh_heading_main_b_wide{\n"
      "\t/* For Mozilla/Gecko (Firefox etc) */\n"
      "\tbackground:-moz-linear-gradient(top, #FFFFFF, #F0F0F0);\n"
      "\t/* For WebKit (Safari, Google Chrome etc) */\n"
      "\tbackground:-webkit-gradient(linear, left top, left bottom, "
      "from(#FFFFFF), to(#F0F0F0));\n"
      "}\n"
      "/* Safari */\n"
      "html:not(:only-child:only-child) .sh_heading_main_b,\n"
      "html:not(:only-child:only-child) .sh_heading_main_b_wide{\n"
      "\t/* For WebKit (Safari, Google Chrome etc) */\n"
      "\tbackground: -webkit-gradient(linear, left top, left bottom, "
      "from(#FFFFFF), to(#F0F0F0));\n"
      "}\n",

      "body:not(:target) .sh_heading_main_b, body:not(:target) "
      ".sh_heading_main_b_wide{background:url(data:image/png;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAAoCAYAAAA/tpB3AAAAQ0lEQVR42k3EMQLAIAg"
      "EMP//WkRQVMB2YLgMae/XMhOLCMzdq3svds7B9t6VmWFrLWzOWakqJiLYGKNiZqz3jh"
      "HR+wBZbpvd95zR6QAAAABJRU5ErkJggg==) repeat-x left top}"
      "html:not([lang*=""]) .sh_heading_main_b,\n"
      "html:not([lang*=""]) .sh_heading_main_b_wide{"
      "background:-moz-linear-gradient(top,#fff,#f0f0f0);"
      "background:-webkit-gradient(linear,left top,left bottom,"
      "from(#fff),to(#f0f0f0))}"
      "html:not(:only-child:only-child) .sh_heading_main_b,\n"
      "html:not(:only-child:only-child) .sh_heading_main_b_wide{"
      "background:-webkit-gradient(linear,left top,left bottom,"
      "from(#fff),to(#f0f0f0))}" },

    // @import stuff
    { "@import \"styles.css\"foo; a { color: red; }",
      "@import url(styles.css) foo;a{color:red}" },

    // @media with no contents
    { "@media; a { color: red; }", "a{color:red}" },
    { "@media screen, print; a { color: red; }", "a{color:red}" },
  };

  for (int i = 0; i < arraysize(examples); ++i) {
    GoogleString id = StringPrintf("complex_css%d", i);
    ValidateRewrite(id, examples[i][0], examples[i][1]);
  }

  const char* parse_fail_examples[] = {
    // Unexpected @-statements
    "@-webkit-keyframes wiggle {\n"
    "  0% {-webkit-transform:rotate(6deg);}\n"
    "  50% {-webkit-transform:rotate(-6deg);}\n"
    "  100% {-webkit-transform:rotate(6deg);}\n"
    "}\n"
    "@-moz-keyframes wiggle {\n"
    "  0% {-moz-transform:rotate(6deg);}\n"
    "  50% {-moz-transform:rotate(-6deg);}\n"
    "  100% {-moz-transform:rotate(6deg);}\n"
    "}\n"
    "@keyframes wiggle {\n"
    "  0% {transform:rotate(6deg);}\n"
    "  50% {transform:rotate(-6deg);}\n"
    "  100% {transform:rotate(6deg);}\n"
    "}\n",

    "@font-face{font-family:'Ubuntu';font-style:normal;font-weight:normal;"
    "src:local('Ubuntu'), url('http://themes.googleusercontent.com/static/"
    "fonts/ubuntu/v2/2Q-AW1e_taO6pHwMXcXW5w.ttf') format('truetype')}"
    "@font-face{font-family:'Ubuntu';font-style:normal;font-weight:bold;"
    "src:local('Ubuntu Bold'), local('Ubuntu-Bold'), url('http://themes."
    "googleusercontent.com/static/fonts/ubuntu/v2/0ihfXUL2emPh0ROJezvraKCWc"
    "ynf_cDxXwCLxiixG1c.ttf') format('truetype')}",

    // Bad syntax
    "}}",
  };

  for (int i = 0; i < arraysize(parse_fail_examples); ++i) {
    GoogleString id = StringPrintf("complex_css_parse_fail%d", i);
    ValidateFailParse(id, parse_fail_examples[i]);
  }
}

// Most tests are run with set_always_rewrite_css(true),
// but all production use has set_always_rewrite_css(false).
// This test makes sure that setting to false still does what we intend.
TEST_P(CssFilterTest, NoAlwaysRewriteCss) {
  // When we force always_rewrite_css, we can expand some statements.
  // Note: when this example is fixed in the minifier, this test will break :/
  options()->ClearSignatureForTesting();
  options()->set_always_rewrite_css(true);
  resource_manager()->ComputeSignature(options());
  ValidateRewrite("expanding_example",
                  "@import url(http://www.example.com)",
                  "@import url(http://www.example.com) ;");
  // With it set false, we do not expand CSS (as long as we didn't do anything
  // else, like rewrite sub-resources.
  options()->ClearSignatureForTesting();
  options()->set_always_rewrite_css(false);
  resource_manager()->ComputeSignature(options());
  ValidateRewrite("non_expanding_example",
                  "@import url(http://www.example.com)",
                  "@import url(http://www.example.com)",
                  kExpectNoChange | kExpectSuccess);
  // Here: kExpectSuccess means there was no error. (Minification that
  // actually expands the statement is not considered an error.)

  // When we force always_rewrite_css, we allow rewriting something to nothing.
  options()->ClearSignatureForTesting();
  options()->set_always_rewrite_css(true);
  resource_manager()->ComputeSignature(options());
  ValidateRewrite("contracting_example",     "  ", "");
  // With it set false, we do not allow something to be minified to nothing.
  // Note: We may allow this in the future if contents are all whitespace.
  options()->ClearSignatureForTesting();
  options()->set_always_rewrite_css(false);
  resource_manager()->ComputeSignature(options());
  ValidateRewrite("non_contracting_example", "  ", "  ",
                  kExpectNoChange | kExpectFailure);
}

TEST_P(CssFilterTest, NoQuirksModeForXhtml) {
  const char quirky_css[]     = "body {color:DECAFB}";
  const char normalized_css[] = "body{color:#decafb}";
  const char no_quirks_css[]  = "body{color:DECAFB}";

  // By default we parse the CSS with quirks-mode enabled and "fix" the CSS.
  ValidateRewrite("quirks_mode", quirky_css, normalized_css,
                  kExpectChange | kExpectSuccess);

  // But when in XHTML mode, we don't allow CSS quirks.
  SetDoctype("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" "
             "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">");
  ValidateRewrite("no_quirks_mode", quirky_css, no_quirks_css,
                  kExpectChange | kExpectSuccess | kNoOtherContexts);
  // NOTE: We must set kNoOtherContexts, because this change depends upon the
  // rewriter knowing that the original resource was found in an XHTML page
  // which we don't know if we are recieving a Fetch request and don't have
  // the resource. This could cause issues :/
}

// http://code.google.com/p/modpagespeed/issues/detail?id=324
TEST_P(CssFilterTest, RetainExtraHeaders) {
  GoogleString url = StrCat(kTestDomain, "retain.css");
  InitResponseHeaders(url, kContentTypeCss, kInputStyle, 300);
  TestRetainExtraHeaders("retain.css", "cf", "css");
}

TEST_P(CssFilterTest, RewriteStyleAttribute) {
  // Test that nothing happens if rewriting is disabled (default).
  ValidateNoChanges("RewriteStyleAttribute",
                    "<div style='background-color: #f00; color: yellow;'/>");

  options()->ClearSignatureForTesting();
  options()->EnableFilter(RewriteOptions::kRewriteStyleAttributes);
  resource_manager()->ComputeSignature(options());

  // Test no rewriting.
  ValidateNoChanges("no-rewriting",
                    "<div style='background-color:red;color:#ff0'/>");

  // Test successful rewriting.
  ValidateExpected("rewrite-simple",
                   "<div style='background-color: #f00; color: yellow;'/>",
                   "<div style='background-color:red;color:#ff0'/>");

  SetFetchResponse404("404.css");
  static const char kMixedInput[] =
      "<div style=\""
      "  background-image: url('images/watch-icons.png?1');\n"
      "  background-position: -19px 60%;\""
      ">\n"
      "<link rel=stylesheet href='404.css'>\n"
      "<span style=\"font-family: Verdana\">Verdana</span>\n"
      "</div>";
  static const char kMixedOutput[] =
      "<div style=\""
      "background-image:url(images/watch-icons.png?1);"
      "background-position:-19px 60%\""
      ">\n"
      "<link rel=stylesheet href='404.css'>\n"
      "<span style=\"font-family:Verdana\">Verdana</span>\n"
      "</div>";
  ValidateExpected("rewrite-mixed", kMixedInput, kMixedOutput);

  // Test that nothing happens if we have a style attribute on a style element,
  // which is actually invalid.
  ValidateNoChanges("rewrite-style-with-style",
                   "<style style='background-color: #f00; color: yellow;'/>");
}

TEST_P(CssFilterTest, DontAbsolutifyCssImportUrls) {
  // Since we are not using a proxy URL namer (TestUrlNamer) nor any
  // domain rewriting/sharding, we expect the relative URLs in
  // the @import's to be passed though untouched.
  const char styles_filename[] = "styles.css";
  const char styles_css[] =
      ".background_red{background-color:red}"
      ".foreground_yellow{color:#ff0}";
  const GoogleString css_in = StrCat(
      "@import url(media/print.css) print;",
      "@import url(media/screen.css) screen;",
      styles_css);
  InitResponseHeaders(styles_filename, kContentTypeCss, css_in, 100);

  static const char html_prefix[] =
      "<head>\n"
      "  <title>Example style outline</title>\n"
      "  <!-- Style starts here -->\n"
      "  <style type='text/css'>";
  static const char html_suffix[] = "</style>\n"
      "  <!-- Style ends here -->\n"
      "</head>";

  GoogleString html = StrCat(html_prefix, css_in,  html_suffix);

  ValidateNoChanges("dont_absolutify_css_import_urls", html);
}

TEST_P(CssFilterTest, DontAbsolutifyEmptyUrl) {
  // Ensure that an empty URL is left as-is and is not absolutified.
  const char kEmptyUrlRule[] = "#gallery { list-style: none outside url(''); }";
  const char kNoUrlRule[] = "#gallery{list-style:none outside url()}";
  ValidateRewrite("empty_url_in_rule", kEmptyUrlRule, kNoUrlRule);

  const char kEmptyUrlImport[] = "@import url('');";
  const char kNoUrlImport[] = "@import url() ;";
  ValidateRewrite("empty_url_in_import", kEmptyUrlImport, kNoUrlImport);
}

// We test with asynchronous_rewrites() == GetParam() as both true and false.
INSTANTIATE_TEST_CASE_P(CssFilterTestInstance,
                        CssFilterTest,
                        ::testing::Bool());

}  // namespace

}  // namespace net_instaweb
