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

// Unit-test the cache extender.

#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/rewriter/public/css_outline_filter.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_manager_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

namespace {

const char kCssFormat[] = "<link rel='stylesheet' href='%s' type='text/css'>\n";
const char kHtmlFormat[] =
    "<link rel='stylesheet' href='%s' type='text/css'>\n"
    "<img src='%s'/>\n"
    "<script type='text/javascript' src='%s'></script>\n";

// See Issue 295: cache_extender, which only rewrites content on
// fetch, failed to recognize a cache-extended CSS file specified with
// a query-param as CSS.  It failed to recognize it because its
// file-extension was obscured by a query-param.  Moreover, we should
// not be dependent on input resource extensions to determine
// content-type.  Thus it did not run its absolutification pass.
//
// Instead we must ensure that the content-type is discovered from the
// input resource response headers.
const char kCssFile[]       = "sub/a.css?v=1";
const char kCssFileQuoted[] = "sub/a.css,qv=1";
const char kCssDataFormat[] = ".blue {color: blue; src: url(%sembedded.png);}";
const char kFilterId[]      = "ce";
const char kImageData[]     = "Not really JPEG but irrelevant for this test";
const char kJsData[]        = "alert('hello, world!')";
const char kNewDomain[]     = "http://new.com/";
const int kShortTtlSec      = 100;
const int kMediumTtlSec     = 100000;
const int kLongTtlSec       = 100000000;

class CacheExtenderTest : public ResourceManagerTestBase,
                          public ::testing::WithParamInterface<bool> {
 protected:
  CacheExtenderTest() : kCssData(CssData("")) {}

  virtual void SetUp() {
    ResourceManagerTestBase::SetUp();
    rewrite_driver()->SetAsynchronousRewrites(GetParam());
  }

  void InitTest(int64 ttl) {
    AddFilter(RewriteOptions::kExtendCache);
    InitResponseHeaders(kCssFile, kContentTypeCss, kCssData, ttl);
    InitResponseHeaders("b.jpg", kContentTypeJpeg, kImageData, ttl);
    InitResponseHeaders("c.js", kContentTypeJavascript, kJsData, ttl);
  }

  // Generate HTML loading 3 resources with the specified URLs
  GoogleString GenerateHtml(const GoogleString& a,
                            const GoogleString& b,
                            const GoogleString& c) {
    return StringPrintf(kHtmlFormat, a.c_str(), b.c_str(), c.c_str());
  }

  // Helper to test for how we handle trailing junk in URLs
  void TestCorruptUrl(const char* junk, bool should_fetch_ok) {
    InitTest(kShortTtlSec);
    GoogleString a_ext = Encode(kTestDomain, "ce", "0", kCssFile, "css");
    GoogleString b_ext = Encode(kTestDomain, "ce", "0", "b.jpg", "jpg");
    GoogleString c_ext = Encode(kTestDomain, "ce", "0", "c.js", "js");

    ValidateExpected("no_ext_corrupt_fetched",
                     GenerateHtml(kCssFile, "b.jpg", "c.js"),
                     GenerateHtml(a_ext, b_ext, c_ext));
    GoogleString output;
    EXPECT_EQ(should_fetch_ok, ServeResourceUrl(StrCat(a_ext, junk), &output));
    EXPECT_EQ(should_fetch_ok, ServeResourceUrl(StrCat(b_ext, junk), &output));
    EXPECT_EQ(should_fetch_ok, ServeResourceUrl(StrCat(c_ext, junk), &output));
    ValidateExpected("no_ext_corrupt_cached",
                     GenerateHtml(kCssFile, "b.jpg", "c.js"),
                     GenerateHtml(a_ext, b_ext, c_ext));
  }

  static GoogleString CssData(const StringPiece& url) {
    return StringPrintf(kCssDataFormat, url.as_string().c_str());
  }

  const GoogleString kCssData;
};

TEST_P(CacheExtenderTest, DoExtend) {
  InitTest(kShortTtlSec);
  for (int i = 0; i < 3; i++) {
    ValidateExpected(
        "do_extend",
        GenerateHtml(kCssFile, "b.jpg", "c.js"),
        GenerateHtml(Encode(kTestDomain, "ce", "0", kCssFile, "css"),
                     Encode(kTestDomain, "ce", "0", "b.jpg", "jpg"),
                     Encode(kTestDomain, "ce", "0", "c.js", "js")));
  }
}

TEST_P(CacheExtenderTest, UrlTooLong) {
  AddFilter(RewriteOptions::kExtendCache);

  // Make the filename too long.
  GoogleString long_string(options()->max_url_segment_size() + 1, 'z');

  GoogleString css_name = StrCat("style.css?z=", long_string);
  GoogleString jpg_name = StrCat("image.jpg?z=", long_string);
  GoogleString js_name  = StrCat("script.js?z=", long_string);
  InitResponseHeaders(css_name, kContentTypeCss, kCssData, kShortTtlSec);
  InitResponseHeaders(jpg_name, kContentTypeJpeg, kImageData, kShortTtlSec);
  InitResponseHeaders(js_name, kContentTypeJavascript, kJsData, kShortTtlSec);

  // If filename wasn't too long, this would be rewritten (like in DoExtend).
  ValidateNoChanges("url_too_long", GenerateHtml(css_name, jpg_name, js_name));
}

TEST_P(CacheExtenderTest, NoInputResource) {
  InitTest(kShortTtlSec);
  // Test for not crashing on bad/disallowed URL.
  ValidateNoChanges("bad url",
                    GenerateHtml("swly://example.com/sub/a.css",
                                 "http://evil.com/b.jpg",
                                 "http://moreevil.com/c.js"));
}

TEST_P(CacheExtenderTest, NoExtendAlreadyCachedProperly) {
  InitTest(kLongTtlSec);  // cached for a long time to begin with
  ValidateNoChanges("no_extend_cached_properly",
                    GenerateHtml(kCssFile, "b.jpg", "c.js"));
}

TEST_P(CacheExtenderTest, ExtendIfSharded) {
  InitTest(kLongTtlSec);  // cached for a long time to begin with
  EXPECT_TRUE(options()->domain_lawyer()->AddShard(
      kTestDomain, "shard0.com,shard1.com", &message_handler_));
  // shard0 is always selected in the test because of our mock hasher
  // that always returns 0.
  ValidateExpected("extend_if_sharded",
                   GenerateHtml(kCssFile, "b.jpg", "c.js"),
                   GenerateHtml(
                       "http://shard0.com/sub/a.css,qv=1.pagespeed.ce.0.css",
                       "http://shard0.com/b.jpg.pagespeed.ce.0.jpg",
                       "http://shard0.com/c.js.pagespeed.ce.0.js"));
}

TEST_P(CacheExtenderTest, ExtendIfRewritten) {
  InitTest(kLongTtlSec);  // cached for a long time to begin with

  EXPECT_TRUE(options()->domain_lawyer()->AddRewriteDomainMapping(
      "cdn.com", kTestDomain, &message_handler_));
  ValidateExpected("extend_if_rewritten",
                   GenerateHtml(kCssFile, "b.jpg", "c.js"),
                   GenerateHtml(
                       "http://cdn.com/sub/a.css,qv=1.pagespeed.ce.0.css",
                       "http://cdn.com/b.jpg.pagespeed.ce.0.jpg",
                       "http://cdn.com/c.js.pagespeed.ce.0.js"));
}

TEST_P(CacheExtenderTest, ExtendIfShardedAndRewritten) {
  InitTest(kLongTtlSec);  // cached for a long time to begin with

  EXPECT_TRUE(options()->domain_lawyer()->AddRewriteDomainMapping(
      "cdn.com", kTestDomain, &message_handler_));

  // Domain-rewriting is performed first.  Then we shard.
  EXPECT_TRUE(options()->domain_lawyer()->AddShard(
      "cdn.com", "shard0.com,shard1.com", &message_handler_));
  // shard0 is always selected in the test because of our mock hasher
  // that always returns 0.
  ValidateExpected("extend_if_sharded_and_rewritten",
                   GenerateHtml(kCssFile, "b.jpg", "c.js"),
                   GenerateHtml(
                       "http://shard0.com/sub/a.css,qv=1.pagespeed.ce.0.css",
                       "http://shard0.com/b.jpg.pagespeed.ce.0.jpg",
                       "http://shard0.com/c.js.pagespeed.ce.0.js"));
}

// TODO(jmarantz): consider implementing and testing the sharding and
// domain-rewriting of uncacheable resources -- just don't sign the URLs.

TEST_P(CacheExtenderTest, NoExtendOriginUncacheable) {
  InitTest(0);  // origin not cacheable
  ValidateNoChanges("no_extend_origin_not_cacheable",
                    GenerateHtml(kCssFile, "b.jpg", "c.js"));
}

TEST_P(CacheExtenderTest, ServeFiles) {
  GoogleString content;

  InitTest(kShortTtlSec);
  ASSERT_TRUE(ServeResource(kTestDomain, kFilterId, kCssFile, "css", &content));
  EXPECT_EQ(kCssData, content);  // no absolutification
  ASSERT_TRUE(ServeResource(kTestDomain, kFilterId, "b.jpg", "jpg", &content));
  EXPECT_EQ(GoogleString(kImageData), content);
  ASSERT_TRUE(ServeResource(kTestDomain, kFilterId, "c.js", "js", &content));
  EXPECT_EQ(GoogleString(kJsData), content);
}

TEST_P(CacheExtenderTest, ConsistentHashWithRewrite) {
  // Since CacheExtend is an on-the-fly filter, ServeFilesWithRewrite, above,
  // verifies that we can decode a cache-extended CSS file and properly
  // domain-rewrite embedded images.  However, we go through the exercise
  // of generating the rewritten content in the HTML path too -- we just
  // don't cache it.  However, what we must do is generate the correct hash
  // code.  To test that we need to use the real hasher.
  UseMd5Hasher();
  DomainLawyer* lawyer = options()->domain_lawyer();
  lawyer->AddRewriteDomainMapping(kNewDomain, kTestDomain, &message_handler_);
  InitTest(kShortTtlSec);

  // First do the HTML rewrite.
  const char kHash[] = "UfiC1QHcaF";
  GoogleString extended_css = Encode(kNewDomain, "ce", kHash, kCssFile, "css");
  ValidateExpected("consistent_hash",
                   StringPrintf(kCssFormat, kCssFile),
                   StringPrintf(kCssFormat, extended_css.c_str()));

  // Note that the only output that gets cached is the MetaData insert, not
  // the rewritten content, because this is an on-the-fly filter and we
  // elect not to add cache pressure. We do of course also cache the original,
  // and under traditional flow also get it from the cache.
  EXPECT_EQ(2, lru_cache()->num_inserts());
  if (rewrite_driver()->asynchronous_rewrites()) {
    EXPECT_EQ(0, lru_cache()->num_hits());
  } else {
    EXPECT_EQ(1, lru_cache()->num_hits());
  }

  // TODO(jmarantz): To make this test pass we need to set up the mock
  // fetcher so it can find the resource in new.com, not just
  // test.com.  Functionally, this wouldn't be needed with a
  // functional installation where both test.com and new.com are the
  // same physical server.  However it does indicate that we are going
  // to fetch the resource using its original resolved name while
  // rewriting HTML, but then when we serve the cache-extended
  // resource we will not have it in our cache; we will have to fetch
  // it again using the new name.  We ought to be canonicalizing the
  // URLs we write into the cache so we don't need this.  This also
  // applies to sharding.
  InitResponseHeaders(StrCat(kNewDomain, kCssFile), kContentTypeCss,
                      kCssData, kShortTtlSec);

  // Now serve the resource, as in ServeFilesWithRewrite above.
  GoogleString content;
  ASSERT_TRUE(ServeResourceUrl(extended_css, &content));
  EXPECT_EQ(kCssData, content);
  EXPECT_EQ(kHash, hasher()->Hash(content));
}

TEST_P(CacheExtenderTest, ConsistentHashWithShard) {
  // Similar to ConsistentHashWithRewrite, except that we've added sharding,
  // and the shard computed for the embedded image is (luckily for the test)
  // different than that for the .css file, thus the references within the
  // css file are rewritten as absolute.
  UseMd5Hasher();
  DomainLawyer* lawyer = options()->domain_lawyer();
  lawyer->AddRewriteDomainMapping(kNewDomain, kTestDomain, &message_handler_);
  lawyer->AddShard(kNewDomain, "shard1.com,shard2.com", &message_handler_);
  InitTest(kShortTtlSec);

  // First do the HTML rewrite.
  const char kHash[] = "MnXHB3ChUY";
  GoogleString extended_css = Encode("http://shard2.com/", "ce", kHash,
                                     kCssFile, "css");
  ValidateExpected("consistent_hash",
                   StringPrintf(kCssFormat, kCssFile),
                   StringPrintf(kCssFormat, extended_css.c_str()));

  // Note that the only output that gets cached is the MetaData insert, not
  // the rewritten content, because this is an on-the-fly filter and we
  // elect not to add cache pressure. We do of course also cache the original,
  // and under traditional flow also get it from the cache.
  EXPECT_EQ(2, lru_cache()->num_inserts());
  if (rewrite_driver()->asynchronous_rewrites()) {
    EXPECT_EQ(0, lru_cache()->num_hits());
  } else {
    EXPECT_EQ(1, lru_cache()->num_hits());
  }

  // TODO(jmarantz): eliminate this when we canonicalize URLs before caching.
  InitResponseHeaders(StrCat("http://shard2.com/", kCssFile), kContentTypeCss,
                      kCssData, kShortTtlSec);

  // Now serve the resource, as in ServeFilesWithRewrite above.
  GoogleString content;
  ASSERT_TRUE(ServeResourceUrl(extended_css, &content));

  // Note that, through the luck of hashes, we've sharded the embedded
  // image differently than the css file.
  EXPECT_EQ(CssData("http://shard1.com/sub/"), content);
  EXPECT_EQ(kHash, hasher()->Hash(content));
}

TEST_P(CacheExtenderTest, ServeFilesWithRewriteDomainsEnabled) {
  GoogleString content;
  DomainLawyer* lawyer = options()->domain_lawyer();
  lawyer->AddRewriteDomainMapping(kNewDomain, kTestDomain, &message_handler_);
  InitTest(kShortTtlSec);
  ASSERT_TRUE(ServeResource(kTestDomain, kFilterId, kCssFile, "css", &content));
  EXPECT_EQ(CssData("http://new.com/sub/"), content);
}

TEST_P(CacheExtenderTest, ServeFilesWithShard) {
  GoogleString content;
  DomainLawyer* lawyer = options()->domain_lawyer();
  lawyer->AddRewriteDomainMapping(kNewDomain, kTestDomain, &message_handler_);
  lawyer->AddShard(kNewDomain, "shard1.com,shard2.com", &message_handler_);
  InitTest(kShortTtlSec);
  ASSERT_TRUE(ServeResource(kTestDomain, kFilterId, kCssFile, "css", &content));
  EXPECT_EQ(CssData("http://shard1.com/sub/"), content);
}

TEST_P(CacheExtenderTest, ServeFilesFromDelayedFetch) {
  InitTest(kShortTtlSec);
  ServeResourceFromManyContexts(Encode(kTestDomain, "ce", "0", kCssFile, "css"),
                                kCssData);
  ServeResourceFromManyContexts(Encode(kTestDomain, "ce", "0", "b.jpg", "jpg"),
                                kImageData);
  ServeResourceFromManyContexts(Encode(kTestDomain, "ce", "0", "c.js", "js"),
                                kJsData);

  // TODO(jmarantz): make ServeResourceFromManyContexts check:
  //  1. Gets the data from the cache, with no mock fetchers, null file system
  //  2. Gets the data from the file system, with no cache, no mock fetchers.
  //  3. Gets the data from the mock fetchers: no cache, no file system.
}

TEST_P(CacheExtenderTest, MinimizeCacheHits) {
  options()->EnableFilter(RewriteOptions::kOutlineCss);
  options()->EnableFilter(RewriteOptions::kExtendCache);
  options()->set_css_outline_min_bytes(1);
  rewrite_driver()->AddFilters();
  GoogleString html_input = StrCat("<style>", kCssData, "</style>");
  GoogleString html_output = StringPrintf(
      "<link rel=\"stylesheet\" href=\"%s\">",
      Encode(kTestDomain, CssOutlineFilter::kFilterId, "0", "_",
             "css").c_str());
  ValidateExpected("no_extend_origin_not_cacheable", html_input, html_output);

  // The key thing about this test is that the CacheExtendFilter should
  // not pound the cache looking to see if it's already rewritten this
  // resource.  If we try, in the cache extend filter, to this already-optimized
  // resource from the cache, then we'll get a cache-hit and decide that
  // it's already got a long cache lifetime.  But we should know, just from
  // the name of the resource, that it should not be cache extended.
  // The CSS outliner also should not produce any cache misses, as it currently
  // does not cache.
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_misses());
}

TEST_P(CacheExtenderTest, NoExtensionCorruption) {
  TestCorruptUrl("%22", false);
}

TEST_P(CacheExtenderTest, NoQueryCorruption) {
  TestCorruptUrl("?query", true);
}

TEST_P(CacheExtenderTest, MadeOnTheFly) {
  // Make sure our fetches go through on-the-fly construction and not the cache.
  InitTest(kMediumTtlSec);

  GoogleString b_ext = Encode(kTestDomain, "ce", "0", "b.jpg", "jpg");
  ValidateExpected("and_img", "<img src=\"b.jpg\">",
                   StrCat("<img src=\"", b_ext, "\">"));

  EXPECT_EQ(0, resource_manager()->cached_resource_fetches()->Get());
  EXPECT_EQ(0, resource_manager()->succeeded_filter_resource_fetches()->Get());
  GoogleString out;
  EXPECT_TRUE(ServeResourceUrl(b_ext, &out));
  EXPECT_EQ(0, resource_manager()->cached_resource_fetches()->Get());
  EXPECT_EQ(1, resource_manager()->succeeded_filter_resource_fetches()->Get());
}

// We test with asynchronous_rewrites() == GetParam() as both true and false.
INSTANTIATE_TEST_CASE_P(CacheExtenderTestInstance,
                        CacheExtenderTest,
                        ::testing::Bool());

}  // namespace

}  // namespace net_instaweb
