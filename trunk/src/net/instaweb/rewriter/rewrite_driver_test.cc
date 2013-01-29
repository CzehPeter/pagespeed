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

// Author: jmarantz@google.com (Joshua D. Marantz)

#include "net/instaweb/rewriter/public/rewrite_driver.h"

#include "net/instaweb/htmlparse/html_event.h"
#include "net/instaweb/htmlparse/html_testing_peer.h"
#include "net/instaweb/htmlparse/public/empty_html_filter.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/counting_url_async_fetcher.h"
#include "net/instaweb/http/public/fake_url_async_fetcher.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/mock_url_fetcher.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/user_agent_matcher.h"
#include "net/instaweb/http/public/user_agent_matcher_test.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/file_load_policy.h"
#include "net/instaweb/rewriter/public/mock_resource_callback.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/single_rewrite_context.h"
#include "net/instaweb/rewriter/public/test_rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/test_url_namer.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/mock_property_page.h"
#include "net/instaweb/util/public/mock_scheduler.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/scheduler.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/worker_test_base.h"

namespace net_instaweb {

class RewriteFilter;

class RewriteDriverTest : public RewriteTestBase {
 protected:
  RewriteDriverTest() {}

  bool CanDecodeUrl(const StringPiece& url) {
    GoogleUrl gurl(url);
    RewriteFilter* filter;
    OutputResourcePtr resource(
        rewrite_driver()->DecodeOutputResource(gurl, &filter));
    return (resource.get() != NULL);
  }

  GoogleString BaseUrlSpec() {
    return rewrite_driver()->base_url().Spec().as_string();
  }

  // A helper to call ComputeCurrentFlushWindowRewriteDelayMs() that allows
  // us to keep it private.
  int64 GetFlushTimeout() {
    return rewrite_driver()->ComputeCurrentFlushWindowRewriteDelayMs();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(RewriteDriverTest);
};

TEST_F(RewriteDriverTest, NoChanges) {
  ValidateNoChanges("no_changes",
                    "<head><script src=\"foo.js\"></script></head>"
                    "<body><form method=\"post\">"
                    "<input type=\"checkbox\" checked>"
                    "</form></body>");
}

TEST_F(RewriteDriverTest, TestLegacyUrl) {
  rewrite_driver()->AddFilters();
  EXPECT_FALSE(CanDecodeUrl("http://example.com/dir/123/jm.0.orig"))
      << "not enough dots";
  EXPECT_TRUE(CanDecodeUrl("http://example.com/dir/123/jm.0.orig.js"));
  EXPECT_TRUE(CanDecodeUrl(
      "http://x.com/dir/123/jm.0123456789abcdef0123456789ABCDEF.orig.js"));
  EXPECT_FALSE(CanDecodeUrl("http://example.com/dir/123/xx.0.orig.js"))
      << "invalid filter xx";
  ASSERT_FALSE(CanDecodeUrl("http://example.com/dir/123/jm.z.orig.js"))
      << "invalid hash code -- not hex";
  ASSERT_FALSE(CanDecodeUrl("http://example.com/dir/123/jm.ab.orig.js"))
      << "invalid hash code -- not 1 or 32 chars";
  ASSERT_FALSE(CanDecodeUrl("http://example.com/dir/123/jm.0.orig.x"))
      << "invalid extension";
}

TEST_F(RewriteDriverTest, TestModernUrl) {
  rewrite_driver()->AddFilters();

  // Sanity-check on a valid one
  EXPECT_TRUE(CanDecodeUrl(
      Encode("http://example.com/", "ce", "HASH", "Puzzle.jpg", "jpg")));

  // Query is OK, too.
  EXPECT_TRUE(CanDecodeUrl(
      StrCat(Encode("http://example.com/", "ce", "HASH", "Puzzle.jpg", "jpg"),
             "?s=ok")));

  // Invalid filter code
  EXPECT_FALSE(CanDecodeUrl(
      Encode("http://example.com/", "NOFILTER", "HASH", "Puzzle.jpg", "jpg")));

  // Nonsense extension -- we will just ignore it these days.
  EXPECT_TRUE(CanDecodeUrl(
      Encode("http://example.com/", "ce", "HASH", "Puzzle.jpg", "jpgif")));

  // No hash
  GoogleString encoded_url(Encode("http://example.com/", "ce", "123456789",
                                  "Puzzle.jpg", "jpg"));
  GlobalReplaceSubstring("123456789", "", &encoded_url);
  EXPECT_FALSE(CanDecodeUrl(encoded_url));
}

class RewriteDriverTestUrlNamer : public RewriteDriverTest {
 public:
  RewriteDriverTestUrlNamer() {
    SetUseTestUrlNamer(true);
  }
};

TEST_F(RewriteDriverTestUrlNamer, TestEncodedUrls) {
  rewrite_driver()->AddFilters();

  // Sanity-check on a valid one
  EXPECT_TRUE(CanDecodeUrl(
      Encode("http://example.com/", "ce", "HASH", "Puzzle.jpg", "jpg")));

  // Query is OK, too.
  EXPECT_TRUE(CanDecodeUrl(
      StrCat(Encode("http://example.com/", "ce", "HASH", "Puzzle.jpg", "jpg"),
             "?s=ok")));

  // Invalid filter code
  EXPECT_FALSE(CanDecodeUrl(
      Encode("http://example.com/", "NOFILTER", "HASH", "Puzzle.jpg", "jpg")));

  // Nonsense extension -- we will just ignore it these days.
  EXPECT_TRUE(CanDecodeUrl(
      Encode("http://example.com/", "ce", "HASH", "Puzzle.jpg", "jpgif")));

  // No hash
  GoogleString encoded_url(Encode("http://example.com/", "ce", "123456789",
                                  "Puzzle.jpg", "jpg"));
  GlobalReplaceSubstring("123456789", "", &encoded_url);
  EXPECT_FALSE(CanDecodeUrl(encoded_url));

  // Valid proxy domain but invalid decoded URL.
  encoded_url = Encode("http://example.com/", "ce", "0", "Puzzle.jpg", "jpg");
  GlobalReplaceSubstring("example.com/",
                         "example.comWYTHQ000JRJFCAAKYU1EMA6VUBDTS4DESLRWIPMS"
                         "KKMQH0XYN1FURDBBSQ9AYXVX3TZDKZEIJNLRHU05ATHBAWWAG2+"
                         "ADDCXPWGGP1VTHJIYU13IIFQYSYMGKIMSFIEBM+HCAACVNGO8CX"
                         "XO%81%9F%F1m/", &encoded_url);
  // By default TestUrlNamer doesn't proxy but we need it to for this test.
  TestUrlNamer::SetProxyMode(true);
  EXPECT_FALSE(CanDecodeUrl(encoded_url));
}

TEST_F(RewriteDriverTestUrlNamer, TestDecodeUrls) {
  // Sanity-check on a valid one
  GoogleUrl gurl_good(Encode(
      "http://example.com/", "ce", "HASH", "Puzzle.jpg", "jpg"));
  rewrite_driver()->AddFilters();
  StringVector urls;
  TestUrlNamer::SetProxyMode(true);
  EXPECT_TRUE(rewrite_driver()->DecodeUrl(gurl_good, &urls));
  EXPECT_EQ(1, urls.size());
  EXPECT_EQ("http://example.com/Puzzle.jpg", urls[0]);

  // Invalid filter code
  urls.clear();
  GoogleUrl gurl_bad(Encode(
      "http://example.com/", "NOFILTER", "HASH", "Puzzle.jpg", "jpgif"));
  EXPECT_FALSE(rewrite_driver()->DecodeUrl(gurl_bad, &urls));

  // Combine filters
  urls.clear();
  GoogleUrl gurl_multi(Encode(
      "http://example.com/", "cc", "HASH", MultiUrl("a.css", "b.css"), "css"));
  EXPECT_TRUE(rewrite_driver()->DecodeUrl(gurl_multi, &urls));
  EXPECT_EQ(2, urls.size());
  EXPECT_EQ("http://example.com/a.css", urls[0]);
  EXPECT_EQ("http://example.com/b.css", urls[1]);

  // Invalid Url.
  urls.clear();
  GoogleUrl gurl_invalid("invalid url");
  EXPECT_FALSE(rewrite_driver()->DecodeUrl(gurl_invalid, &urls));
  EXPECT_EQ(0, urls.size());

  // ProxyMode off
  urls.clear();
  TestUrlNamer::SetProxyMode(false);
  SetUseTestUrlNamer(false);
  gurl_good.Reset(Encode(
      "http://example.com/", "ce", "HASH", "Puzzle.jpg", "jpg"));
  EXPECT_TRUE(rewrite_driver()->DecodeUrl(gurl_good, &urls));
  EXPECT_EQ(1, urls.size());
  EXPECT_EQ("http://example.com/Puzzle.jpg", urls[0]);

  urls.clear();
  gurl_multi.Reset(Encode(
      "http://example.com/", "cc", "HASH", MultiUrl("a.css", "b.css"), "css"));
  EXPECT_TRUE(rewrite_driver()->DecodeUrl(gurl_multi, &urls));
  EXPECT_EQ(2, urls.size());
  EXPECT_EQ("http://example.com/a.css", urls[0]);
  EXPECT_EQ("http://example.com/b.css", urls[1]);
}

// Test to make sure we do not put in extra things into the cache.
// This is using the CSS rewriter, which caches the output.
TEST_F(RewriteDriverTest, TestCacheUse) {
  AddFilter(RewriteOptions::kRewriteCss);

  const char kCss[] = "* { display: none; }";
  const char kMinCss[] = "*{display:none}";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);

  GoogleString css_minified_url =
      Encode(kTestDomain, RewriteOptions::kCssFilterId,
             hasher()->Hash(kMinCss), "a.css", "css");

  // Cold load.
  EXPECT_TRUE(TryFetchResource(css_minified_url));

  // We should have 3 things inserted:
  // 1) the source data
  // 2) the result
  // 3) the rname entry for the result
  int cold_num_inserts = lru_cache()->num_inserts();
  EXPECT_EQ(3, cold_num_inserts);

  // Warm load. This one should not change the number of inserts at all
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  EXPECT_EQ(cold_num_inserts, lru_cache()->num_inserts());
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());
}

// Extension of above with cache invalidation.
TEST_F(RewriteDriverTest, TestCacheUseWithInvalidation) {
  AddFilter(RewriteOptions::kRewriteCss);

  const char kCss[] = "* { display: none; }";
  const char kMinCss[] = "*{display:none}";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);

  GoogleString css_minified_url =
      Encode(kTestDomain, RewriteOptions::kCssFilterId,
             hasher()->Hash(kMinCss), "a.css", "css");

  // Cold load.
  EXPECT_TRUE(TryFetchResource(css_minified_url));

  // We should have 3 things inserted:
  // 1) the source data
  // 2) the result
  // 3) the rname entry for the result.
  int cold_num_inserts = lru_cache()->num_inserts();
  EXPECT_EQ(3, cold_num_inserts);

  // Warm load. This one should not change the number of inserts at all
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  EXPECT_EQ(cold_num_inserts, lru_cache()->num_inserts());
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());

  // Set cache invalidation timestamp (to now, so that response date header is
  // in the "past") and load. Should get inserted again.
  ClearStats();
  int64 now_ms = timer()->NowMs();
  options()->ClearSignatureForTesting();
  options()->set_cache_invalidation_timestamp(now_ms);
  options()->ComputeSignature(hasher());
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  // We expect: identical input a new rname entry (its version # changed),
  // and the output which may not may not auto-advance due to MockTimer
  // black magic.
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_identical_reinserts());
}

TEST_F(RewriteDriverTest, TestCacheUseWithUrlPatternAllInvalidation) {
  AddFilter(RewriteOptions::kRewriteCss);

  const char kCss[] = "* { display: none; }";
  const char kMinCss[] = "*{display:none}";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);

  GoogleString css_minified_url =
      Encode(kTestDomain, RewriteOptions::kCssFilterId,
             hasher()->Hash(kMinCss), "a.css", "css");

  // Cold load.
  EXPECT_TRUE(TryFetchResource(css_minified_url));

  // We should have 3 things inserted:
  // 1) the source data
  // 2) the result
  // 3) the rname entry for the result.
  int cold_num_inserts = lru_cache()->num_inserts();
  EXPECT_EQ(3, cold_num_inserts);

  // Warm load. This one should not change the number of inserts at all
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  EXPECT_EQ(cold_num_inserts, lru_cache()->num_inserts());
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());

  ClearStats();
  int64 now_ms = timer()->NowMs();
  options()->ClearSignatureForTesting();
  // Set cache invalidation (to now) for all URLs with "a.css" and also
  // invalidate all metadata (the last 'false' argument below).
  options()->AddUrlCacheInvalidationEntry("*a.css*", now_ms, false);
  options()->ComputeSignature(hasher());
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  // We expect: identical input, a new rewrite entry (its version # changed),
  // and the output which may not may not auto-advance due to MockTimer black
  // magic.
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_identical_reinserts());
}

TEST_F(RewriteDriverTest, TestCacheUseWithUrlPatternOnlyInvalidation) {
  AddFilter(RewriteOptions::kRewriteCss);

  const char kCss[] = "* { display: none; }";
  const char kMinCss[] = "*{display:none}";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);

  GoogleString css_minified_url =
      Encode(kTestDomain, RewriteOptions::kCssFilterId,
             hasher()->Hash(kMinCss), "a.css", "css");

  // Cold load.
  EXPECT_TRUE(TryFetchResource(css_minified_url));

  // We should have 3 things inserted:
  // 1) the source data
  // 2) the result
  // 3) the rname entry for the result.
  int cold_num_inserts = lru_cache()->num_inserts();
  EXPECT_EQ(3, cold_num_inserts);

  // Warm load. This one should not change the number of inserts at all
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  EXPECT_EQ(cold_num_inserts, lru_cache()->num_inserts());
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());

  ClearStats();
  int64 now_ms = timer()->NowMs();
  options()->ClearSignatureForTesting();
  // Set cache invalidation (to now) for all URLs with "a.css". Does not
  // invalidate any metadata (the last 'true' argument below).
  options()->AddUrlCacheInvalidationEntry("*a.css*", now_ms, true);
  options()->ComputeSignature(hasher());
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  // The output rewritten URL is invalidated, the input is also invalidated, and
  // fetched again.  The rewrite entry does not change, and gets reinserted.
  // Thus, we have identical input, rname entry, and the output.
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(3, lru_cache()->num_identical_reinserts());
}

TEST_F(RewriteDriverTest, TestCacheUseWithRewrittenUrlAllInvalidation) {
  AddFilter(RewriteOptions::kRewriteCss);

  const char kCss[] = "* { display: none; }";
  const char kMinCss[] = "*{display:none}";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);

  GoogleString css_minified_url =
      Encode(kTestDomain, RewriteOptions::kCssFilterId,
             hasher()->Hash(kMinCss), "a.css", "css");

  // Cold load.
  EXPECT_TRUE(TryFetchResource(css_minified_url));

  // We should have 3 things inserted:
  // 1) the source data
  // 2) the result
  // 3) the rname entry for the result.
  int cold_num_inserts = lru_cache()->num_inserts();
  EXPECT_EQ(3, cold_num_inserts);

  // Warm load. This one should not change the number of inserts at all
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  EXPECT_EQ(cold_num_inserts, lru_cache()->num_inserts());
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());

  ClearStats();
  int64 now_ms = timer()->NowMs();
  options()->ClearSignatureForTesting();
  // Set a URL cache invalidation entry for output URL.  Original input URL is
  // not affected.  Also invalidate all metadata (the last 'false' argument
  // below).
  options()->AddUrlCacheInvalidationEntry(css_minified_url, now_ms, false);
  options()->ComputeSignature(hasher());
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  // We expect:  a new rewrite entry (its version # changed), and identical
  // output.
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_identical_reinserts());
}

TEST_F(RewriteDriverTest, TestCacheUseWithRewrittenUrlOnlyInvalidation) {
  AddFilter(RewriteOptions::kRewriteCss);

  const char kCss[] = "* { display: none; }";
  const char kMinCss[] = "*{display:none}";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);

  GoogleString css_minified_url =
      Encode(kTestDomain, RewriteOptions::kCssFilterId,
             hasher()->Hash(kMinCss), "a.css", "css");

  // Cold load.
  EXPECT_TRUE(TryFetchResource(css_minified_url));

  // We should have 3 things inserted:
  // 1) the source data
  // 2) the result
  // 3) the rname entry for the result.
  int cold_num_inserts = lru_cache()->num_inserts();
  EXPECT_EQ(3, cold_num_inserts);

  // Warm load. This one should not change the number of inserts at all
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  EXPECT_EQ(cold_num_inserts, lru_cache()->num_inserts());
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());

  ClearStats();
  int64 now_ms = timer()->NowMs();
  options()->ClearSignatureForTesting();
  // Set cache invalidation (to now) for output URL.  Original input URL is not
  // affected.  Does not invalidate any metadata (the last 'true' argument
  // below).
  options()->AddUrlCacheInvalidationEntry(css_minified_url, now_ms, true);
  options()->ComputeSignature(hasher());
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  // We expect:  identical rewrite entry and output.
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_identical_reinserts());
}

TEST_F(RewriteDriverTest, TestCacheUseWithOriginalUrlInvalidation) {
  AddFilter(RewriteOptions::kRewriteCss);

  const char kCss[] = "* { display: none; }";
  const char kMinCss[] = "*{display:none}";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);

  GoogleString css_minified_url =
      Encode(kTestDomain, RewriteOptions::kCssFilterId,
             hasher()->Hash(kMinCss), "a.css", "css");

  // Cold load.
  EXPECT_TRUE(TryFetchResource(css_minified_url));

  // We should have 3 things inserted:
  // 1) the source data
  // 2) the result
  // 3) the rname entry for the result.
  int cold_num_inserts = lru_cache()->num_inserts();
  EXPECT_EQ(3, cold_num_inserts);

  // Warm load. This one should not change the number of inserts at all
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  EXPECT_EQ(cold_num_inserts, lru_cache()->num_inserts());
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());

  ClearStats();
  int64 now_ms = timer()->NowMs();
  options()->ClearSignatureForTesting();
  // Set cache invalidation (to now) for input URL.  Rewritten output URL is not
  // affected.  So there will be no cache inserts or reinserts.
  // Note:  Whether we invalidate all metadata (the last argument below) is
  // immaterial in this test.
  options()->AddUrlCacheInvalidationEntry("http://test.com/a.css", now_ms,
                                          false);
  options()->ComputeSignature(hasher());
  EXPECT_TRUE(TryFetchResource(css_minified_url));
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, lru_cache()->num_identical_reinserts());
}

// Similar to TestCacheUse, but with cache-extender which reconstructs on the
// fly.
TEST_F(RewriteDriverTest, TestCacheUseOnTheFly) {
  AddFilter(RewriteOptions::kExtendCacheCss);

  const char kCss[] = "* { display: none; }";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);

  GoogleString cache_extended_url =
      Encode(kTestDomain, RewriteOptions::kCacheExtenderId,
             hasher()->Hash(kCss), "a.css", "css");

  // Cold load.
  EXPECT_TRUE(TryFetchResource(cache_extended_url));

  // We should have 2 things inserted:
  // 1) the source data
  // 2) the rname entry for the result (only in sync)
  int cold_num_inserts = lru_cache()->num_inserts();
  EXPECT_EQ(2, cold_num_inserts);

  // Warm load. This one re-inserts in the rname entry, without changing it.
  EXPECT_TRUE(TryFetchResource(cache_extended_url));
  EXPECT_EQ(cold_num_inserts, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_identical_reinserts());
}

// Verifies that the computed rewrite delay agrees with expectations
// depending on the configuration of constituent delay variables.
TEST_F(RewriteDriverTest, TestComputeCurrentFlushWindowRewriteDelayMs) {
  rewrite_driver()->set_rewrite_deadline_ms(1000);

  // "Start" a parse to configure the start time in the driver.
  ASSERT_TRUE(rewrite_driver()->StartParseId("http://site.com/",
                                             "compute_flush_window_test",
                                             kContentTypeHtml));

  // The per-page deadline is initially unconfigured.
  EXPECT_EQ(1000, GetFlushTimeout());

  // If the per-page deadline is less than the per-flush window timeout,
  // the per-page deadline is returned.
  rewrite_driver()->set_max_page_processing_delay_ms(500);
  EXPECT_EQ(500, GetFlushTimeout());

  // If the per-page deadline exceeds the per-flush window timeout, the flush
  // timeout is returned.
  rewrite_driver()->set_max_page_processing_delay_ms(1750);
  EXPECT_EQ(1000, GetFlushTimeout());

  // If we advance mock time to leave less than a flush window timeout remaining
  // against the page deadline, the appropriate page deadline difference is
  // returned.
  SetTimeMs(start_time_ms() + 1000);
  EXPECT_EQ(750, GetFlushTimeout());  // 1750 - 1000

  // If we advance mock time beyond the per-page limit, a value of 1 is
  // returned. (This is required since values <= 0 are interpreted by internal
  // timeout functions as unlimited.)
  SetTimeMs(start_time_ms() + 2000);
  EXPECT_EQ(1, GetFlushTimeout());

  rewrite_driver()->FinishParse();
}

// Extension of above with cache invalidation.
TEST_F(RewriteDriverTest, TestCacheUseOnTheFlyWithInvalidation) {
  AddFilter(RewriteOptions::kExtendCacheCss);

  const char kCss[] = "* { display: none; }";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);

  GoogleString cache_extended_url =
      Encode(kTestDomain, RewriteOptions::kCacheExtenderId,
             hasher()->Hash(kCss), "a.css", "css");

  // Cold load.
  EXPECT_TRUE(TryFetchResource(cache_extended_url));

  // We should have 2 things inserted:
  // 1) the source data
  // 2) the rname entry for the result
  int cold_num_inserts = lru_cache()->num_inserts();
  EXPECT_EQ(2, cold_num_inserts);

  // Warm load. This one re-inserts in the rname entry, without changing it.
  EXPECT_TRUE(TryFetchResource(cache_extended_url));
  EXPECT_EQ(cold_num_inserts, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_identical_reinserts());

  // Set cache invalidation timestamp (to now, so that response date header is
  // in the "past") and load.
  ClearStats();
  int64 now_ms = timer()->NowMs();
  options()->ClearSignatureForTesting();
  options()->set_cache_invalidation_timestamp(now_ms);
  options()->ComputeSignature(hasher());
  EXPECT_TRUE(TryFetchResource(cache_extended_url));
  // We expect: input re-insert, new metadata key
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_identical_reinserts());
}

TEST_F(RewriteDriverTest, BaseTags) {
  // Starting the parse, the base-tag will be derived from the html url.
  ASSERT_TRUE(rewrite_driver()->StartParse("http://example.com/index.html"));
  rewrite_driver()->Flush();
  EXPECT_EQ("http://example.com/index.html", BaseUrlSpec());

  // If we then encounter a base tag, that will become the new base.
  rewrite_driver()->ParseText("<base href='http://new.example.com/subdir/'>");
  rewrite_driver()->Flush();
  EXPECT_EQ(0, message_handler()->TotalMessages());
  EXPECT_EQ("http://new.example.com/subdir/", BaseUrlSpec());

  // A second base tag will be ignored, and an info message will be printed.
  rewrite_driver()->ParseText("<base href=http://second.example.com/subdir2>");
  rewrite_driver()->Flush();
  EXPECT_EQ(1, message_handler()->TotalMessages());
  EXPECT_EQ("http://new.example.com/subdir/", BaseUrlSpec());

  // Restart the parse with a new URL and we start fresh.
  rewrite_driver()->FinishParse();
  ASSERT_TRUE(rewrite_driver()->StartParse(
      "http://restart.example.com/index.html"));
  rewrite_driver()->Flush();
  EXPECT_EQ("http://restart.example.com/index.html", BaseUrlSpec());

  // We should be able to reset again.
  rewrite_driver()->ParseText("<base href='http://new.example.com/subdir/'>");
  rewrite_driver()->Flush();
  EXPECT_EQ(1, message_handler()->TotalMessages());
  EXPECT_EQ("http://new.example.com/subdir/", BaseUrlSpec());
}

TEST_F(RewriteDriverTest, RelativeBaseTag) {
  // Starting the parse, the base-tag will be derived from the html url.
  ASSERT_TRUE(rewrite_driver()->StartParse("http://example.com/index.html"));
  rewrite_driver()->ParseText("<base href='subdir/'>");
  rewrite_driver()->Flush();
  EXPECT_EQ(0, message_handler()->TotalMessages());
  EXPECT_EQ("http://example.com/subdir/", BaseUrlSpec());
}

TEST_F(RewriteDriverTest, InvalidBaseTag) {
  // Encountering an invalid base tag should be ignored (except info message).
  ASSERT_TRUE(rewrite_driver()->StartParse("slwly://example.com/index.html"));
  rewrite_driver()->ParseText("<base href='subdir_not_allowed_on_slwly/'>");
  rewrite_driver()->Flush();

  EXPECT_EQ(1, message_handler()->TotalMessages());
  EXPECT_EQ("slwly://example.com/index.html", BaseUrlSpec());

  // And we will accept a subsequent base-tag with legal aboslute syntax.
  rewrite_driver()->ParseText("<base href='http://example.com/absolute/'>");
  rewrite_driver()->Flush();
  EXPECT_EQ("http://example.com/absolute/", BaseUrlSpec());
}

TEST_F(RewriteDriverTest, CreateOutputResourceTooLong) {
  const OutputResourceKind resource_kinds[] = {
    kRewrittenResource,
    kOnTheFlyResource,
    kOutlinedResource,
  };

  // short_path.size() < options()->max_url_size() < long_path.size()
  GoogleString short_path = "http://www.example.com/dir/";
  GoogleString long_path = short_path;
  for (int i = 0; 2 * i < options()->max_url_size(); ++i) {
    long_path += "z/";
  }

  // short_name.size() < options()->max_url_segment_size() < long_name.size()
  GoogleString short_name = "foo.html";
  GoogleString long_name =
      StrCat("foo.html?",
             GoogleString(options()->max_url_segment_size() + 1, 'z'));

  GoogleString dummy_filter_id = "xy";

  OutputResourcePtr resource;
  for (int k = 0; k < arraysize(resource_kinds); ++k) {
    // Short name should always succeed at creating new resource.
    resource.reset(rewrite_driver()->CreateOutputResourceWithPath(
        short_path, dummy_filter_id, short_name, resource_kinds[k]));
    EXPECT_TRUE(NULL != resource.get());

    // Long leaf-name should always fail at creating new resource.
    resource.reset(rewrite_driver()->CreateOutputResourceWithPath(
        short_path, dummy_filter_id, long_name, resource_kinds[k]));
    EXPECT_TRUE(NULL == resource.get());

    // Long total URL length should always fail at creating new resource.
    resource.reset(rewrite_driver()->CreateOutputResourceWithPath(
        long_path, dummy_filter_id, short_name, resource_kinds[k]));
    EXPECT_TRUE(NULL == resource.get());
  }
}

TEST_F(RewriteDriverTest, MultipleDomains) {
  rewrite_driver()->AddFilters();

  // Make sure we authorize domains for resources properly. This is a regression
  // test for where loading things from a domain would prevent loads from an
  // another domain from the same RewriteDriver.

  const char kCss[] = "* { display: none; }";
  const char kAltDomain[] = "http://www.example.co.uk/";
  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "a.css"), kContentTypeCss,
                                kCss, 100);
  SetResponseWithDefaultHeaders(StrCat(kAltDomain, "b.css"), kContentTypeCss,
                                kCss, 100);

  GoogleString rewritten1 = Encode(kTestDomain,
                                   RewriteOptions::kCacheExtenderId,
                                   hasher()->Hash(kCss), "a.css", "css");

  GoogleString rewritten2 = Encode(kAltDomain, RewriteOptions::kCacheExtenderId,
                                   hasher()->Hash(kCss), "b.css", "css");

  EXPECT_TRUE(TryFetchResource(rewritten1));
  ClearRewriteDriver();
  EXPECT_TRUE(TryFetchResource(rewritten2));
}

TEST_F(RewriteDriverTest, ResourceCharset) {
  // Make sure we properly pick up the charset into a resource on read.
  const char kUrl[] = "http://www.example.com/foo.css";
  ResponseHeaders resource_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &resource_headers);
  resource_headers.Replace(HttpAttributes::kContentType,
                           "text/css; charset=koi8-r");

  const char kContents[] = "\xF5\xD2\xC1!";  // Ура!
  SetFetchResponse(kUrl, resource_headers, kContents);

  // We do this twice to make sure the cached version is OK, too.
  for (int round = 0; round < 2; ++round) {
    ResourcePtr resource(
        rewrite_driver()->CreateInputResourceAbsoluteUnchecked(kUrl));
    MockResourceCallback mock_callback(resource);
    EXPECT_TRUE(resource.get() != NULL);
    server_context()->ReadAsync(Resource::kReportFailureIfNotCacheable,
                                rewrite_driver()->request_context(),
                                &mock_callback);
    EXPECT_TRUE(mock_callback.done());
    EXPECT_TRUE(mock_callback.success());
    EXPECT_EQ(kContents, resource->contents());
    ASSERT_TRUE(resource->type() != NULL);
    EXPECT_EQ(ContentType::kCss, resource->type()->type());
    EXPECT_STREQ("koi8-r", resource->charset());
  }
}

// Test caching behavior for normal UrlInputResources.
// This is the base case that LoadResourcesFromFiles below contrasts with.
TEST_F(RewriteDriverTest, LoadResourcesFromTheWeb) {
  rewrite_driver()->AddFilters();

  const char kStaticUrlPrefix[] = "http://www.example.com/";
  const char kResourceName[ ]= "foo.css";
  GoogleString resource_url = StrCat(kStaticUrlPrefix, kResourceName);
  const char kResourceContents1[] = "body { background: red; }";
  const char kResourceContents2[] = "body { background: blue; }";
  ResponseHeaders resource_headers;
  // This sets 1 year cache lifetime :/ TODO(sligocki): Shorten this.
  SetDefaultLongCacheHeaders(&kContentTypeCss, &resource_headers);
  // Clear the Etag and Last-Modified headers since this
  // SetDefaultLongCacheHeaders sets their value to constants which don't change
  // when their value is updated.
  resource_headers.RemoveAll(HttpAttributes::kEtag);
  resource_headers.RemoveAll(HttpAttributes::kLastModified);

  // Set the fetch value.
  SetFetchResponse(resource_url, resource_headers, kResourceContents1);
  // Make sure file can be loaded. Note this cannot be loaded through the
  // mock_url_fetcher, because it has not been set in that fetcher.
  ResourcePtr resource(
      rewrite_driver()->CreateInputResourceAbsoluteUnchecked(resource_url));
  MockResourceCallback mock_callback(resource);
  EXPECT_TRUE(resource.get() != NULL);
  server_context()->ReadAsync(Resource::kReportFailureIfNotCacheable,
                              rewrite_driver()->request_context(),
                              &mock_callback);
  EXPECT_TRUE(mock_callback.done());
  EXPECT_TRUE(mock_callback.success());
  EXPECT_EQ(kResourceContents1, resource->contents());
  // TODO(sligocki): Check it was cached.

  // Change the fetch value.
  SetFetchResponse(resource_url, resource_headers, kResourceContents2);
  // Check that the resource loads cached.
  ResourcePtr resource2(
      rewrite_driver()->CreateInputResourceAbsoluteUnchecked(resource_url));
  MockResourceCallback mock_callback2(resource2);
  EXPECT_TRUE(resource2.get() != NULL);
  server_context()->ReadAsync(Resource::kReportFailureIfNotCacheable,
                              rewrite_driver()->request_context(),
                              &mock_callback2);
  EXPECT_TRUE(mock_callback2.done());
  EXPECT_TRUE(mock_callback2.success());
  EXPECT_EQ(kResourceContents1, resource2->contents());

  // Advance timer and check that the resource loads updated.
  AdvanceTimeMs(10 * Timer::kYearMs);

  // Check that the resource loads updated.
  ResourcePtr resource3(
      rewrite_driver()->CreateInputResourceAbsoluteUnchecked(resource_url));
  MockResourceCallback mock_callback3(resource3);
  EXPECT_TRUE(resource3.get() != NULL);
  server_context()->ReadAsync(Resource::kReportFailureIfNotCacheable,
                              rewrite_driver()->request_context(),
                              &mock_callback3);
  EXPECT_TRUE(mock_callback3.done());
  EXPECT_EQ(kResourceContents2, resource3->contents());
}

// Test that we successfully load specified resources from files and that
// file resources have the appropriate properties, such as being loaded from
// file every time they are fetched (not being cached).
TEST_F(RewriteDriverTest, LoadResourcesFromFiles) {
  rewrite_driver()->AddFilters();

  const char kStaticUrlPrefix[] = "http://www.example.com/static/";
  const char kStaticFilenamePrefix[] = "/htmlcontent/static/";
  const char kResourceName[ ]= "foo.css";
  GoogleString resource_filename = StrCat(kStaticFilenamePrefix, kResourceName);
  GoogleString resource_url = StrCat(kStaticUrlPrefix, kResourceName);
  const char kResourceContents1[] = "body { background: red; }";
  const char kResourceContents2[] = "body { background: blue; }";

  // Tell RewriteDriver to associate static URLs with filenames.
  options()->file_load_policy()->Associate(kStaticUrlPrefix,
                                           kStaticFilenamePrefix);

  // Write a file.
  WriteFile(resource_filename.c_str(), kResourceContents1);
  // Make sure file can be loaded. Note this cannot be loaded through the
  // mock_url_fetcher, because it has not been set in that fetcher.
  ResourcePtr resource(
      rewrite_driver()->CreateInputResourceAbsoluteUnchecked(resource_url));
  EXPECT_TRUE(resource.get() != NULL);
  EXPECT_EQ(&kContentTypeCss, resource->type());
  MockResourceCallback mock_callback(resource);
  server_context()->ReadAsync(Resource::kReportFailureIfNotCacheable,
                              rewrite_driver()->request_context(),
                              &mock_callback);
  EXPECT_TRUE(mock_callback.done());
  EXPECT_TRUE(mock_callback.success());
  EXPECT_EQ(kResourceContents1, resource->contents());
  // TODO(sligocki): Check it wasn't cached.

  // Change the file.
  WriteFile(resource_filename.c_str(), kResourceContents2);
  // Make sure the resource loads updated.
  ResourcePtr resource2(
      rewrite_driver()->CreateInputResourceAbsoluteUnchecked(resource_url));
  EXPECT_TRUE(resource2.get() != NULL);
  EXPECT_EQ(&kContentTypeCss, resource2->type());
  MockResourceCallback mock_callback2(resource2);
  server_context()->ReadAsync(Resource::kReportFailureIfNotCacheable,
                              rewrite_driver()->request_context(),
                              &mock_callback2);
  EXPECT_TRUE(mock_callback2.done());
  EXPECT_TRUE(mock_callback2.success());
  EXPECT_EQ(kResourceContents2, resource2->contents());
}

// Make sure the content-type is set correctly, even for URLs with queries.
// http://code.google.com/p/modpagespeed/issues/detail?id=405
TEST_F(RewriteDriverTest, LoadResourcesContentType) {
  rewrite_driver()->AddFilters();

  // Tell RewriteDriver to associate static URLs with filenames.
  options()->file_load_policy()->Associate("http://www.example.com/static/",
                                           "/htmlcontent/static/");

  // Write file with readable extension.
  WriteFile("/htmlcontent/foo.js", "");
  // Load the file with a query param (add .css at the end of the param just
  // for optimal trickyness).
  ResourcePtr resource(rewrite_driver()->CreateInputResourceAbsoluteUnchecked(
      "http://www.example.com/static/foo.js?version=2.css"));
  EXPECT_TRUE(resource.get() != NULL);
  EXPECT_EQ(&kContentTypeJavascript, resource->type());

  // Write file with bogus extension.
  WriteFile("/htmlcontent/bar.bogus", "");
  // Load it normally.
  ResourcePtr resource2(rewrite_driver()->CreateInputResourceAbsoluteUnchecked(
      "http://www.example.com/static/bar.bogus"));
  EXPECT_TRUE(resource2.get() != NULL);
  EXPECT_TRUE(NULL == resource2->type());
}

TEST_F(RewriteDriverTest, ResolveAnchorUrl) {
  rewrite_driver()->AddFilters();
  ASSERT_TRUE(rewrite_driver()->StartParse("http://example.com/index.html"));
  GoogleUrl resolved(rewrite_driver()->base_url(), "#anchor");
  EXPECT_EQ("http://example.com/index.html#anchor", resolved.Spec());
  rewrite_driver()->FinishParse();
}

namespace {

// A rewrite context that's not actually capable of rewriting -- we just need
// one to pass in to InfoAt in test below.
class MockRewriteContext : public SingleRewriteContext {
 public:
  explicit MockRewriteContext(RewriteDriver* driver) :
      SingleRewriteContext(driver, NULL, NULL) {}

  virtual void RewriteSingle(const ResourcePtr& input,
                             const OutputResourcePtr& output) {}
  virtual const char* id() const { return "mock"; }
  virtual OutputResourceKind kind() const { return kOnTheFlyResource; }
};

}  // namespace

TEST_F(RewriteDriverTest, DiagnosticsWithPercent) {
  // Regression test for crash in InfoAt where location has %stuff in it.
  // (make sure it actually shows up first, though).
  int prev_log_level = logging::GetMinLogLevel();
  logging::SetMinLogLevel(logging::LOG_INFO);
  rewrite_driver()->AddFilters();
  MockRewriteContext context(rewrite_driver());
  ResourcePtr resource(rewrite_driver()->CreateInputResourceAbsoluteUnchecked(
      "http://www.example.com/%s%s%s%d%f"));
  ResourceSlotPtr slot(new FetchResourceSlot(resource));
  context.AddSlot(slot);
  rewrite_driver()->InfoAt(&context, "Just a test");
  logging::SetMinLogLevel(prev_log_level);
}

// Tests that we reject https URLs quickly.
TEST_F(RewriteDriverTest, RejectHttpsQuickly) {
  // Need to expressly authorize https even though we don't support it.
  options()->domain_lawyer()->AddDomain("https://*/", message_handler());
  AddFilter(RewriteOptions::kRewriteJavascript);

  // When we don't support https then we fail quickly and cleanly.
  factory()->mock_url_async_fetcher()->set_fetcher_supports_https(false);
  ValidateNoChanges("reject_https_quickly",
                    "<script src='https://example.com/a.js'></script>");
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());

  // When we do support https the fetcher fails to find the resource.
  factory()->mock_url_async_fetcher()->set_fetcher_supports_https(true);
  SetFetchResponse404("https://example.com/a.js");
  ValidateNoChanges("reject_https_quickly",
                    "<script src='https://example.com/a.js'></script>");
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(0, counting_url_async_fetcher()->failure_count());
}

// Test that CreateInputResource doesn't crash when handed a data url.
// This was causing a query of death in some circumstances.
TEST_F(RewriteDriverTest, RejectDataResourceGracefully) {
  MockRewriteContext context(rewrite_driver());
  GoogleUrl dataUrl("data:");
  ResourcePtr resource(rewrite_driver()->CreateInputResource(dataUrl));
  EXPECT_TRUE(resource.get() == NULL);
}

namespace {

class ResponseHeadersCheckingFilter : public EmptyHtmlFilter {
 public:
  explicit ResponseHeadersCheckingFilter(RewriteDriver* driver)
      : driver_(driver),
        flush_occurred_(false) {
  }

  void CheckAccess() {
    EXPECT_TRUE(driver_->response_headers() != NULL);
    if (flush_occurred_) {
      EXPECT_TRUE(driver_->mutable_response_headers() == NULL);
    } else {
      EXPECT_EQ(driver_->mutable_response_headers(),
                driver_->response_headers());
    }
  }

  virtual void StartDocument() {
    flush_occurred_ = false;
    CheckAccess();
  }

  virtual void Flush() {
    CheckAccess();  // We still can access the mutable headers during Flush.
    flush_occurred_ = true;
  }

  virtual void StartElement(HtmlElement* element) { CheckAccess(); }
  virtual void EndElement(HtmlElement* element) { CheckAccess(); }
  virtual void EndDocument() { CheckAccess(); }

  virtual const char* Name() const { return "ResponseHeadersCheckingFilter"; }

 private:
  RewriteDriver* driver_;
  bool flush_occurred_;
};

class DetermineEnabledCheckingFilter : public EmptyHtmlFilter {
 public:
  DetermineEnabledCheckingFilter() :
    start_document_called_(false),
    enabled_value_(false) {}

  virtual void StartDocument() {
    start_document_called_ = true;
  }

  virtual void DetermineEnabled() {
    set_is_enabled(enabled_value_);
  }

  void SetEnabled(bool enabled_value) {
    enabled_value_ = enabled_value;
  }

  bool start_document_called() {
    return start_document_called_;
  }

  virtual const char* Name() const { return "DetermineEnabledCheckingFilter"; }

 private:
  bool start_document_called_;
  bool enabled_value_;
};

}  // namespace

TEST_F(RewriteDriverTest, DetermineEnabledTest) {
  RewriteDriver* driver = rewrite_driver();
  DetermineEnabledCheckingFilter* filter =
      new DetermineEnabledCheckingFilter();
  driver->AddOwnedEarlyPreRenderFilter(filter);
  driver->StartParse("http://example.com/index.html");
  rewrite_driver()->ParseText("<div>");
  driver->Flush();
  EXPECT_FALSE(filter->start_document_called());
  rewrite_driver()->ParseText("</div>");
  driver->FinishParse();

  filter = new DetermineEnabledCheckingFilter();
  filter->SetEnabled(true);
  driver->AddOwnedEarlyPreRenderFilter(filter);
  driver->StartParse("http://example.com/index.html");
  rewrite_driver()->ParseText("<div>");
  driver->Flush();
  EXPECT_TRUE(filter->start_document_called());
  rewrite_driver()->ParseText("</div>");
  driver->FinishParse();
}

// Tests that we access driver->response_headers() before/after Flush(),
// and driver->mutable_response_headers() at only before Flush().
TEST_F(RewriteDriverTest, ResponseHeadersAccess) {
  RewriteDriver* driver = rewrite_driver();
  ResponseHeaders headers;
  driver->set_response_headers_ptr(&headers);
  driver->AddOwnedEarlyPreRenderFilter(new ResponseHeadersCheckingFilter(
      driver));
  driver->AddOwnedPostRenderFilter(new ResponseHeadersCheckingFilter(driver));

  // Starting the parse, the base-tag will be derived from the html url.
  ASSERT_TRUE(driver->StartParse("http://example.com/index.html"));
  rewrite_driver()->ParseText("<div>");
  driver->Flush();
  rewrite_driver()->ParseText("</div>");
  driver->FinishParse();
}

TEST_F(RewriteDriverTest, SetSessionFetcherTest) {
  AddFilter(RewriteOptions::kExtendCacheCss);

  const char kFetcher1Css[] = "Fetcher #1";
  const char kFetcher2Css[] = "Fetcher #2";
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kFetcher1Css, 100);

  GoogleString url = Encode(kTestDomain, RewriteOptions::kCacheExtenderId,
                            hasher()->Hash(kFetcher1Css), "a.css", "css");

  // Fetch from default.
  GoogleString output;
  ResponseHeaders response_headers;
  EXPECT_TRUE(FetchResourceUrl(url, &output, &response_headers));
  EXPECT_STREQ(kFetcher1Css, output);
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());

  // Load up a different file into a second fetcher.
  // We misappropriate the response_headers from previous fetch for simplicity.
  MockUrlFetcher mock2;
  mock2.SetResponse(AbsolutifyUrl("a.css"), response_headers, kFetcher2Css);

  // Switch over to new fetcher, making sure to set two of them to exercise
  // memory management. Note the synchronous mock fetcher we still have to
  // manage ourselves (as the RewriteDriver API is for async ones only).
  RewriteDriver* driver = rewrite_driver();
  driver->SetSessionFetcher(new FakeUrlAsyncFetcher(&mock2));
  CountingUrlAsyncFetcher* counter =
      new CountingUrlAsyncFetcher(driver->async_fetcher());
  driver->SetSessionFetcher(counter);
  EXPECT_EQ(counter, driver->async_fetcher());

  // Note that FetchResourceUrl will call driver->Clear() so we cannot
  // access 'counter' past this point.
  lru_cache()->Clear();  // get rid of cached version of input
  EXPECT_TRUE(FetchResourceUrl(url, &output, &response_headers));
  EXPECT_STREQ(kFetcher2Css, output);
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());

  // As FetchResourceUrl has cleared the driver, further fetcher should
  // grab fetcher 1 version.
  lru_cache()->Clear();  // get rid of cached version of input
  EXPECT_TRUE(FetchResourceUrl(url, &output, &response_headers));
  EXPECT_STREQ(kFetcher1Css, output);
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
}

class WaitAsyncFetch : public StringAsyncFetch {
 public:
  WaitAsyncFetch(const RequestContextPtr& req, GoogleString* content,
                 ThreadSystem* thread_system)
      : StringAsyncFetch(req, content),
        sync_(thread_system) {
  }
  virtual ~WaitAsyncFetch() {}

  virtual void HandleDone(bool status) {
    StringAsyncFetch::HandleDone(status);
    sync_.Notify();
  }
  void Wait() { sync_.Wait(); }

 private:
  WorkerTestBase::SyncPoint sync_;
};

class InPlaceTest : public RewriteTestBase {
 protected:
  InPlaceTest() {}
  virtual ~InPlaceTest() {}

  bool FetchInPlaceResource(const StringPiece& url,
                            bool perform_http_fetch,
                            GoogleString* content,
                            ResponseHeaders* response) {
    GoogleUrl gurl(url);
    content->clear();
    WaitAsyncFetch async_fetch(CreateRequestContext(), content,
                               server_context()->thread_system());
    async_fetch.set_response_headers(response);
    rewrite_driver_->FetchInPlaceResource(gurl, perform_http_fetch,
                                          &async_fetch);
    async_fetch.Wait();

    // Make sure we let the rewrite complete, and also wait for the driver to be
    // idle so we can reuse it safely.
    rewrite_driver_->WaitForShutDown();
    rewrite_driver_->Clear();

    EXPECT_TRUE(async_fetch.done());
    return async_fetch.done() && async_fetch.success();
  }

  bool TryFetchInPlaceResource(const StringPiece& url,
                               bool perform_http_fetch) {
    GoogleString contents;
    ResponseHeaders response;
    return FetchInPlaceResource(url, perform_http_fetch, &contents, &response);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(InPlaceTest);
};

TEST_F(InPlaceTest, FetchInPlaceResource) {
  AddFilter(RewriteOptions::kRewriteCss);

  GoogleString url = "http://example.com/foo.css";
  SetResponseWithDefaultHeaders(url, kContentTypeCss,
                                ".a { color: red; }", 100);

  // This will fail because cache is empty and we are not allowing HTTP fetch.
  bool perform_http_fetch = false;
  EXPECT_FALSE(TryFetchInPlaceResource(url, perform_http_fetch));
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  ClearStats();

  // Now we allow HTTP fetches and we expect success.
  perform_http_fetch = true;
  EXPECT_TRUE(TryFetchInPlaceResource(url, perform_http_fetch));
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  // We insert both original and rewritten resources.
  EXPECT_EQ(2, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
  ClearStats();

  // Now that we've loaded the resource into cache, we expect success.
  perform_http_fetch = false;
  EXPECT_TRUE(TryFetchInPlaceResource(url, perform_http_fetch));
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  ClearStats();
}

class RewriteDriverInhibitTest : public RewriteDriverTest {
 protected:
  RewriteDriverInhibitTest() {}

  void SetUpDocument() {
    SetupWriter();
    ASSERT_TRUE(rewrite_driver()->StartParse("http://example.com/index.html"));

    // Set up a document: <html><body><p></p></body></html>.
    html_ = rewrite_driver()->NewElement(NULL, HtmlName::kHtml);
    body_ = rewrite_driver()->NewElement(html_, HtmlName::kBody);
    par_ = rewrite_driver()->NewElement(body_, HtmlName::kP);
    par_->set_close_style(HtmlElement::EXPLICIT_CLOSE);
    HtmlCharactersNode* start = rewrite_driver()->NewCharactersNode(NULL, "");
    HtmlTestingPeer::AddEvent(rewrite_driver(),
                              new HtmlCharactersEvent(start, -1));
    rewrite_driver()->InsertElementAfterElement(start, html_);
    rewrite_driver()->AppendChild(html_, body_);
    rewrite_driver()->AppendChild(body_, par_);
  }

  // Uninhibits the EndEvent for element, and waits for the necessary flush
  // to complete.
  void UninhibitEndElementAndWait(HtmlElement* element) {
    rewrite_driver()->UninhibitEndElement(element);
    ASSERT_TRUE(!rewrite_driver()->EndElementIsInhibited(element));
    rewrite_driver()->Flush();
  }

  HtmlElement* html_;
  HtmlElement* body_;
  HtmlElement* par_;

 private:
  DISALLOW_COPY_AND_ASSIGN(RewriteDriverInhibitTest);
};

// Tests that we stop the flush immediately before the EndElementEvent for an
// inhibited element, and resume it when that element is uninhibited.
TEST_F(RewriteDriverInhibitTest, InhibitEndElement) {
  SetUpDocument();

  // Inhibit </body>.
  rewrite_driver()->InhibitEndElement(body_);
  ASSERT_TRUE(rewrite_driver()->EndElementIsInhibited(body_));

  // Verify that we do not flush </body> or beyond, even on a second flush.
  rewrite_driver()->Flush();
  EXPECT_EQ("<html><body><p></p>", output_buffer_);
  rewrite_driver()->Flush();
  EXPECT_EQ("<html><body><p></p>", output_buffer_);

  // Verify that we flush the entire document once </body> is uninhibited.
  UninhibitEndElementAndWait(body_);
  EXPECT_EQ("<html><body><p></p></body></html>", output_buffer_);
}

// Tests that we can inhibit and uninhibit the flush in multiple places.
TEST_F(RewriteDriverInhibitTest, MultipleInhibitEndElement) {
  SetUpDocument();

  // Inhibit </body> and </html>.
  rewrite_driver()->InhibitEndElement(body_);
  ASSERT_TRUE(rewrite_driver()->EndElementIsInhibited(body_));
  rewrite_driver()->InhibitEndElement(html_);
  ASSERT_TRUE(rewrite_driver()->EndElementIsInhibited(html_));

  // Verify that we will not flush </body> or beyond.
  rewrite_driver()->Flush();
  EXPECT_EQ("<html><body><p></p>", output_buffer_);

  // Uninhibit </body> and verify that we flush it.
  UninhibitEndElementAndWait(body_);
  EXPECT_EQ("<html><body><p></p></body>", output_buffer_);

  // Verify that we will flush the entire document once </html> is uninhibited.
  UninhibitEndElementAndWait(html_);
  EXPECT_EQ("<html><body><p></p></body></html>", output_buffer_);
}

// Tests that FinishParseAsync respects inhibits.
TEST_F(RewriteDriverInhibitTest, InhibitWithFinishParse) {
  SetUpDocument();

  // Inhibit </body>.
  rewrite_driver()->InhibitEndElement(body_);
  ASSERT_TRUE(rewrite_driver()->EndElementIsInhibited(body_));

  // Start finishing the parse.
  SchedulerBlockingFunction wait(rewrite_driver()->scheduler());
  rewrite_driver()->FinishParseAsync(&wait);

  // Busy wait until the resulting async flush completes.
  mock_scheduler()->AwaitQuiescence();
  EXPECT_EQ("<html><body><p></p>", output_buffer_);

  // Uninhibit </body> and wait for FinishParseAsync to call back.
  rewrite_driver()->UninhibitEndElement(body_);
  ASSERT_TRUE(!rewrite_driver()->EndElementIsInhibited(body_));
  wait.Block();

  // Verify that we flush the entire document once </body> is uninhibited.
  EXPECT_EQ("<html><body><p></p></body></html>", output_buffer_);
}


}  // namespace net_instaweb
