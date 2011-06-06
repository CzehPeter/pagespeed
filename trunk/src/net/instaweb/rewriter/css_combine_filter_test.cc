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

#include <vector>

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/empty_html_filter.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/mock_callback.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/public/css_tag_scanner.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/resource_manager_test_base.h"
#include "net/instaweb/rewriter/public/resource_namer.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mem_file_system.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/stl_util.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/url_multipart_encoder.h"

namespace net_instaweb {

namespace {

const char kDomain[] = "http://combine_css.test/";
const char kYellow[] = ".yellow {background-color: yellow;}";
const char kBlue[] = ".blue {color: blue;}\n";

class CssCombineFilterTest : public ResourceManagerTestBase {
 protected:
  virtual void SetUp() {
    ResourceManagerTestBase::SetUp();
    AddFilter(RewriteOptions::kCombineCss);
    AddOtherFilter(RewriteOptions::kCombineCss);
  }

  // Test spriting CSS with options to write headers and use a hasher.
  void CombineCss(const StringPiece& id, const char* barrier_text,
                  bool is_barrier) {
    CombineCssWithNames(id, barrier_text, is_barrier, "a.css", "b.css");
  }

  void CombineCssWithNames(const StringPiece& id,
                           const char* barrier_text,
                           bool is_barrier,
                           const char* a_css_name,
                           const char* b_css_name) {
    // URLs and content for HTML document and resources.
    CHECK_EQ(StringPiece::npos, id.find("/"));
    GoogleString html_url = StrCat(kDomain, id, ".html");
    GoogleString a_css_url = StrCat(kDomain, a_css_name);
    GoogleString b_css_url = StrCat(kDomain, b_css_name);
    GoogleString c_css_url = StrCat(kDomain, "c.css");

    static const char html_input_format[] =
        "<head>\n"
        "  <link rel='stylesheet' href='%s' type='text/css'>\n"
        "  <link rel='stylesheet' href='%s' type='text/css'>\n"
        "  <title>Hello, Instaweb</title>\n"
        "%s"
        "</head>\n"
        "<body>\n"
        "  <div class=\"c1\">\n"
        "    <div class=\"c2\">\n"
        "      Yellow on Blue\n"
        "    </div>\n"
        "  </div>\n"
        "  <link rel='stylesheet' href='c.css' type='text/css'>\n"
        "</body>\n";
    GoogleString html_input =
        StringPrintf(html_input_format, a_css_name, b_css_name, barrier_text);
    const char a_css_body[] = ".c1 {\n background-color: blue;\n}\n";
    const char b_css_body[] = ".c2 {\n color: yellow;\n}\n";
    const char c_css_body[] = ".c3 {\n font-weight: bold;\n}\n";

    // Put original CSS files into our fetcher.
    ResponseHeaders default_css_header;
    SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
    SetFetchResponse(a_css_url, default_css_header, a_css_body);
    SetFetchResponse(b_css_url, default_css_header, b_css_body);
    SetFetchResponse(c_css_url, default_css_header, c_css_body);

    ParseUrl(html_url, html_input);

    GoogleString headers;
    AppendDefaultHeaders(kContentTypeCss, &headers);

    // Check for CSS files in the rewritten page.
    StringVector css_urls;
    CollectCssLinks(id, output_buffer_, &css_urls);
    EXPECT_LE(1UL, css_urls.size());

    const GoogleString& combine_url = css_urls[0];
    GoogleUrl gurl(combine_url);

    GoogleString combine_filename;
    EncodeFilename(combine_url, &combine_filename);

    // Expected CSS combination.
    // This syntax must match that in css_combine_filter
    GoogleString expected_combination = StrCat(a_css_body, b_css_body);
    if (!is_barrier) {
      expected_combination.append(c_css_body);
    }

    static const char expected_output_format[] =
        "<head>\n"
        "  <link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">\n"
        "  \n"  // The whitespace from the original link is preserved here ...
        "  <title>Hello, Instaweb</title>\n"
        "%s"
        "</head>\n"
        "<body>\n"
        "  <div class=\"c1\">\n"
        "    <div class=\"c2\">\n"
        "      Yellow on Blue\n"
        "    </div>\n"
        "  </div>\n"
        "  %s\n"
        "</body>\n";
    GoogleString expected_output = StringPrintf(
        expected_output_format, combine_url.c_str(), barrier_text,
        is_barrier ? "<link rel='stylesheet' href='c.css' type='text/css'>"
        : "");
    EXPECT_EQ(AddHtmlBody(expected_output), output_buffer_);

    GoogleString actual_combination;
    ASSERT_TRUE(ReadFile(combine_filename.c_str(), &actual_combination));
    EXPECT_EQ(headers + expected_combination, actual_combination);

    // Fetch the combination to make sure we can serve the result from above.
    RequestHeaders request_headers;
    ResponseHeaders response_headers;
    GoogleString fetched_resource_content;
    StringWriter writer(&fetched_resource_content);
    ExpectCallback dummy_callback(true);
    rewrite_driver()->FetchResource(combine_url, request_headers,
                                    &response_headers, &writer,
                                    &dummy_callback);
    EXPECT_EQ(HttpStatus::kOK, response_headers.status_code()) << combine_url;
    EXPECT_EQ(expected_combination, fetched_resource_content);

    // Now try to fetch from another server (other_rewrite_driver()) that
    // does not already have the combination cached.
    // TODO(sligocki): This has too much shared state with the first server.
    // See RewriteImage for details.
    ResponseHeaders other_response_headers;
    fetched_resource_content.clear();
    message_handler_.Message(kInfo, "Now with serving.");
    file_system()->Enable();
    dummy_callback.Reset();
    other_rewrite_driver()->FetchResource(combine_url, request_headers,
                                          &other_response_headers, &writer,
                                          &dummy_callback);
    EXPECT_EQ(HttpStatus::kOK, other_response_headers.status_code());
    EXPECT_EQ(expected_combination, fetched_resource_content);

    // Try to fetch from an independent server.
    ServeResourceFromManyContexts(combine_url, fetched_resource_content);
  }

  // Test what happens when CSS combine can't find a previously-rewritten
  // resource during a subsequent resource fetch.  This used to segfault.
  void CssCombineMissingResource() {
    GoogleString a_css_url = StrCat(kDomain, "a.css");
    GoogleString c_css_url = StrCat(kDomain, "c.css");

    const char a_css_body[] = ".c1 {\n background-color: blue;\n}\n";
    const char c_css_body[] = ".c3 {\n font-weight: bold;\n}\n";
    GoogleString expected_combination = StrCat(a_css_body, c_css_body);

    // Put original CSS files into our fetcher.
    ResponseHeaders default_css_header;
    SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
    SetFetchResponse(a_css_url, default_css_header, a_css_body);
    SetFetchResponse(c_css_url, default_css_header, c_css_body);

    // First make sure we can serve the combination of a & c.  This is to avoid
    // spurious test successes.

    GoogleString kACUrl = Encode(kDomain, "cc", "0", "a.css+c.css", "css");
    GoogleString kABCUrl = Encode(kDomain, "cc", "0", "a.css+bbb.css+c.css",
                                  "css");
    RequestHeaders request_headers;
    ResponseHeaders response_headers;
    GoogleString fetched_resource_content;
    StringWriter writer(&fetched_resource_content);
    ExpectCallback dummy_callback(true);

    // NOTE: This first fetch used to return status 0 because response_headers
    // weren't initialized by the first resource fetch (but were cached
    // correctly).  Content was correct.
    EXPECT_TRUE(
        rewrite_driver()->FetchResource(kACUrl, request_headers,
                                        &response_headers, &writer,
                                        &dummy_callback));
    EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
    EXPECT_EQ(expected_combination, fetched_resource_content);

    // We repeat the fetch to prove that it succeeds from cache:
    fetched_resource_content.clear();
    dummy_callback.Reset();
    response_headers.Clear();
    EXPECT_TRUE(
        rewrite_driver()->FetchResource(kACUrl, request_headers,
                                        &response_headers, &writer,
                                        &dummy_callback));
    EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
    EXPECT_EQ(expected_combination, fetched_resource_content);

    // Now let's try fetching the url that references a missing resource
    // (bbb.css) in addition to the two that do exist, a.css and c.css.  Using
    // an entirely non-existent resource appears to test a strict superset of
    // filter code paths when compared with returning a 404 for the resource.
    SetFetchFailOnUnexpected(false);
    ExpectCallback fail_callback(false);
    fetched_resource_content.clear();
    response_headers.Clear();
    EXPECT_TRUE(
        rewrite_driver()->FetchResource(kABCUrl, request_headers,
                                        &response_headers, &writer,
                                        &fail_callback));
    // What status we get here depends a lot on details of when exactly
    // we detect the failure. If done early enough, nothing will be set.
    // This test may change, but see also
    // ResourceCombinerTest.TestContinuingFetchWhenFastFailed
    EXPECT_FALSE(response_headers.headers_complete());
    EXPECT_EQ("", fetched_resource_content);
  }

  // Representation for a CSS tag.
  class CssLink {
   public:
    CssLink(const StringPiece& url, const StringPiece& content,
            const StringPiece& media, bool supply_mock)
        : url_(url.data(), url.size()),
          content_(content.data(), content.size()),
          media_(media.data(), media.size()),
          supply_mock_(supply_mock) {
    }

    // A vector of CssLink* should know how to accumulate and add.
    class Vector : public std::vector<CssLink*> {
     public:
      ~Vector() {
        STLDeleteElements(this);
      }

      void Add(const StringPiece& url, const StringPiece& content,
               const StringPiece& media, bool supply_mock) {
        push_back(new CssLink(url, content, media, supply_mock));
      }
    };

    // Parses a combined CSS elementand provides the segments from which
    // it came.
    bool DecomposeCombinedUrl(GoogleString* base, StringVector* segments,
                              MessageHandler* handler) {
      GoogleUrl gurl(url_);
      bool ret = false;
      if (gurl.is_valid()) {
        gurl.AllExceptLeaf().CopyToString(base);
        ResourceNamer namer;
        if (namer.Decode(gurl.LeafWithQuery()) &&
            (namer.id() == RewriteDriver::kCssCombinerId)) {
          UrlMultipartEncoder multipart_encoder;
          GoogleString segment;
          ret = multipart_encoder.Decode(namer.name(), segments, NULL, handler);
        }
      }
      return ret;
    }

    GoogleString url_;
    GoogleString content_;
    GoogleString media_;
    bool supply_mock_;
  };

  // Helper class to collect CSS hrefs.
  class CssCollector : public EmptyHtmlFilter {
   public:
    CssCollector(HtmlParse* html_parse, CssLink::Vector* css_links)
        : css_links_(css_links),
          css_tag_scanner_(html_parse) {
    }

    virtual void EndElement(HtmlElement* element) {
      HtmlElement::Attribute* href;
      const char* media;
      if (css_tag_scanner_.ParseCssElement(element, &href, &media)) {
        // TODO(jmarantz): collect content of the CSS files, before and
        // after combination, so we can diff.
        const char* content = "";
        css_links_->Add(href->value(), content, media, false);
      }
    }

    virtual const char* Name() const { return "CssCollector"; }

   private:
    CssLink::Vector* css_links_;
    CssTagScanner css_tag_scanner_;

    DISALLOW_COPY_AND_ASSIGN(CssCollector);
  };

  // Collects just the hrefs from CSS links into a string vector.
  void CollectCssLinks(const StringPiece& id, const StringPiece& html,
                       StringVector* css_links) {
    CssLink::Vector v;
    CollectCssLinks(id, html, &v);
    for (int i = 0, n = v.size(); i < n; ++i) {
      css_links->push_back(v[i]->url_);
    }
  }

  // Collects all information about CSS links into a CssLink::Vector.
  void CollectCssLinks(const StringPiece& id, const StringPiece& html,
                       CssLink::Vector* css_links) {
    HtmlParse html_parse(&message_handler_);
    CssCollector collector(&html_parse, css_links);
    html_parse.AddFilter(&collector);
    GoogleString dummy_url = StrCat("http://collect.css.links/", id, ".html");
    html_parse.StartParse(dummy_url);
    html_parse.ParseText(html.data(), html.size());
    html_parse.FinishParse();
  }

  // Common framework for testing barriers.  A null-terminated set of css
  // names is specified, with optional media tags.  E.g.
  //   static const char* link[] {
  //     "a.css",
  //     "styles/b.css",
  //     "print.css media=print",
  //   }
  //
  // The output of this function is the collected CSS links after rewrite.
  void BarrierTestHelper(
      const StringPiece& id,
      const CssLink::Vector& input_css_links,
      CssLink::Vector* output_css_links) {
    // TODO(sligocki): Allow other domains (this is constrained right now b/c
    // of InitResponseHeaders.
    GoogleString html_url = StrCat(kTestDomain, id, ".html");
    GoogleString html_input("<head>\n");
    for (int i = 0, n = input_css_links.size(); i < n; ++i) {
      const CssLink* link = input_css_links[i];
      if (!link->url_.empty()) {
        if (link->supply_mock_) {
          // If the css-vector contains a 'true' for this, then we supply the
          // mock fetcher with headers and content for the CSS file.
          InitResponseHeaders(link->url_, kContentTypeCss, link->content_, 600);
        }
        GoogleString style("  <");
        style += StringPrintf("link rel='stylesheet' type='text/css' href='%s'",
                              link->url_.c_str());
        if (!link->media_.empty()) {
          StrAppend(&style, " media='", link->media_, "'");
        }
        style += ">\n";
        html_input += style;
      } else {
        html_input += link->content_;
      }
    }
    html_input += "</head>\n<body>\n  <div class='yellow'>\n";
    html_input += "    Hello, mod_pagespeed!\n  </div>\n</body>\n";

    ParseUrl(html_url, html_input);
    CollectCssLinks("combine_css_missing_files", output_buffer_,
                    output_css_links);

    // TODO(jmarantz): fetch all content and provide output as text.
  }

  // Helper for testing handling of URLs with trailing junk
  void TestCorruptUrl(const char* junk, bool should_fetch_ok) {
    CssLink::Vector css_in, css_out;
    css_in.Add("1.css", kYellow, "", true);
    css_in.Add("2.css", kYellow, "", true);
    BarrierTestHelper("no_ext_corrupt", css_in, &css_out);
    ASSERT_EQ(1, css_out.size());
    GoogleString normal_url = css_out[0]->url_;

    GoogleString out;
    EXPECT_EQ(should_fetch_ok,
              ServeResourceUrl(StrCat(normal_url, junk),  &out));

    // Now re-do it and make sure %22 didn't get stuck in the URL
    STLDeleteElements(&css_out);
    css_out.clear();
    BarrierTestHelper("no_ext_corrupt", css_in, &css_out);
    ASSERT_EQ(1, css_out.size());
    EXPECT_EQ(css_out[0]->url_, normal_url);
  }

  // Test to make sure we don't miscombine things when handling the input
  // as XHTML producing non-flat <link>'s from the parser
  void TestXhtml(bool flush) {
    GoogleString a_css_url = StrCat(kTestDomain, "a.css");
    GoogleString b_css_url = StrCat(kTestDomain, "b.css");

    ResponseHeaders default_css_header;
    SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
    SetFetchResponse(a_css_url, default_css_header, "A");
    SetFetchResponse(b_css_url, default_css_header, "B");

    GoogleString combined_url =
        StrCat(kTestDomain, "a.css+b.css.pagespeed.cc.0.css");

    SetupWriter();
    rewrite_driver()->StartParse(kTestDomain);
    GoogleString input_beginning =
        StrCat(kXhtmlDtd, "<div><link rel=stylesheet href=a.css>",
               "<link rel=stylesheet href=b.css>");
    rewrite_driver()->ParseText(input_beginning);

    if (flush) {
      // This is a regression test: previously getting a flush here would
      // cause attempts to modify data structures, as we would only
      // start seeing the links at the </div>
      rewrite_driver()->Flush();
    }
    rewrite_driver()->ParseText("</div>");
    rewrite_driver()->FinishParse();

    // Note: As of 3/25/2011 our parser ignores XHTML directives from DOCTYPE
    // or mime-type, since those are not reliable: see Issue 252.  So we
    // do sloppy HTML-style parsing in all cases.  If we were to decided that
    // we could reliably detect XHTML then we could consider tightening the
    // parser constraints, in which case the expected results from this
    // code might change depending on the 'flush' arg to this method.
    EXPECT_EQ(
        StrCat(kXhtmlDtd, "<div>",
               "<link rel=\"stylesheet\" type=\"text/css\" href=\"",
               combined_url, "\"></div>"),
        output_buffer_);
  }

  void CombineWithBaseTag(const char* html_input, StringVector *css_urls) {
    // Put original CSS files into our fetcher.
    GoogleString html_url = StrCat(kDomain, "base_url.html");
    const char a_css_url[] = "http://other_domain.test/foo/a.css";
    const char b_css_url[] = "http://other_domain.test/foo/b.css";
    const char c_css_url[] = "http://other_domain.test/foo/c.css";

    const char a_css_body[] = ".c1 {\n background-color: blue;\n}\n";
    const char b_css_body[] = ".c2 {\n color: yellow;\n}\n";
    const char c_css_body[] = ".c3 {\n font-weight: bold;\n}\n";

    ResponseHeaders default_css_header;
    SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
    SetFetchResponse(a_css_url, default_css_header, a_css_body);
    SetFetchResponse(b_css_url, default_css_header, b_css_body);
    SetFetchResponse(c_css_url, default_css_header, c_css_body);

    // Rewrite
    ParseUrl(html_url, html_input);

    // Check for CSS files in the rewritten page.
    CollectCssLinks("combine_css_no_media-links", output_buffer_, css_urls);
  }
};

TEST_F(CssCombineFilterTest, CombineCss) {
  CombineCss("combine_css_no_hash", "", false);
}

TEST_F(CssCombineFilterTest, CombineCssMD5) {
  UseMd5Hasher();
  CombineCss("combine_css_md5", "", false);
}

// Make sure that if we re-parse the same html twice we do not
// end up recomputing the CSS (and writing to cache) again
TEST_F(CssCombineFilterTest, CombineCssRecombine) {
  UseMd5Hasher();
  CombineCss("combine_css_recombine", "", false);
  int inserts_before = lru_cache()->num_inserts();

  CombineCss("combine_css_recombine", "", false);
  int inserts_after = lru_cache()->num_inserts();
  EXPECT_EQ(inserts_before, inserts_after);
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());
}


// http://code.google.com/p/modpagespeed/issues/detail?q=css&id=39
TEST_F(CssCombineFilterTest, DealWithParams) {
  CombineCssWithNames("with_params", "", false, "a.css?U", "b.css?rev=138");
}

// http://code.google.com/p/modpagespeed/issues/detail?q=css&id=252
TEST_F(CssCombineFilterTest, ClaimsXhtmlButHasUnclosedLink) {
  // XHTML text should not have unclosed links.  But if they do, like
  // in Issue 252, then we should leave them alone.
  static const char html_format[] =
      "<head>\n"
      "  %s\n"
      "  %s\n"
      "</head>\n"
      "<body><div class=\"c1\"><div class=\"c2\"><p>\n"
      "  Yellow on Blue</p></div></div></body>";

  static const char unclosed_links[] =
      "  <link rel='stylesheet' href='a.css' type='text/css'>\n"  // unclosed
      "  <script type='text/javascript' src='c.js'></script>"     // 'in' <link>
      "  <link rel='stylesheet' href='b.css' type='text/css'>";
  static const char combination[] =
      "  <link rel=\"stylesheet\" type=\"text/css\" "
      "href=\"http://test.com/a.css+b.css.pagespeed.cc.0.css\">\n"
      "  <script type='text/javascript' src='c.js'></script>  ";

  // Put original CSS files into our fetcher.
  ResponseHeaders default_css_header;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
  SetFetchResponse(StrCat(kTestDomain, "a.css"), default_css_header, ".a {}");
  SetFetchResponse(StrCat(kTestDomain, "b.css"), default_css_header, ".b {}");
  ValidateExpected("claims_xhtml_but_has_unclosed_links",
                   StringPrintf(html_format, kXhtmlDtd, unclosed_links),
                   StringPrintf(html_format, kXhtmlDtd, combination));
}

TEST_F(CssCombineFilterTest, CombineCssWithIEDirective) {
  const char ie_directive_barrier[] =
      "<!--[if IE]>\n"
      "<link rel=\"stylesheet\" type=\"text/css\" "
      "href=\"http://graphics8.nytimes.com/css/"
      "0.1/screen/build/homepage/ie.css\">\n"
      "<![endif]-->";
  UseMd5Hasher();
  CombineCss("combine_css_ie", ie_directive_barrier, true);
}

TEST_F(CssCombineFilterTest, CombineCssWithStyle) {
  const char style_barrier[] = "<style>a { color: red }</style>\n";
  UseMd5Hasher();
  CombineCss("combine_css_style", style_barrier, true);
}

TEST_F(CssCombineFilterTest, CombineCssWithBogusLink) {
  const char bogus_barrier[] = "<link rel='stylesheet' type='text/css' "
      "href='crazee://big/blue/fake'>\n";
  UseMd5Hasher();
  CombineCss("combine_css_bogus_link", bogus_barrier, true);
}

TEST_F(CssCombineFilterTest, CombineCssWithImportInFirst) {
  CssLink::Vector css_in, css_out;
  css_in.Add("1.css", "@Import \"1a.css\"", "", true);
  css_in.Add("2.css", kYellow, "", true);
  css_in.Add("3.css", kYellow, "", true);
  BarrierTestHelper("combine_css_with_import1", css_in, &css_out);
  EXPECT_EQ(1, css_out.size());
}

TEST_F(CssCombineFilterTest, CombineCssWithImportInSecond) {
  CssLink::Vector css_in, css_out;
  css_in.Add("1.css", kYellow, "", true);
  css_in.Add("2.css", "@Import \"2a.css\"", "", true);
  css_in.Add("3.css", kYellow, "", true);
  BarrierTestHelper("combine_css_with_import1", css_in, &css_out);
  EXPECT_EQ("1.css", css_out[0]->url_);
  EXPECT_EQ(2, css_out.size());
}

TEST_F(CssCombineFilterTest, CombineCssWithNoscriptBarrier) {
  const char noscript_barrier[] =
      "<noscript>\n"
      "  <link rel='stylesheet' type='text/css' href='d.css'>\n"
      "</noscript>\n";

  // Put this in the Test class to remove repetition here and below.
  GoogleString d_css_url = StrCat(kDomain, "d.css");
  const char d_css_body[] = ".c4 {\n color: green;\n}\n";
  ResponseHeaders default_css_header;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
  SetFetchResponse(d_css_url, default_css_header, d_css_body);

  UseMd5Hasher();
  CombineCss("combine_css_noscript", noscript_barrier, true);
}

TEST_F(CssCombineFilterTest, CombineCssWithFakeNoscriptBarrier) {
  const char non_barrier[] =
      "<noscript>\n"
      "  <p>You have no scripts installed</p>\n"
      "</noscript>\n";
  UseMd5Hasher();
  CombineCss("combine_css_fake_noscript", non_barrier, false);
}

TEST_F(CssCombineFilterTest, CombineCssWithMediaBarrier) {
  const char media_barrier[] =
      "<link rel='stylesheet' type='text/css' href='d.css' media='print'>\n";

  GoogleString d_css_url = StrCat(kDomain, "d.css");
  const char d_css_body[] = ".c4 {\n color: green;\n}\n";
  ResponseHeaders default_css_header;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
  SetFetchResponse(d_css_url, default_css_header, d_css_body);

  UseMd5Hasher();
  CombineCss("combine_css_media", media_barrier, true);
}

TEST_F(CssCombineFilterTest, CombineCssWithNonMediaBarrier) {
  // Put original CSS files into our fetcher.
  GoogleString html_url = StrCat(kDomain, "no_media_barrier.html");
  GoogleString a_css_url = StrCat(kDomain, "a.css");
  GoogleString b_css_url = StrCat(kDomain, "b.css");
  GoogleString c_css_url = StrCat(kDomain, "c.css");
  GoogleString d_css_url = StrCat(kDomain, "d.css");

  const char a_css_body[] = ".c1 {\n background-color: blue;\n}\n";
  const char b_css_body[] = ".c2 {\n color: yellow;\n}\n";
  const char c_css_body[] = ".c3 {\n font-weight: bold;\n}\n";
  const char d_css_body[] = ".c4 {\n color: green;\n}\n";

  ResponseHeaders default_css_header;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
  SetFetchResponse(a_css_url, default_css_header, a_css_body);
  SetFetchResponse(b_css_url, default_css_header, b_css_body);
  SetFetchResponse(c_css_url, default_css_header, c_css_body);
  SetFetchResponse(d_css_url, default_css_header, d_css_body);

  // Only the first two CSS files should be combined.
  const char html_input[] =
      "<head>\n"
      "  <link rel='stylesheet' type='text/css' href='a.css' media='print'>\n"
      "  <link rel='stylesheet' type='text/css' href='b.css' media='print'>\n"
      "  <link rel='stylesheet' type='text/css' href='c.css'>\n"
      "  <link rel='stylesheet' type='text/css' href='d.css' media='print'>\n"
      "</head>";

  // Rewrite
  ParseUrl(html_url, html_input);

  // Check for CSS files in the rewritten page.
  StringVector css_urls;
  CollectCssLinks("combine_css_no_media-links", output_buffer_, &css_urls);
  EXPECT_EQ(3UL, css_urls.size());
  const GoogleString& combine_url = css_urls[0];

  const char expected_output_format[] =
      "<head>\n"
      "  <link rel=\"stylesheet\" type=\"text/css\" media=\"print\" "
      "href=\"%s\">\n"
      "  \n"
      "  <link rel='stylesheet' type='text/css' href='c.css'>\n"
      "  <link rel='stylesheet' type='text/css' href='d.css' media='print'>\n"
      "</head>";
  GoogleString expected_output = StringPrintf(expected_output_format,
                                              combine_url.c_str());

  EXPECT_EQ(AddHtmlBody(expected_output), output_buffer_);
}

// This test, as rewritten as of March 2011, is testing an invalid HTML
// construct, where no hrefs should precede a base tag.  The current expected
// behavior is that we leave any urls before the base tag alone, and then try
// to combine urls after the base tag.  Since this test has only one css after
// the base tag, it should leave that one alone.
TEST_F(CssCombineFilterTest, NoCombineCssBaseUrlOutOfOrder) {
  StringVector css_urls;
  const char input_buffer[] =
      "<head>\n"
      "  <link rel='stylesheet' type='text/css' href='a.css'>\n"
      "  <base href='http://other_domain.test/foo/'>\n"
      "  <link rel='stylesheet' type='text/css' href='b.css'>\n"
      "</head>\n";
  CombineWithBaseTag(input_buffer, &css_urls);
  EXPECT_EQ(2UL, css_urls.size());
  EXPECT_EQ(AddHtmlBody(input_buffer), output_buffer_);
}

// Same invalid configuration, but now with two css refs after the base tag,
// which should get combined.
TEST_F(CssCombineFilterTest, CombineCssBaseUrlOutOfOrder) {
  StringVector css_urls;
  const char input_buffer[] =
      "<head>\n"
      "  <link rel='stylesheet' type='text/css' href='a.css'>\n"
      "  <base href='http://other_domain.test/foo/'>\n"
      "  <link rel='stylesheet' type='text/css' href='b.css'>\n"
      "  <link rel='stylesheet' type='text/css' href='c.css'>\n"
      "</head>\n";
  CombineWithBaseTag(input_buffer, &css_urls);

  const char expected_output_format[] =
      "<head>\n"
      "  <link rel='stylesheet' type='text/css' href='a.css'>\n"
      "  <base href='http://other_domain.test/foo/'>\n"
      "  <link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">\n"
      "  \n"
      "</head>\n";
  EXPECT_EQ(2UL, css_urls.size());

  GoogleString expected_output = StringPrintf(expected_output_format,
                                              css_urls[1].c_str());
  EXPECT_EQ("http://other_domain.test/foo/b.css+c.css.pagespeed.cc.0.css",
            css_urls[1]);
  EXPECT_EQ(AddHtmlBody(expected_output), output_buffer_);
  EXPECT_TRUE(GoogleUrl(css_urls[1]).is_valid());
}

// Same invalid configuration, but now with a full qualified url before
// the base tag.  We should be able to find and combine that one.
TEST_F(CssCombineFilterTest, CombineCssAbsoluteBaseUrlOutOfOrder) {
  StringVector css_urls;
  const char input_buffer[] =
      "<head>\n"
      "  <link rel='stylesheet' type='text/css'"
      " href='http://other_domain.test/foo/a.css'>\n"
      "  <base href='http://other_domain.test/foo/'>\n"
      "  <link rel='stylesheet' type='text/css' href='b.css'>\n"
      "</head>\n";
  CombineWithBaseTag(input_buffer, &css_urls);

  const char expected_output_format[] =
      "<head>\n"
      "  <link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">\n"
      "  <base href='http://other_domain.test/foo/'>\n"
      "  \n"
      "</head>\n";
  EXPECT_EQ(1UL, css_urls.size());

  GoogleString expected_output = StringPrintf(expected_output_format,
                                              css_urls[0].c_str());
  EXPECT_EQ("http://other_domain.test/foo/a.css+b.css.pagespeed.cc.0.css",
            css_urls[0]);
  EXPECT_EQ(AddHtmlBody(expected_output), output_buffer_);
  EXPECT_TRUE(GoogleUrl(css_urls[0]).is_valid());
}

// Here's the same test as NoCombineCssBaseUrlOutOfOrder, legalized to have
// the base url before the first link.
TEST_F(CssCombineFilterTest, CombineCssBaseUrlCorrectlyOrdered) {
  // <base> tag correctly precedes any urls.
  StringVector css_urls;
  CombineWithBaseTag(
      "<head>\n"
      "  <base href='http://other_domain.test/foo/'>\n"
      "  <link rel='stylesheet' type='text/css' href='a.css'>\n"
      "  <link rel='stylesheet' type='text/css' href='b.css'>\n"
      "</head>\n", &css_urls);

  const char expected_output_format[] =
      "<head>\n"
      "  <base href='http://other_domain.test/foo/'>\n"
      "  <link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">\n"
      "  \n"
      "</head>\n";
  EXPECT_EQ(1UL, css_urls.size());
  GoogleString expected_output = StringPrintf(expected_output_format,
                                              css_urls[0].c_str());
  EXPECT_EQ(AddHtmlBody(expected_output), output_buffer_);
  EXPECT_EQ("http://other_domain.test/foo/a.css+b.css.pagespeed.cc.0.css",
            css_urls[0]);
  EXPECT_TRUE(GoogleUrl(css_urls[0]).is_valid());
}

TEST_F(CssCombineFilterTest, CombineCssNoInput) {
  SetFetchFailOnUnexpected(false);
  ResponseHeaders default_css_header;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
  SetFetchResponse(StrCat(kTestDomain, "b.css"),
                   default_css_header, ".a {}");
  static const char html_input[] =
      "<head>\n"
      "  <link rel='stylesheet' href='a_broken.css' type='text/css'>\n"
      "  <link rel='stylesheet' href='b.css' type='text/css'>\n"
      "</head>\n"
      "<body><div class=\"c1\"><div class=\"c2\"><p>\n"
      "  Yellow on Blue</p></div></div></body>";
  ValidateNoChanges("combine_css_missing_input", html_input);
}

TEST_F(CssCombineFilterTest, CombineCssXhtml) {
  TestXhtml(false);
}

TEST_F(CssCombineFilterTest, CombineCssXhtmlWithFlush) {
  TestXhtml(true);
}

TEST_F(CssCombineFilterTest, CombineCssMissingResource) {
  CssCombineMissingResource();
}

TEST_F(CssCombineFilterTest, CombineCssManyFiles) {
  // Prepare an HTML fragment with too many CSS files to combine,
  // exceeding the char limit.
  //
  // It looks like we can fit a limited number of encodings of
  // "yellow%d.css" in the buffer.  It might be more general to base
  // this on the constant declared in RewriteOptions but I think it's
  // easier to understand leaving these exposed as constants; we can
  // abstract them later.
  const int kNumCssLinks = 100;
  // Note: Without CssCombine::Partnership::kUrlSlack this was:
  // const int kNumCssInCombination = 18
  const int kNumCssInCombination = 70;  // based on how we encode "yellow%d.css"
  CssLink::Vector css_in, css_out;
  for (int i = 0; i < kNumCssLinks; ++i) {
    css_in.Add(StringPrintf("styles/yellow%d.css", i),
               kYellow, "", true);
  }
  BarrierTestHelper("combine_css_many_files", css_in, &css_out);
  ASSERT_EQ(2, css_out.size());

  // Check that the first element is really a combination.
  GoogleString base;
  StringVector segments;
  ASSERT_TRUE(css_out[0]->DecomposeCombinedUrl(&base, &segments,
                                               &message_handler_));
  EXPECT_EQ(StrCat(kTestDomain, "styles/"), base);
  EXPECT_EQ(kNumCssInCombination, segments.size());

  segments.clear();
  ASSERT_TRUE(css_out[1]->DecomposeCombinedUrl(&base, &segments,
                                               &message_handler_));
  EXPECT_EQ(StrCat(kTestDomain, "styles/"), base);
  EXPECT_EQ(kNumCssLinks - kNumCssInCombination, segments.size());
}

TEST_F(CssCombineFilterTest, CombineCssManyFilesOneOrphan) {
  // This test differs from the previous test in we have exactly one CSS file
  // that stays on its own.
  // Note: Without CssCombine::Partnership::kUrlSlack this was:
  // const int kNumCssInCombination = 18
  const int kNumCssInCombination = 70;  // based on how we encode "yellow%d.css"
  const int kNumCssLinks = kNumCssInCombination + 1;
  CssLink::Vector css_in, css_out;
  for (int i = 0; i < kNumCssLinks - 1; ++i) {
    css_in.Add(StringPrintf("styles/yellow%d.css", i),
               kYellow, "", true);
  }
  css_in.Add("styles/last_one.css",
             kYellow, "", true);
  BarrierTestHelper("combine_css_many_files", css_in, &css_out);
  ASSERT_EQ(2, css_out.size());

  // Check that the first element is really a combination.
  GoogleString base;
  StringVector segments;
  ASSERT_TRUE(css_out[0]->DecomposeCombinedUrl(&base, &segments,
                                               &message_handler_));
  EXPECT_EQ(StrCat(kTestDomain, "styles/"), base);
  EXPECT_EQ(kNumCssInCombination, segments.size());
  EXPECT_EQ("styles/last_one.css", css_out[1]->url_);
}

// Note -- this test is redundant with CombineCssMissingResource -- this
// is a taste test.  This new mechanism is more code per test but I think
// the failures are more obvious and the expect/assert tests are in the
// top level of the test which might make it easier to debug.
TEST_F(CssCombineFilterTest, CombineCssNotCached) {
  CssLink::Vector css_in, css_out;
  css_in.Add("1.css", kYellow, "", true);
  css_in.Add("2.css", kYellow, "", true);
  css_in.Add("3.css", kYellow, "", false);
  css_in.Add("4.css", kYellow, "", true);
  SetFetchFailOnUnexpected(false);
  BarrierTestHelper("combine_css_not_cached", css_in, &css_out);
  EXPECT_EQ(3, css_out.size());
  GoogleString base;
  StringVector segments;
  ASSERT_TRUE(css_out[0]->DecomposeCombinedUrl(&base, &segments,
                                               &message_handler_));
  EXPECT_EQ(2, segments.size());
  EXPECT_EQ("1.css", segments[0]);
  EXPECT_EQ("2.css", segments[1]);
  EXPECT_EQ("3.css", css_out[1]->url_);
  EXPECT_EQ("4.css", css_out[2]->url_);
}

// Note -- this test is redundant with CombineCssWithIEDirective -- this
// is a taste test.
TEST_F(CssCombineFilterTest, CombineStyleTag) {
  CssLink::Vector css_in, css_out;
  css_in.Add("1.css", kYellow, "", true);
  css_in.Add("2.css", kYellow, "", true);
  css_in.Add("", "<style>a { color: red }</style>\n", "", false);
  css_in.Add("4.css", kYellow, "", true);
  BarrierTestHelper("combine_css_with_style", css_in, &css_out);
  EXPECT_EQ(2, css_out.size());
  GoogleString base;
  StringVector segments;
  ASSERT_TRUE(css_out[0]->DecomposeCombinedUrl(&base, &segments,
                                               &message_handler_));
  EXPECT_EQ(2, segments.size());
  EXPECT_EQ("1.css", segments[0]);
  EXPECT_EQ("2.css", segments[1]);
  EXPECT_EQ("4.css", css_out[1]->url_);
}

TEST_F(CssCombineFilterTest, NoAbsolutifySameDir) {
  CssLink::Vector css_in, css_out;
  css_in.Add("1.css", ".yellow {background-image: url('1.png');}\n", "", true);
  css_in.Add("2.css", ".yellow {background-image: url('2.png');}\n", "", true);
  BarrierTestHelper("combine_css_with_style", css_in, &css_out);
  EXPECT_EQ(1, css_out.size());

  // Note: the urls are not absolutified.
  GoogleString expected_combination =
      ".yellow {background-image: url('1.png');}\n"
      ".yellow {background-image: url('2.png');}\n";

  // Check fetched resource.
  GoogleString actual_combination;
  EXPECT_TRUE(ServeResourceUrl(css_out[0]->url_, &actual_combination));
  // TODO(sligocki): Check headers?
  EXPECT_EQ(expected_combination, actual_combination);
}

TEST_F(CssCombineFilterTest, DoAbsolutifyDifferentDir) {
  CssLink::Vector css_in, css_out;
  css_in.Add("1.css", ".yellow {background-image: url('1.png');}\n", "", true);
  css_in.Add("foo/2.css", ".yellow {background-image: url('2.png');}\n",
             "", true);
  BarrierTestHelper("combine_css_with_style", css_in, &css_out);
  EXPECT_EQ(1, css_out.size());

  GoogleString expected_combination = StrCat(
      ".yellow {background-image: url('1.png');}\n"
      ".yellow {background-image: url('", kTestDomain, "foo/2.png');}\n");

  // Check fetched resource.
  GoogleString actual_combination;
  EXPECT_TRUE(ServeResourceUrl(css_out[0]->url_, &actual_combination));
  // TODO(sligocki): Check headers?
  EXPECT_EQ(expected_combination, actual_combination);
}

// Verifies that we don't produce URLs that are too long in a corner case.
TEST_F(CssCombineFilterTest, CrossAcrossPathsExceedingUrlSize) {
  CssLink::Vector css_in, css_out;
  GoogleString long_name(600, 'z');
  css_in.Add(long_name + "/a.css", "a", "", true);
  css_in.Add(long_name + "/b.css", "b", "", true);

  // This last 'Add' causes the resolved path to change from long_path to "/".
  // Which makes the encoding way too long. So we expect this URL not to be
  // added to the combination and for the combination base to remain long_path.
  css_in.Add("sites/all/modules/ckeditor/ckeditor.css?3", "z", "", true);
  BarrierTestHelper("cross_paths", css_in, &css_out);
  EXPECT_EQ(2, css_out.size());
  GoogleString actual_combination;
  EXPECT_TRUE(ServeResourceUrl(css_out[0]->url_, &actual_combination));
  GoogleUrl gurl(css_out[0]->url_);
  EXPECT_EQ("/" + long_name + "/", gurl.PathSansLeaf());
  ResourceNamer namer;
  ASSERT_TRUE(namer.Decode(gurl.LeafWithQuery()));
  EXPECT_EQ("a.css+b.css", namer.name());
  EXPECT_EQ("ab", actual_combination);
}

// Verifies that we don't allow path-crossing URLs if that option is turned off.
TEST_F(CssCombineFilterTest, CrossAcrossPathsDisallowed) {
  options()->set_combine_across_paths(false);
  CssLink::Vector css_in, css_out;
  css_in.Add("a/a.css", "a", "", true);
  css_in.Add("b/b.css", "b", "", true);
  BarrierTestHelper("cross_paths", css_in, &css_out);
  ASSERT_EQ(2, css_out.size());
  EXPECT_EQ("a/a.css", css_out[0]->url_);
  EXPECT_EQ("b/b.css", css_out[1]->url_);
}

TEST_F(CssCombineFilterTest, CrossMappedDomain) {
  CssLink::Vector css_in, css_out;
  DomainLawyer* laywer = options()->domain_lawyer();
  laywer->AddRewriteDomainMapping("a.com", "b.com", &message_handler_);
  bool supply_mock = false;
  css_in.Add("http://a.com/1.css", kYellow, "", supply_mock);
  css_in.Add("http://b.com/2.css", kBlue, "", supply_mock);
  ResponseHeaders default_css_header;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
  SetFetchResponse("http://a.com/1.css", default_css_header,
                                kYellow);
  SetFetchResponse("http://b.com/2.css", default_css_header,
                                kBlue);
  BarrierTestHelper("combine_css_with_style", css_in, &css_out);
  EXPECT_EQ(1, css_out.size());
  GoogleString actual_combination;
  EXPECT_TRUE(ServeResourceUrl(css_out[0]->url_, &actual_combination));
  EXPECT_EQ(StringPiece("http://a.com/1.css+2.css.pagespeed.cc.0.css"),
            css_out[0]->url_);
  EXPECT_EQ(StrCat(kYellow, kBlue), actual_combination);
}

// Verifies that we cannot do the same cross-domain combo when we lack
// the domain mapping.
TEST_F(CssCombineFilterTest, CrossUnmappedDomain) {
  CssLink::Vector css_in, css_out;
  DomainLawyer* laywer = options()->domain_lawyer();
  laywer->AddDomain("a.com", &message_handler_);
  laywer->AddDomain("b.com", &message_handler_);
  bool supply_mock = false;
  const char kUrl1[] = "http://a.com/1.css";
  const char kUrl2[] = "http://b.com/2.css";
  css_in.Add(kUrl1, kYellow, "", supply_mock);
  css_in.Add(kUrl2, kBlue, "", supply_mock);
  ResponseHeaders default_css_header;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
  SetFetchResponse(kUrl1, default_css_header, kYellow);
  SetFetchResponse(kUrl2, default_css_header, kBlue);
  BarrierTestHelper("combine_css_with_style", css_in, &css_out);
  EXPECT_EQ(2, css_out.size());
  GoogleString actual_combination;
  EXPECT_EQ(kUrl1, css_out[0]->url_);
  EXPECT_EQ(kUrl2, css_out[1]->url_);
}

// Make sure bad requests do not corrupt our extension.
TEST_F(CssCombineFilterTest, NoExtensionCorruption) {
  TestCorruptUrl("%22", false);
}

TEST_F(CssCombineFilterTest, NoQueryCorruption) {
  TestCorruptUrl("?query", true);
}

/*
  TODO(jmarantz): cover intervening FLUSH
  TODO(jmarantz): consider converting some of the existing tests to this
   format, covering
           IE Directive
           @Import in any css element except the first
           link in noscript tag
           change in 'media'
           incompatible domain
           intervening inline style tag (TODO: outline first?)
*/

}  // namespace

}  // namespace net_instaweb
