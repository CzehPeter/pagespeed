/*
 * Copyright 2011 Google Inc.
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

// Author: morlovich@google.com (Maksim Orlovich)

// Unit-tests for ProxyInterface

#include "net/instaweb/automatic/public/proxy_interface.h"

#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/counting_url_async_fetcher.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/mock_callback.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/public/resource_manager_test_base.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_scheduler.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/worker_test_base.h"

namespace net_instaweb {

namespace {

const char kCssContent[] = "* { display: none; }";
const char kMinimizedCssContent[] = "*{display:none}";

// Like ExpectCallback but for asynchronous invocation -- it lets
// one specify a WorkerTestBase::SyncPoint to help block until completion.
class AsyncExpectCallback : public ExpectCallback {
 public:
  AsyncExpectCallback(bool expect_success, WorkerTestBase::SyncPoint* notify)
      : ExpectCallback(expect_success), notify_(notify) {}

  virtual ~AsyncExpectCallback() {}

  virtual void Done(bool success) {
    ExpectCallback::Done(success);
    notify_->Notify();
  }

 private:
  WorkerTestBase::SyncPoint* notify_;
  DISALLOW_COPY_AND_ASSIGN(AsyncExpectCallback);
};

// TODO(morlovich): This currently relies on ResourceManagerTestBase to help
// setup fetchers; and also indirectly to prevent any rewrites from timing out
// (as it runs the tests with real scheduler but mock timer). It would probably
// be better to port this away to use TestRewriteDriverFactory directly.
class ProxyInterfaceTest : public ResourceManagerTestBase {
 protected:
  static const int kHtmlCacheTimeSec = 5000;

  ProxyInterfaceTest() :
    last_modified_time_("Sat, 03 Apr 2010 18:51:26 GMT") {}
  virtual ~ProxyInterfaceTest() {}

  virtual void SetUp() {
    RewriteOptions* options = resource_manager()->global_options();
    options->ClearSignatureForTesting();
    options->EnableFilter(RewriteOptions::kRewriteCss);
    options->set_max_html_cache_time_ms(kHtmlCacheTimeSec * Timer::kSecondMs);
    resource_manager()->ComputeSignature(options);
    ResourceManagerTestBase::SetUp();
    proxy_interface_.reset(
        new ProxyInterface("localhost", 80, resource_manager(), statistics()));
    start_time_ms_ = mock_timer()->NowMs();
  }

  virtual void TearDown() {
    // Make sure all the jobs are over before we check for leaks ---
    // someone might still be trying to clean themselves up.
    mock_scheduler()->AwaitQuiescence();
    EXPECT_EQ(0, resource_manager()->num_active_rewrite_drivers());
    ResourceManagerTestBase::TearDown();
  }

  void FetchFromProxy(const StringPiece& url,
                      bool expect_success,
                      GoogleString* string_out,
                      ResponseHeaders* headers_out) {
    RequestHeaders request_headers;
    FetchFromProxy(url, request_headers, expect_success, string_out,
                   headers_out);
  }

  void FetchFromProxy(const StringPiece& url,
                      const RequestHeaders& request_headers,
                      bool expect_success,
                      GoogleString* string_out,
                      ResponseHeaders* headers_out) {
    StringWriter writer(string_out);
    WorkerTestBase::SyncPoint sync(resource_manager()->thread_system());
    AsyncExpectCallback callback(expect_success, &sync);
    bool already_done =
        proxy_interface_->StreamingFetch(
            AbsolutifyUrl(url), request_headers, headers_out, &writer,
            message_handler(), &callback);
    if (already_done) {
      EXPECT_TRUE(callback.done());
    } else {
      sync.Wait();
    }
  }

  void CheckHeaders(const ResponseHeaders& headers,
                    const ContentType& expect_type) {
    ASSERT_TRUE(headers.has_status_code());
    EXPECT_EQ(HttpStatus::kOK, headers.status_code());
    EXPECT_STREQ(expect_type.mime_type(),
                 headers.Lookup1(HttpAttributes::kContentType));
  }

  RewriteOptions* GetCustomOptions(const StringPiece& url,
                                   const RequestHeaders& request_headers,
                                   RewriteOptions* domain_options) {
    // The default url_namer does not yield any name-derived options, and we
    // have not specified any URL params or request-headers, so there will be
    // no custom options, and no errors.
    GoogleUrl gurl(url);
    RewriteOptions* copy_options = domain_options != NULL ?
        domain_options->Clone() : NULL;
    ProxyInterface::OptionsBoolPair options_success =
        proxy_interface_->GetCustomOptions(gurl, request_headers,
                                           copy_options, message_handler());
    EXPECT_TRUE(options_success.second);
    return options_success.first;
  }

  scoped_ptr<ProxyInterface> proxy_interface_;
  int64 start_time_ms_;
  const GoogleString last_modified_time_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ProxyInterfaceTest);
};

TEST_F(ProxyInterfaceTest, FetchFailure) {
  GoogleString text;
  ResponseHeaders headers;

  // We don't want fetcher to fail the test, merely the fetch.
  SetFetchFailOnUnexpected(false);
  FetchFromProxy("invalid", false, &text, &headers);
}

TEST_F(ProxyInterfaceTest, PassThrough404) {
  GoogleString text;
  ResponseHeaders headers;
  SetFetchResponse404("404");
  FetchFromProxy("404", true, &text, &headers);
  ASSERT_TRUE(headers.has_status_code());
  EXPECT_EQ(HttpStatus::kNotFound, headers.status_code());
}

TEST_F(ProxyInterfaceTest, PassThroughResource) {
  GoogleString text;
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";

  InitResponseHeaders("text.txt", kContentTypeText, kContent,
                      kHtmlCacheTimeSec * 2);
  FetchFromProxy("text.txt", true, &text, &headers);
  CheckHeaders(headers, kContentTypeText);
  EXPECT_EQ(kContent, text);
}

TEST_F(ProxyInterfaceTest, SetCookieNotCached) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.Add(HttpAttributes::kSetCookie, "cookie");
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has Set-Cookie headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_STREQ("cookie", response_headers.Lookup1(HttpAttributes::kSetCookie));
  EXPECT_EQ(kContent, text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());

  // The next response that is served from cache does not have any Set-Cookie
  // headers.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(NULL, response_headers2.Lookup1(HttpAttributes::kSetCookie));
  EXPECT_EQ(kContent, text2);
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, SetCookie2NotCached) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.Add(HttpAttributes::kSetCookie2, "cookie");
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has Set-Cookie headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_STREQ("cookie", response_headers.Lookup1(HttpAttributes::kSetCookie2));
  EXPECT_EQ(kContent, text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());

  // The next response that is served from cache does not have any Set-Cookie
  // headers.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(NULL, response_headers2.Lookup1(HttpAttributes::kSetCookie2));
  EXPECT_EQ(kContent, text2);
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, EtagMatching) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.Replace(HttpAttributes::kEtag, "etag");
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has an Etag in the response.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
  EXPECT_STREQ("etag", response_headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());

  // The next response is served from cache.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(HttpStatus::kOK, response_headers2.status_code());
  EXPECT_STREQ("etag", response_headers2.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text2);
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());

  // The Etag matches and a 304 is served out.
  GoogleString text3;
  ResponseHeaders response_headers3;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kIfNoneMatch, "etag");
  FetchFromProxy("text.txt", request_headers, true, &text3, &response_headers3);
  EXPECT_EQ(HttpStatus::kNotModified, response_headers3.status_code());
  EXPECT_STREQ(NULL, response_headers3.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ("", text3);
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());

  // The Etag doesn't match and the full response is returned.
  GoogleString text4;
  ResponseHeaders response_headers4;
  request_headers.Replace(HttpAttributes::kIfNoneMatch, "mismatch");
  FetchFromProxy("text.txt", request_headers, true, &text4, &response_headers4);
  EXPECT_EQ(HttpStatus::kOK, response_headers4.status_code());
  EXPECT_STREQ("etag", response_headers4.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text4);
  EXPECT_EQ(3, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, LastModifiedMatch) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.SetLastModified(MockTimer::kApr_5_2010_ms - 2 * Timer::kDayMs);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has an Etag in the response.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
  EXPECT_STREQ(last_modified_time_,
               response_headers.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ(kContent, text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());

  // The next response is served from cache.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(HttpStatus::kOK, response_headers2.status_code());
  EXPECT_STREQ(last_modified_time_,
               response_headers2.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ(kContent, text2);
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());

  // The last modified timestamp matches and a 304 is served out.
  GoogleString text3;
  ResponseHeaders response_headers3;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kIfModifiedSince, last_modified_time_);
  FetchFromProxy("text.txt", request_headers, true, &text3, &response_headers3);
  EXPECT_EQ(HttpStatus::kNotModified, response_headers3.status_code());
  EXPECT_STREQ(NULL, response_headers3.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ("", text3);
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());

  // The last modified timestamp doesn't match and the full response is
  // returned.
  GoogleString text4;
  ResponseHeaders response_headers4;
  request_headers.Replace(HttpAttributes::kIfModifiedSince,
                          "Fri, 02 Apr 2010 18:51:26 GMT");
  FetchFromProxy("text.txt", request_headers, true, &text4, &response_headers4);
  EXPECT_EQ(HttpStatus::kOK, response_headers4.status_code());
  EXPECT_STREQ(last_modified_time_,
               response_headers4.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ(kContent, text4);
  EXPECT_EQ(3, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, EatCookiesOnReconstructFailure) {
  // Make sure we don't pass through a Set-Cookie[2] when reconstructing
  // a resource on demand fails.
  GoogleString abs_path = AbsolutifyUrl("a.css");
  ResponseHeaders response_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &response_headers);
  response_headers.Add(HttpAttributes::kSetCookie, "a cookie");
  response_headers.Add(HttpAttributes::kSetCookie2, "a weird old-time cookie");
  response_headers.ComputeCaching();
  SetFetchResponse(abs_path, response_headers, "broken_css{");

  ResponseHeaders out_response_headers;
  GoogleString text;
  FetchFromProxy(AbsolutifyUrl("a.css.pagespeed.cf.0.css"), true,
                 &text, &out_response_headers);
  EXPECT_EQ(NULL, out_response_headers.Lookup1(HttpAttributes::kSetCookie));
  EXPECT_EQ(NULL, out_response_headers.Lookup1(HttpAttributes::kSetCookie2));
}

TEST_F(ProxyInterfaceTest, RewriteHtml) {
  GoogleString text;
  ResponseHeaders headers;

  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  resource_manager()->ComputeSignature(options);
  InitResponseHeaders("page.html", kContentTypeHtml,
                      CssLinkHref("a.css"), kHtmlCacheTimeSec * 2);
  InitResponseHeaders("a.css", kContentTypeCss, kCssContent,
                      kHtmlCacheTimeSec * 2);

  FetchFromProxy("page.html", true, &text, &headers);
  CheckHeaders(headers, kContentTypeHtml);
  EXPECT_EQ(CssLinkHref(AbsolutifyUrl("a.css.pagespeed.cf.0.css")), text);
  headers.ComputeCaching();
  EXPECT_LE(start_time_ms_ + kHtmlCacheTimeSec * Timer::kSecondMs,
            headers.CacheExpirationTimeMs());

  // Fetch the rewritten resource as well.
  text.clear();
  FetchFromProxy(AbsolutifyUrl("a.css.pagespeed.cf.0.css"), true,
                 &text, &headers);
  CheckHeaders(headers, kContentTypeCss);
  headers.ComputeCaching();
  EXPECT_LE(start_time_ms_ + Timer::kYearMs, headers.CacheExpirationTimeMs());
  EXPECT_EQ(kMinimizedCssContent, text);
}

TEST_F(ProxyInterfaceTest, ReconstructResource) {
  GoogleString text;
  ResponseHeaders headers;

  // Fetching of a rewritten resource we did not just create
  // after an HTML rewrite.
  InitResponseHeaders("a.css", kContentTypeCss, kCssContent,
                      kHtmlCacheTimeSec * 2);
  FetchFromProxy("a.css.pagespeed.cf.0.css", true, &text, &headers);
  CheckHeaders(headers, kContentTypeCss);
  headers.ComputeCaching();
  EXPECT_LE(start_time_ms_ + Timer::kYearMs, headers.CacheExpirationTimeMs());
  EXPECT_EQ(kMinimizedCssContent, text);
}

TEST_F(ProxyInterfaceTest, CustomOptionsWithNoUrlNamerOptions) {
  // The default url_namer does not yield any name-derived options, and we
  // have not specified any URL params or request-headers, so there will be
  // no custom options, and no errors.
  RequestHeaders request_headers;
  scoped_ptr<RewriteOptions> options(
      GetCustomOptions("http://example.com/", request_headers, NULL));
  ASSERT_TRUE(options.get() == NULL);

  // Now put a query-param in, just turning on PageSpeed.  The core filters
  // should be enabled.
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeed=on",
      request_headers, NULL));
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  EXPECT_TRUE(options->Enabled(RewriteOptions::kExtendCache));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Now explicitly enable a filter, which should disable others.
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeedFilters=extend_cache",
      request_headers, NULL));
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->Enabled(RewriteOptions::kExtendCache));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Now put a request-header in, turning off pagespeed.  request-headers get
  // priority over query-params.
  request_headers.Add("ModPagespeed", "off");
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeed=on",
      request_headers, NULL));
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_FALSE(options->enabled());

  // Now explicitly enable a bogus filter, which should will cause the
  // options to be uncomputable.
  GoogleUrl gurl("http://example.com/?ModPagespeedFilters=bogus_filter");
  EXPECT_FALSE(proxy_interface_->GetCustomOptions(gurl, request_headers, NULL,
                                                  message_handler()).second);
}

TEST_F(ProxyInterfaceTest, CustomOptionsWithUrlNamerOptions) {
  // Inject a url-namer that will establish a domain configuration.
  RewriteOptions namer_options;
  namer_options.EnableFilter(RewriteOptions::kCombineJavascript);

  RequestHeaders request_headers;
  scoped_ptr<RewriteOptions> options(
      GetCustomOptions("http://example.com/", request_headers, &namer_options));
  // Even with no query-params or request-headers, we get the custom
  // options as domain options provided as argument.
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  EXPECT_FALSE(options->Enabled(RewriteOptions::kExtendCache));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Now combine with query params, which turns core-filters on.
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeed=on",
      request_headers, &namer_options));
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  EXPECT_TRUE(options->Enabled(RewriteOptions::kExtendCache));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Explicitly enable a filter in query-params, which will turn off
  // the core filters that have not been explicitly enabled.  Note
  // that explicit filter-setting in query-params overrides completely
  // the options provided as a parameter.
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeedFilters=combine_css",
      request_headers, &namer_options));
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  EXPECT_FALSE(options->Enabled(RewriteOptions::kExtendCache));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Now explicitly enable a bogus filter, which should will cause the
  // options to be uncomputable.
  GoogleUrl gurl("http://example.com/?ModPagespeedFilters=bogus_filter");
  EXPECT_FALSE(proxy_interface_->GetCustomOptions(gurl, request_headers,
                                                  (&namer_options)->Clone(),
                                                  message_handler()).second);
}

TEST_F(ProxyInterfaceTest, MinResourceTimeZero) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  options->set_min_resource_cache_time_to_rewrite_ms(
      kHtmlCacheTimeSec * Timer::kSecondMs);
  resource_manager()->ComputeSignature(options);

  InitResponseHeaders("page.html", kContentTypeHtml,
                      CssLinkHref("a.css"), kHtmlCacheTimeSec * 2);
  InitResponseHeaders("a.css", kContentTypeCss, kCssContent,
                      kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy("page.html", true, &text, &headers);
  EXPECT_EQ(CssLinkHref(AbsolutifyUrl("a.css.pagespeed.cf.0.css")), text);
}

TEST_F(ProxyInterfaceTest, MinResourceTimeLarge) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  options->set_min_resource_cache_time_to_rewrite_ms(
      4 * kHtmlCacheTimeSec * Timer::kSecondMs);
  resource_manager()->ComputeSignature(options);

  InitResponseHeaders("page.html", kContentTypeHtml,
                      CssLinkHref("a.css"), kHtmlCacheTimeSec * 2);
  InitResponseHeaders("a.css", kContentTypeCss, kCssContent,
                      kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy("page.html", true, &text, &headers);
  EXPECT_EQ(CssLinkHref("a.css"), text);
}

}  // namespace

}  // namespace net_instaweb
