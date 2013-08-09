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

#include <cstddef>

#include "base/scoped_ptr.h"            // for scoped_ptr
#include "net/instaweb/htmlparse/public/empty_html_filter.h"
#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/mock_url_fetcher.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/mock_callback.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/timing.pb.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_manager_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/test_rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/mock_scheduler.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/public/time_util.h"
#include "net/instaweb/util/worker_test_base.h"

namespace net_instaweb {

class HtmlFilter;
class MessageHandler;

namespace {

const char kCssContent[] = "* { display: none; }";
const char kMinimizedCssContent[] = "*{display:none}";

// Like ExpectStringAsyncFetch but for asynchronous invocation -- it lets
// one specify a WorkerTestBase::SyncPoint to help block until completion.
class AsyncExpectStringAsyncFetch : public ExpectStringAsyncFetch {
 public:
  AsyncExpectStringAsyncFetch(bool expect_success,
                              WorkerTestBase::SyncPoint* notify)
      : ExpectStringAsyncFetch(expect_success), notify_(notify) {}

  virtual ~AsyncExpectStringAsyncFetch() {}

  virtual void HandleDone(bool success) {
    ExpectStringAsyncFetch::HandleDone(success);
    notify_->Notify();
  }

 private:
  WorkerTestBase::SyncPoint* notify_;
  DISALLOW_COPY_AND_ASSIGN(AsyncExpectStringAsyncFetch);
};

// This class creates a proxy URL naming rule that encodes an "owner" domain
// and an "origin" domain, all inside a fixed proxy-domain.
class ProxyUrlNamer : public UrlNamer {
 public:
  static const char kProxyHost[];

  ProxyUrlNamer() : authorized_(true), options_(NULL) {}

  // Given the request_url, generate the original url.
  virtual bool Decode(const GoogleUrl& gurl,
                      GoogleUrl* domain,
                      GoogleString* decoded) const {
    if (gurl.Host() != kProxyHost) {
      return false;
    }
    StringPieceVector path_vector;
    SplitStringPieceToVector(gurl.PathAndLeaf(), "/", &path_vector, false);
    if (path_vector.size() < 3) {
      return false;
    }
    if (domain != NULL) {
      domain->Reset(StrCat("http://", path_vector[1]));
    }

    // [0] is "" because PathAndLeaf returns a string with a leading slash
    *decoded = StrCat(gurl.Scheme(), ":/");
    for (size_t i = 2, n = path_vector.size(); i < n; ++i) {
      StrAppend(decoded, "/", path_vector[i]);
    }
    return true;
  }

  virtual bool IsAuthorized(const GoogleUrl& gurl,
                            const RewriteOptions& options) const {
    return authorized_;
  }

  // Given the request url and request headers, generate the rewrite options.
  virtual void DecodeOptions(const GoogleUrl& request_url,
                             const RequestHeaders& request_headers,
                             Callback* callback,
                             MessageHandler* handler) const {
    callback->Done((options_ == NULL) ? NULL : options_->Clone());
  }

  void set_authorized(bool authorized) { authorized_ = authorized; }
  void set_options(RewriteOptions* options) { options_ = options; }

 private:
  bool authorized_;
  RewriteOptions* options_;
  DISALLOW_COPY_AND_ASSIGN(ProxyUrlNamer);
};

const char ProxyUrlNamer::kProxyHost[] = "proxy_host.com";

// Mock filter which gets passed to the new rewrite driver created in
// proxy_fetch. We need this to ascertain that start_time_ms is set correctly.
class MockFilter : public EmptyHtmlFilter {
 public:
  MockFilter(RewriteDriver* driver)
      : driver_(driver) {
  }

  virtual void StartDocument() {
  }

  virtual const char* Name() const { return "MockFilter"; }

 private:
  RewriteDriver* driver_;
};

// TODO(morlovich): This currently relies on ResourceManagerTestBase to help
// setup fetchers; and also indirectly to prevent any rewrites from timing out
// (as it runs the tests with real scheduler but mock timer). It would probably
// be better to port this away to use TestRewriteDriverFactory directly.
class ProxyInterfaceTest : public ResourceManagerTestBase {
 protected:
  static const int kHtmlCacheTimeSec = 5000;

  ProxyInterfaceTest()
      : max_age_300_("max-age=300"),
        request_start_time_ms_(-1) {
    ConvertTimeToString(MockTimer::kApr_5_2010_ms, &start_time_string_);
    ConvertTimeToString(MockTimer::kApr_5_2010_ms + 5 * Timer::kMinuteMs,
                        &start_time_plus_300s_string_);
    ConvertTimeToString(MockTimer::kApr_5_2010_ms - 2 * Timer::kDayMs,
                        &old_time_string_);
  }
  virtual ~ProxyInterfaceTest() {}

  virtual void SetUp() {
    RewriteOptions* options = resource_manager()->global_options();
    options->ClearSignatureForTesting();
    options->EnableFilter(RewriteOptions::kRewriteCss);
    options->set_max_html_cache_time_ms(kHtmlCacheTimeSec * Timer::kSecondMs);
    options->set_ajax_rewriting_enabled(true);
    options->Disallow("*blacklist*");
    resource_manager()->ComputeSignature(options);
    ResourceManagerTestBase::SetUp();
    ProxyInterface::Initialize(statistics());
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
    WorkerTestBase::SyncPoint sync(resource_manager()->thread_system());
    AsyncExpectStringAsyncFetch callback(expect_success, &sync);
    callback.set_response_headers(headers_out);
    callback.request_headers()->CopyFrom(request_headers);
    bool already_done = proxy_interface_->Fetch(AbsolutifyUrl(url),
                                                message_handler(), &callback);
    if (already_done) {
      EXPECT_TRUE(callback.done());
    } else {
      sync.Wait();
    }
    mock_scheduler()->AwaitQuiescence();
    *string_out = callback.buffer();
    timing_info_.CopyFrom(*callback.timing_info());
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

  // Serve a trivial HTML page with initial Cache-Control header set to
  // input_cache_control and return the Cache-Control header after running
  // through ProxyInterface.
  //
  // A unique id must be set to assure different websites are requested.
  // id is put in a URL, so it probably shouldn't have spaces and other
  // special chars.
  GoogleString RewriteHtmlCacheHeader(const StringPiece& id,
                                      const StringPiece& input_cache_control) {
    GoogleString url = StrCat("http://www.example.com/", id, ".html");
    ResponseHeaders input_headers;
    DefaultResponseHeaders(kContentTypeHtml, 100, &input_headers);
    input_headers.Replace(HttpAttributes::kCacheControl, input_cache_control);
    SetFetchResponse(url, input_headers, "<body>Foo</body>");

    GoogleString body;
    ResponseHeaders output_headers;
    FetchFromProxy(url, true, &body, &output_headers);
    ConstStringStarVector values;
    output_headers.Lookup(HttpAttributes::kCacheControl, &values);
    return JoinStringStar(values, ", ");
  }

  void CheckExtendCache(RewriteOptions* options, bool x) {
    EXPECT_EQ(x, options->Enabled(RewriteOptions::kExtendCacheCss));
    EXPECT_EQ(x, options->Enabled(RewriteOptions::kExtendCacheImages));
    EXPECT_EQ(x, options->Enabled(RewriteOptions::kExtendCacheScripts));
  }

  scoped_ptr<ProxyInterface> proxy_interface_;
  int64 start_time_ms_;
  GoogleString start_time_string_;
  GoogleString start_time_plus_300s_string_;
  GoogleString old_time_string_;
  TimingInfo timing_info_;
  const GoogleString max_age_300_;
  int64 request_start_time_ms_;

 private:
  friend class FilterCallback;

  DISALLOW_COPY_AND_ASSIGN(ProxyInterfaceTest);
};

class FilterCallback : public TestRewriteDriverFactory::CreateFilterCallback {
 public:
  FilterCallback(ProxyInterfaceTest* proxy_interface)
      : proxy_interface_(proxy_interface) {
  }

  virtual HtmlFilter* Done(RewriteDriver* driver) {
    return new MockFilter(driver);
  }

  virtual ~FilterCallback() {}

 private:
  ProxyInterfaceTest* proxy_interface_;

  DISALLOW_COPY_AND_ASSIGN(FilterCallback);
};

TEST_F(ProxyInterfaceTest, TimingInfo) {
  GoogleString url = "http://www.example.com/";
  GoogleString text;
  RequestHeaders request_headers;
  ResponseHeaders headers;
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  mock_url_fetcher_.SetResponse("http://www.example.com/", headers,
                                "<html></html>");

  FetchFromProxy(url, request_headers, true, &text, &headers);
  ASSERT_TRUE(timing_info_.has_cache1_ms());
  EXPECT_EQ(timing_info_.cache1_ms(), 0);
  EXPECT_FALSE(timing_info_.has_cache2_ms());
  EXPECT_FALSE(timing_info_.has_header_fetch_ms());
  EXPECT_FALSE(timing_info_.has_fetch_ms());
}

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

  SetResponseWithDefaultHeaders("text.txt", kContentTypeText, kContent,
                                kHtmlCacheTimeSec * 2);
  FetchFromProxy("text.txt", true, &text, &headers);
  CheckHeaders(headers, kContentTypeText);
  EXPECT_EQ(kContent, text);
}

TEST_F(ProxyInterfaceTest, PassThroughEmptyResource) {
  ResponseHeaders headers;
  const char kContent[] = "";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(kContent, text2);
  // The HTTP response is found but the ajax metadata is not found.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
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
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // The next response that is served from cache does not have any Set-Cookie
  // headers.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(NULL, response_headers2.Lookup1(HttpAttributes::kSetCookie));
  EXPECT_EQ(kContent, text2);
  // The HTTP response is found but the ajax metadata is not found.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
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
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // The next response that is served from cache does not have any Set-Cookie
  // headers.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(NULL, response_headers2.Lookup1(HttpAttributes::kSetCookie2));
  EXPECT_EQ(kContent, text2);
  // The HTTP response is found but the ajax metadata is not found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
}

TEST_F(ProxyInterfaceTest, ImplicitCachingHeadersForCss) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  mock_timer()->SetTimeUs(MockTimer::kApr_5_2010_ms * Timer::kMsUs);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kContent);

  // The first response served by the fetcher has caching headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata, one for the HTTP response and one by the css
  // filter which looks up metadata while rewriting. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One hit for ajax metadata and one for the HTTP response.
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, NoImplicitCachingHeadersForHtml) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  mock_timer()->SetTimeUs(MockTimer::kApr_5_2010_ms * Timer::kMsUs);
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);

  // The first response served by the fetcher does not have implicit caching
  // headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.html", true, &text, &response_headers);
  EXPECT_STREQ(NULL, response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // Fetch again. Not found in cache.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);
  EXPECT_EQ(NULL, response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
}

TEST_F(ProxyInterfaceTest, EtagsAddedWhenAbsent) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.RemoveAll(HttpAttributes::kEtag);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has no Etag in the response.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
  EXPECT_EQ(NULL, response_headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  ClearStats();

  // An Etag is added before writing to cache. The next response is served from
  // cache and has an Etag.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(HttpStatus::kOK, response_headers2.status_code());
  EXPECT_STREQ("W/PSA-0", response_headers2.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text2);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  ClearStats();

  // The Etag matches and a 304 is served out.
  GoogleString text3;
  ResponseHeaders response_headers3;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kIfNoneMatch, "W/PSA-0");
  FetchFromProxy("text.txt", request_headers, true, &text3, &response_headers3);
  EXPECT_EQ(HttpStatus::kNotModified, response_headers3.status_code());
  EXPECT_STREQ(NULL, response_headers3.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ("", text3);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
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
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());

  ClearStats();
  // The next response is served from cache.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(HttpStatus::kOK, response_headers2.status_code());
  EXPECT_STREQ("etag", response_headers2.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text2);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  ClearStats();

  // The Etag matches and a 304 is served out.
  GoogleString text3;
  ResponseHeaders response_headers3;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kIfNoneMatch, "etag");
  FetchFromProxy("text.txt", request_headers, true, &text3, &response_headers3);
  EXPECT_EQ(HttpStatus::kNotModified, response_headers3.status_code());
  EXPECT_STREQ(NULL, response_headers3.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ("", text3);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());

  ClearStats();
  // The Etag doesn't match and the full response is returned.
  GoogleString text4;
  ResponseHeaders response_headers4;
  request_headers.Replace(HttpAttributes::kIfNoneMatch, "mismatch");
  FetchFromProxy("text.txt", request_headers, true, &text4, &response_headers4);
  EXPECT_EQ(HttpStatus::kOK, response_headers4.status_code());
  EXPECT_STREQ("etag", response_headers4.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text4);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
}

TEST_F(ProxyInterfaceTest, LastModifiedMatch) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.SetLastModified(MockTimer::kApr_5_2010_ms);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has an Etag in the response.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());

  ClearStats();
  // The next response is served from cache.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(HttpStatus::kOK, response_headers2.status_code());
  EXPECT_STREQ(start_time_string_,
               response_headers2.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ(kContent, text2);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());

  ClearStats();
  // The last modified timestamp matches and a 304 is served out.
  GoogleString text3;
  ResponseHeaders response_headers3;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kIfModifiedSince, start_time_string_);
  FetchFromProxy("text.txt", request_headers, true, &text3, &response_headers3);
  EXPECT_EQ(HttpStatus::kNotModified, response_headers3.status_code());
  EXPECT_STREQ(NULL, response_headers3.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ("", text3);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());

  ClearStats();
  // The last modified timestamp doesn't match and the full response is
  // returned.
  GoogleString text4;
  ResponseHeaders response_headers4;
  request_headers.Replace(HttpAttributes::kIfModifiedSince,
                          "Fri, 02 Apr 2010 18:51:26 GMT");
  FetchFromProxy("text.txt", request_headers, true, &text4, &response_headers4);
  EXPECT_EQ(HttpStatus::kOK, response_headers4.status_code());
  EXPECT_STREQ(start_time_string_,
               response_headers4.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ(kContent, text4);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.`
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
}

TEST_F(ProxyInterfaceTest, AjaxRewritingForCss) {
  ResponseHeaders headers;
  mock_timer()->SetTimeUs(MockTimer::kApr_5_2010_ms * Timer::kMsUs);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kCssContent);

  // The first response served by the fetcher and is not rewritten. An ajax
  // rewrite is triggered.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kCssContent, text);
  // One lookup for ajax metadata, one for the HTTP response and one by the css
  // filter which looks up metadata while rewriting. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // The rewrite is complete and the optimized version is served.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kMinimizedCssContent, text);
  // One hit for ajax metadata and one for the rewritten HTTP response.
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, AjaxRewritingDisabledByGlobalDisable) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->set_enabled(false);
  resource_manager()->ComputeSignature(options);

  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("a.css", true, &text, &response_headers);
  // First fetch will not get rewritten no matter what.
  EXPECT_STREQ(kCssContent, text);

  // Second fetch would get minified if ajax rewriting were on; but
  // it got disabled by the global toggle.
  text.clear();
  FetchFromProxy("a.css", true, &text, &response_headers);
  EXPECT_STREQ(kCssContent, text);
}

TEST_F(ProxyInterfaceTest, AjaxRewritingSkippedIfBlacklisted) {
  ResponseHeaders headers;
  mock_timer()->SetTimeUs(MockTimer::kApr_5_2010_ms * Timer::kMsUs);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("blacklist.css"), headers, kCssContent);

  // The first response is served by the fetcher. Since the url is blacklisted,
  // no ajax rewriting happens.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("blacklist.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kCssContent, text);
  // Since no ajax rewriting happens, there is only a single cache lookup for
  // the resource.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // The same thing happens on the second request.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("blacklist.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kCssContent, text);
  // The resource is found in cache this time.
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
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
  FetchFromProxy(Encode(kTestDomain, "cf", "0", "a.css", "css"), true,
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

  headers.Add(HttpAttributes::kEtag, "something");
  headers.SetDateAndCaching(MockTimer::kApr_5_2010_ms,
                            kHtmlCacheTimeSec * 2 * Timer::kSecondMs);
  headers.SetLastModified(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("page.html"), headers, CssLinkHref("a.css"));

  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  headers.Clear();
  FetchFromProxy("page.html", true, &text, &headers);
  CheckHeaders(headers, kContentTypeHtml);
  EXPECT_EQ(CssLinkHref(Encode(kTestDomain, "cf", "0", "a.css", "css")), text);
  headers.ComputeCaching();
  EXPECT_LE(start_time_ms_ + kHtmlCacheTimeSec * Timer::kSecondMs,
            headers.CacheExpirationTimeMs());
  EXPECT_EQ(NULL, headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(NULL, headers.Lookup1(HttpAttributes::kLastModified));

  // Fetch the rewritten resource as well.
  text.clear();
  FetchFromProxy(Encode(kTestDomain, "cf", "0", "a.css", "css"), true,
                 &text, &headers);
  CheckHeaders(headers, kContentTypeCss);
  headers.ComputeCaching();
  EXPECT_LE(start_time_ms_ + Timer::kYearMs, headers.CacheExpirationTimeMs());
  EXPECT_EQ(kMinimizedCssContent, text);
}

TEST_F(ProxyInterfaceTest, DontRewriteDisallowedHtml) {
  // Blacklisted URL should not be rewritten.
  SetResponseWithDefaultHeaders("blacklist.html", kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2),
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy("blacklist.html", true, &text, &headers);
  CheckHeaders(headers, kContentTypeHtml);
  EXPECT_EQ(CssLinkHref("a.css"), text);
}

TEST_F(ProxyInterfaceTest, DontRewriteMislabeledAsHtml) {
  // Make sure we don't rewrite things that claim to be HTML, but aren't.
  GoogleString text;
  ResponseHeaders headers;

  SetResponseWithDefaultHeaders("page.js", kContentTypeHtml,
                                StrCat("//", CssLinkHref("a.css")),
                                kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  FetchFromProxy("page.js", true, &text, &headers);
  CheckHeaders(headers, kContentTypeHtml);
  EXPECT_EQ(StrCat("//", CssLinkHref("a.css")), text);
}

TEST_F(ProxyInterfaceTest, ReconstructResource) {
  GoogleString text;
  ResponseHeaders headers;

  // Fetching of a rewritten resource we did not just create
  // after an HTML rewrite.
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);
  FetchFromProxy(Encode("", "cf", "0", "a.css", "css"), true, &text, &headers);
  CheckHeaders(headers, kContentTypeCss);
  headers.ComputeCaching();
  EXPECT_LE(start_time_ms_ + Timer::kYearMs, headers.CacheExpirationTimeMs());
  EXPECT_EQ(kMinimizedCssContent, text);
}

TEST_F(ProxyInterfaceTest, ReconstructResourceCustomOptions) {
  const char kCssWithEmbeddedImage[] = "*{background-image:url(%s)}";
  const char kBackgroundImage[] = "1.png";

  GoogleString text;
  ResponseHeaders headers;

  // We're not going to image-compress so we don't need our mock image
  // to really be an image.
  SetResponseWithDefaultHeaders(kBackgroundImage, kContentTypePng, "image",
                                kHtmlCacheTimeSec * 2);
  GoogleString orig_css = StringPrintf(kCssWithEmbeddedImage, kBackgroundImage);
  SetResponseWithDefaultHeaders("embedded.css", kContentTypeCss,
                                orig_css, kHtmlCacheTimeSec * 2);

  // By default, cache extension is off in the default options.
  resource_manager()->global_options()->SetDefaultRewriteLevel(
      RewriteOptions::kPassThrough);
  ASSERT_FALSE(options()->Enabled(RewriteOptions::kExtendCacheCss));
  ASSERT_FALSE(options()->Enabled(RewriteOptions::kExtendCacheImages));
  ASSERT_FALSE(options()->Enabled(RewriteOptions::kExtendCacheScripts));
  ASSERT_EQ(RewriteOptions::kPassThrough, options()->level());

  // Because cache-extension was turned off, the image in the CSS file
  // will not be changed.
  FetchFromProxy("I.embedded.css.pagespeed.cf.0.css", true, &text, &headers);
  EXPECT_EQ(orig_css, text);

  // Now turn on cache-extension for custom options.  Invalidate cache entries
  // up to and including the current timestamp and advance by 1ms, otherwise
  // the previously stored embedded.css.pagespeed.cf.0.css will get re-used.
  scoped_ptr<RewriteOptions> custom_options(factory()->NewRewriteOptions());
  custom_options->EnableFilter(RewriteOptions::kExtendCacheCss);
  custom_options->EnableFilter(RewriteOptions::kExtendCacheImages);
  custom_options->EnableFilter(RewriteOptions::kExtendCacheScripts);
  custom_options->set_cache_invalidation_timestamp(mock_timer()->NowMs());
  mock_timer()->AdvanceUs(Timer::kMsUs);

  // Inject the custom options into the flow via a custom URL namer.
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  resource_manager()->set_url_namer(&url_namer);

  // Use EncodeNormal because it matches the logic used by ProxyUrlNamer.
  const GoogleString kExtendedBackgroundImage =
      EncodeNormal(kTestDomain, "ce", "0", kBackgroundImage, "png");

  // Now when we fetch the options, we'll find the image in the CSS
  // cache-extended.
  text.clear();
  FetchFromProxy("I.embedded.css.pagespeed.cf.0.css", true, &text, &headers);
  EXPECT_EQ(StringPrintf(kCssWithEmbeddedImage,
                         kExtendedBackgroundImage.c_str()),
            text);
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
  CheckExtendCache(options.get(), true);
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Now explicitly enable a filter, which should disable others.
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeedFilters=extend_cache",
      request_headers, NULL));
  ASSERT_TRUE(options.get() != NULL);
  CheckExtendCache(options.get(), true);
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
  CheckExtendCache(options.get(), false);
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Now combine with query params, which turns core-filters on.
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeed=on",
      request_headers, &namer_options));
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  CheckExtendCache(options.get(), true);
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
  CheckExtendCache(options.get(), false);
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

  SetResponseWithDefaultHeaders("page.html", kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy("page.html", true, &text, &headers);
  EXPECT_EQ(CssLinkHref(Encode(kTestDomain, "cf", "0", "a.css", "css")), text);
}

TEST_F(ProxyInterfaceTest, MinResourceTimeLarge) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  options->set_min_resource_cache_time_to_rewrite_ms(
      4 * kHtmlCacheTimeSec * Timer::kSecondMs);
  resource_manager()->ComputeSignature(options);

  SetResponseWithDefaultHeaders("page.html", kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy("page.html", true, &text, &headers);
  EXPECT_EQ(CssLinkHref("a.css"), text);
}

TEST_F(ProxyInterfaceTest, CacheRequests) {
  ResponseHeaders html_headers;
  DefaultResponseHeaders(kContentTypeHtml, kHtmlCacheTimeSec, &html_headers);
  SetFetchResponse(AbsolutifyUrl("page.html"), html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy("page.html", true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(AbsolutifyUrl("page.html"), html_headers, "2");
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");

  // Original response is still cached in both cases, so we do not
  // fetch the new values.
  text.clear();
  FetchFromProxy("page.html", true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);
}

// Verifies that we proxy uncacheable resources, but do not insert them in the
// cache.
TEST_F(ProxyInterfaceTest, UncacheableResourcesNotCachedOnProxy) {
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                     300000, ", private");
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  ProxyUrlNamer url_namer;
  resource_manager()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;

  // We should not cache while fetching via kProxyHost.
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/style.css"),
      true, &out_text, &out_headers);
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());  // mapping, input resource
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // input resource
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());

  // We should likewise not cache while fetching on the origin domain.
  out_text.clear();
  ClearStats();
  FetchFromProxy("style.css", true, &out_text, &out_headers);
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());  // mapping, input resource
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // input resource
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());

  // Since the original response is not cached, we should pick up changes in the
  // input resource immediately.
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");
  out_text.clear();
  ClearStats();
  FetchFromProxy("style.css", true, &out_text, &out_headers);
  EXPECT_EQ("b", out_text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());  // mapping, input resource
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // input resource
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
}

// Verifies that we retrieve and serve uncacheable resources, but do not insert
// them in the cache.
TEST_F(ProxyInterfaceTest, UncacheableResourcesNotCachedOnResourceFetch) {
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                     300000, ", private");
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  resource_manager()->ComputeSignature(options);

  ResponseHeaders out_headers;
  GoogleString out_text;

  // cf is not on-the-fly, and we can reconstruct it while keeping it private.
  FetchFromProxy(Encode(kTestDomain, "cf", "0", "style.css", "css"),
                 true, &out_text, &out_headers);
  EXPECT_TRUE(out_headers.HasValue(HttpAttributes::kCacheControl, "private"));
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(4, lru_cache()->num_misses());  // 2x output, metadata, input
  EXPECT_EQ(3, http_cache()->cache_misses()->Get());  // 2x output, input
  EXPECT_EQ(2, lru_cache()->num_inserts());  // mapping, uncacheable memo
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());  // uncacheable memo

  out_text.clear();
  ClearStats();
  // ce is on-the-fly, and we can recover even though style.css is private.
  FetchFromProxy(Encode(kTestDomain, "ce", "0", "style.css", "css"),
                 true, &out_text, &out_headers);
  EXPECT_TRUE(out_headers.HasValue(HttpAttributes::kCacheControl, "private"));
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(1, lru_cache()->num_hits());  // input uncacheable memo
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get()); // input uncacheable memo
  EXPECT_EQ(1, lru_cache()->num_inserts());  // mapping
  EXPECT_EQ(1, lru_cache()->num_identical_reinserts()); // uncacheable memo
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());  // uncacheable memo

  out_text.clear();
  ClearStats();
  FetchFromProxy(Encode(kTestDomain, "ce", "0", "style.css", "css"),
                 true, &out_text, &out_headers);
  EXPECT_TRUE(out_headers.HasValue(HttpAttributes::kCacheControl, "private"));
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(1, lru_cache()->num_hits());  // uncacheable memo
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // uncacheable memo
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_identical_reinserts())
      << "uncacheable memo, metadata";
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());  // uncacheable memo

  // Since the original response is not cached, we should pick up changes in the
  // input resource immediately.
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");
  out_text.clear();
  ClearStats();
  FetchFromProxy(Encode(kTestDomain, "ce", "0", "style.css", "css"),
                 true, &out_text, &out_headers);
  EXPECT_TRUE(out_headers.HasValue(HttpAttributes::kCacheControl, "private"));
  EXPECT_EQ("b", out_text);
  EXPECT_EQ(1, lru_cache()->num_hits());  // uncacheable memo
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // uncacheable memo
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_identical_reinserts())
      << "uncacheable memo, metadata";
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());  // uncacheable memo
}

// No matter what options->respect_vary() is set to we will respect HTML Vary
// headers.
TEST_F(ProxyInterfaceTest, NoCacheVaryHtml) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->set_respect_vary(false);
  resource_manager()->ComputeSignature(options);

  ResponseHeaders html_headers;
  DefaultResponseHeaders(kContentTypeHtml, kHtmlCacheTimeSec, &html_headers);
  html_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  html_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("page.html"), html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy("page.html", true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(AbsolutifyUrl("page.html"), html_headers, "2");
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");

  // HTML was not cached because of Vary: User-Agent header.
  // So we do fetch the new value.
  text.clear();
  FetchFromProxy("page.html", true, &text, &actual_headers);
  EXPECT_EQ("2", text);
  // Resource was cached because we have respect_vary == false.
  // So we serve the old value.
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);
}

// Respect Vary for resources if options tell us to.
TEST_F(ProxyInterfaceTest, NoCacheVaryAll) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->set_respect_vary(true);
  resource_manager()->ComputeSignature(options);

  ResponseHeaders html_headers;
  DefaultResponseHeaders(kContentTypeHtml, kHtmlCacheTimeSec, &html_headers);
  html_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  html_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("page.html"), html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy("page.html", true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(AbsolutifyUrl("page.html"), html_headers, "2");
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");

  // Original response was not cached in either case, so we do fetch the
  // new value.
  text.clear();
  FetchFromProxy("page.html", true, &text, &actual_headers);
  EXPECT_EQ("2", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("b", text);
}

TEST_F(ProxyInterfaceTest, Blacklist) {
  const char content[] =
      "<html>\n"
      "  <head/>\n"
      "  <body>\n"
      "    <script src='tiny_mce.js'></script>\n"
      "  </body>\n"
      "</html>\n";
  SetResponseWithDefaultHeaders("tiny_mce.js", kContentTypeJavascript, "", 100);
  ValidateNoChanges("blacklist", content);

  SetResponseWithDefaultHeaders("page.html", kContentTypeHtml, content, 0);
  GoogleString text_out;
  ResponseHeaders headers_out;
  FetchFromProxy("page.html", true, &text_out, &headers_out);
  EXPECT_STREQ(content, text_out);
}

TEST_F(ProxyInterfaceTest, RepairMismappedResource) {
  // Teach the mock fetcher to serve origin content for
  // "http://test.com/foo.js".
  const char kContent[] = "function f() {alert('foo');}";
  SetResponseWithDefaultHeaders("foo.js", kContentTypeHtml, kContent,
                                kHtmlCacheTimeSec * 2);

  // Set up a Mock Namer that will mutate output resources to
  // be served on proxy_host.com, encoding the origin URL.
  ProxyUrlNamer url_namer;
  ResponseHeaders headers;
  GoogleString text;
  resource_manager()->set_url_namer(&url_namer);

  // Now fetch the origin content.  This will simply hit the
  // mock fetcher and always worked.
  FetchFromProxy("foo.js", true, &text, &headers);
  EXPECT_EQ(kContent, text);

  // Now make a weird URL encoding of the origin resource using the
  // proxy host.  This may happen via javascript that detects its
  // own path and initiates a 'load()' of another js file from the
  // same path.  In this variant, the resource is served from the
  // "source domain", so it is automatically whitelisted.
  text.clear();
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost, "/test.com/test.com/foo.js"),
      true, &text, &headers);
  EXPECT_EQ(kContent, text);

  // In the next case, the resource is served from a different domain.  This
  // is an open-proxy vulnerability and thus should fail.
  text.clear();
  url_namer.set_authorized(false);
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost, "/test.com/evil.com/foo.js"),
      false, &text, &headers);
}

TEST_F(ProxyInterfaceTest, CrossDomainHeaders) {
  // If we're serving content from test.com via kProxyHost URL, we need to make
  // sure that cookies are not propagated, as evil.com could also be potentially
  // proxied via kProxyHost.
  const char kText[] = "* { pretty; }";

  ResponseHeaders orig_headers;
  DefaultResponseHeaders(kContentTypeCss, 100, &orig_headers);
  orig_headers.Add(HttpAttributes::kSetCookie, "tasty");
  SetFetchResponse("http://test.com/file.css", orig_headers, kText);

  ProxyUrlNamer url_namer;
  resource_manager()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/file.css"),
      true, &out_text, &out_headers);
  EXPECT_STREQ(kText, out_text);
  EXPECT_STREQ(NULL, out_headers.Lookup1(HttpAttributes::kSetCookie));
}

TEST_F(ProxyInterfaceTest, CrossDomainHeadersWithUncacheableResourceOnProxy) {
  // Check that we do not propagate cookies from test.com via kProxyHost URL,
  // as in CrossDomainHeaders above.  Also check that we do propagate cache
  // control.
  const char kText[] = "* { pretty; }";

  ResponseHeaders orig_headers;
  DefaultResponseHeaders(kContentTypeCss, 100, &orig_headers);
  orig_headers.Add(HttpAttributes::kSetCookie, "tasty");
  orig_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                 400000, ", private");
  orig_headers.ComputeCaching();
  SetFetchResponse("http://test.com/file.css", orig_headers, kText);

  ProxyUrlNamer url_namer;
  resource_manager()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/file.css"),
      true, &out_text, &out_headers);

  // Check that we ate the cookies.
  EXPECT_STREQ(kText, out_text);
  ConstStringStarVector values;
  out_headers.Lookup(HttpAttributes::kSetCookie, &values);
  EXPECT_EQ(0, values.size());

  // Check that the resource Cache-Control has been preserved.
  values.clear();
  out_headers.Lookup(HttpAttributes::kCacheControl, &values);
  ASSERT_EQ(2, values.size());
  EXPECT_STREQ("max-age=400", *values[0]);
  EXPECT_STREQ("private", *values[1]);
}

TEST_F(ProxyInterfaceTest, CrossDomainHeadersWithUncacheableResourceOnFetch) {
  // Check that we do not propagate cookies from test.com via a resource fetch,
  // as in CrossDomainHeaders above.  Also check that we do propagate cache
  // control, and that we run the filter specified in the resource fetch URL.
  // Note that the running of filters at present can only happen if
  // the filter is on the-fly.
  const char kText[] = "* { pretty; }";

  ResponseHeaders orig_headers;
  DefaultResponseHeaders(kContentTypeCss, 100, &orig_headers);
  orig_headers.Add(HttpAttributes::kSetCookie, "tasty");
  orig_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                 400000, ", private");
  orig_headers.ComputeCaching();
  SetFetchResponse("http://test.com/file.css", orig_headers, kText);

  ProxyUrlNamer url_namer;
  resource_manager()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(Encode(kTestDomain, "ce", "0", "file.css", "css"),
                 true, &out_text, &out_headers);

  // Check that we passed through the CSS.
  EXPECT_STREQ(kText, out_text);
  // Check that we ate the cookies.
  ConstStringStarVector values;
  out_headers.Lookup(HttpAttributes::kSetCookie, &values);
  EXPECT_EQ(0, values.size());

  // Check that the resource Cache-Control has been preserved.
  // max-age actually gets smaller, though, since this also triggers
  // a rewrite failure.
  values.clear();
  out_headers.Lookup(HttpAttributes::kCacheControl, &values);
  ASSERT_EQ(2, values.size());
  EXPECT_STREQ("max-age=300", *values[0]);
  EXPECT_STREQ("private", *values[1]);
}

TEST_F(ProxyInterfaceTest, CrossDomainHeadersWithUncacheableResourceOnFetch2) {
  // Variant of the above with a non-on-the-fly filter.
  const char kText[] = "* { pretty; }";

  ResponseHeaders orig_headers;
  DefaultResponseHeaders(kContentTypeCss, 100, &orig_headers);
  orig_headers.Add(HttpAttributes::kSetCookie, "tasty");
  orig_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                 400000, ", private");
  orig_headers.ComputeCaching();
  SetFetchResponse("http://test.com/file.css", orig_headers, kText);

  ProxyUrlNamer url_namer;
  resource_manager()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(Encode(kTestDomain, "cf", "0", "file.css", "css"),
                 true, &out_text, &out_headers);
  // Proper output
  EXPECT_STREQ("*{pretty}", out_text);

  // Private.
  ConstStringStarVector values;
  out_headers.Lookup(HttpAttributes::kCacheControl, &values);
  ASSERT_EQ(2, values.size());
  EXPECT_STREQ("max-age=400", *values[0]);
  EXPECT_STREQ("private", *values[1]);

  // Check that we ate the cookies.
  EXPECT_FALSE(out_headers.Has(HttpAttributes::kSetCookie));
}

TEST_F(ProxyInterfaceTest, ProxyResourceQueryOnly) {
  // At one point we had a bug where if we optimized a pagespeed resource
  // whose original name was a bare query, we would loop infinitely when
  // trying to fetch it from a separate-domain proxy.
  const char kUrl[] = "?somestuff";
  SetResponseWithDefaultHeaders(kUrl, kContentTypeJavascript,
                                "var a = 2;// stuff", kHtmlCacheTimeSec * 2);

  ProxyUrlNamer url_namer;
  resource_manager()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/",
             EncodeNormal("", "jm", "0", kUrl, "css")),
      true, &out_text, &out_headers);
  EXPECT_STREQ("var a=2;", out_text);
}

TEST_F(ProxyInterfaceTest, NoRehostIncompatMPS) {
  // Make sure we don't try to interpret a URL from an incompatible
  // mod_pagespeed version at our proxy host level.

  // This url will be rejected by CssUrlEncoder
  const char kOldName[] = "style.css.pagespeed.cf.0.css";
  const char kContent[] = "*     {}";
  SetResponseWithDefaultHeaders(kOldName, kContentTypeCss, kContent, 100);

  ProxyUrlNamer url_namer;
  resource_manager()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/",
             EncodeNormal("", "ce", "0", kOldName, "css")),
      true, &out_text, &out_headers);
  EXPECT_EQ(HttpStatus::kOK, out_headers.status_code());
  EXPECT_STREQ(kContent, out_text);
}

// Test that we serve "Cache-Control: no-store" only when original page did.
TEST_F(ProxyInterfaceTest, NoStore) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->set_max_html_cache_time_ms(0);
  resource_manager()->ComputeSignature(options);

  // Most headers get converted to "no-cache, max-age=0".
  EXPECT_STREQ("max-age=0, no-cache",
               RewriteHtmlCacheHeader("empty", ""));
  EXPECT_STREQ("max-age=0, no-cache",
               RewriteHtmlCacheHeader("private", "private, max-age=100"));
  EXPECT_STREQ("max-age=0, no-cache",
               RewriteHtmlCacheHeader("no-cache", "no-cache"));

  // Headers with "no-store", preserve that header as well.
  EXPECT_STREQ("max-age=0, no-cache, no-store",
               RewriteHtmlCacheHeader("no-store", "no-cache, no-store"));
  EXPECT_STREQ("max-age=0, no-cache, no-store",
               RewriteHtmlCacheHeader("no-store2", "no-store, max-age=300"));
}

}  // namespace

}  // namespace net_instaweb