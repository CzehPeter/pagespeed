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
#include "net/instaweb/automatic/public/proxy_fetch.h"
#include "net/instaweb/htmlparse/public/empty_html_filter.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/mock_callback.h"
#include "net/instaweb/http/public/mock_url_fetcher.h"
#include "net/instaweb/http/public/reflecting_test_fetcher.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/semantic_type.h"
#include "net/instaweb/http/public/user_agent_matcher.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/furious_util.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/resource_manager_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/test_rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/abstract_client_state.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/client_state.h"
#include "net/instaweb/util/public/delay_cache.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/mock_scheduler.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/null_message_handler.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/queued_worker_pool.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_synchronizer.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/time_util.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/worker_test_base.h"

namespace net_instaweb {

class HtmlFilter;
class MessageHandler;

namespace {

// This jpeg file lacks a .jpg or .jpeg extension.  So we initiate
// a property-cache read prior to getting the response-headers back,
// but will never go into the ProxyFetch flow that blocks waiting
// for the cache lookup to come back.
const char kImageFilenameLackingExt[] = "jpg_file_lacks_ext";
const char kPageUrl[] = "page.html";

const char kCssContent[] = "* { display: none; }";
const char kMinimizedCssContent[] = "*{display:none}";
const char kBackgroundFetchHeader[] = "X-Background-Fetch";
const char kFlushEarlyHtml[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"1.css\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"2.css\">"
    "<script src=\"1.js\"></script>"
    "<script src=\"2.js\"></script>"
    "<script src=\"private.js\"></script>"
    "</head>"
    "<body>"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"3.css\">"
    "</body>"
    "</html>";
const char kRewrittenHtml[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<script src=\"private.js\"></script>"
    "</head>"
    "<body>"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "</body>"
    "</html>";
const char kFlushEarlyRewrittenHtmlImageTag[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<script type=\"text/javascript\">(function(){"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";})()</script>"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 5</script>"
    "</head><head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<script src=\"private.js\"></script>"
    "</head>"
    "<body>"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "</body>"
    "</html>";
const char kFlushEarlyRewrittenHtmlLinkRelSubresource[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 5</script>"
    "</head><head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<script src=\"private.js\"></script>"
    "</head>"
    "<body>"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "</body>"
    "</html>";
const char kFlushEarlyRewrittenHtmlImageTagWithDeferJs[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<script type=\"text/javascript\">(function(){"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";})()</script>"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 3</script>"
    "</head><head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script>eval(mod_pagespeed_0);</script>"
    "<script>eval(mod_pagespeed_0);</script>"
    "<script src=\"private.js\"></script>"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "</body>"
    "</html>";
const char kFlushEarlyRewrittenHtmlLinkRelSubresourceWithDeferJs[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 3</script>"
    "</head><head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script>eval(mod_pagespeed_0);</script>"
    "<script>eval(mod_pagespeed_0);</script>"
    "<script src=\"private.js\"></script>"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "</body>"
    "</html>";

// Like ExpectStringAsyncFetch but for asynchronous invocation -- it lets
// one specify a WorkerTestBase::SyncPoint to help block until completion.
class AsyncExpectStringAsyncFetch : public ExpectStringAsyncFetch {
 public:
  AsyncExpectStringAsyncFetch(bool expect_success,
                              WorkerTestBase::SyncPoint* notify,
                              ThreadSynchronizer* sync)
      : ExpectStringAsyncFetch(expect_success),
        notify_(notify),
        sync_(sync) {
  }

  virtual ~AsyncExpectStringAsyncFetch() {}

  virtual void HandleHeadersComplete() {
    sync_->Wait(ProxyFetch::kHeadersSetupRaceWait);
    response_headers()->Add("HeadersComplete", "1");  // Dirties caching info.
    sync_->Signal(ProxyFetch::kHeadersSetupRaceFlush);
  }

  virtual void HandleDone(bool success) {
    ExpectStringAsyncFetch::HandleDone(success);
    notify_->Notify();
  }

 private:
  WorkerTestBase::SyncPoint* notify_;
  ThreadSynchronizer* sync_;

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
// proxy_fetch.
//
// This is used to check the flow for injecting data into filters via the
// ProxyInterface, including:
//     property_cache.
class MockFilter : public EmptyHtmlFilter {
 public:
  explicit MockFilter(RewriteDriver* driver)
      : driver_(driver),
        num_elements_(0),
        num_elements_property_(NULL) {
  }

  virtual void StartDocument() {
    num_elements_ = 0;
    PropertyCache* page_cache =
        driver_->resource_manager()->page_property_cache();
    const PropertyCache::Cohort* cohort =
        page_cache->GetCohort(RewriteDriver::kDomCohort);
    PropertyPage* page = driver_->property_page();
    if (page != NULL) {
      num_elements_property_ = page->GetProperty(cohort, "num_elements");
    } else {
      num_elements_property_ = NULL;
    }

    client_id_ = driver_->client_id();
    client_state_ = driver_->client_state();
    if (client_state_ != NULL) {
      // Set or clear the client state based on its current value, so we can
      // check whether it is being written back to the property cache correctly.
      if (!client_state_->InCache("http://www.fakeurl.com")) {
        client_state_->Set("http://www.fakeurl.com", 1000*1000);
      } else {
        client_state_->Clear();
      }
    }
  }

  virtual void StartElement(HtmlElement* element) {
    if (num_elements_ == 0) {
      // Before the start of the first element, print out the number
      // of elements that we expect based on the cache.
      GoogleString comment = " ";
      PropertyCache* page_cache =
          driver_->resource_manager()->page_property_cache();

      if (!client_id_.empty()) {
        StrAppend(&comment, "ClientID: ", client_id_, " ");
      }
      if (client_state_ != NULL) {
        StrAppend(&comment, "ClientStateID: ",
                  client_state_->ClientId(),
                  " InCache: ",
                  client_state_->InCache("http://www.fakeurl.com") ?
                  "true" : "false", " ");
      }
      if ((num_elements_property_ != NULL) &&
                 num_elements_property_->has_value()) {
        StrAppend(&comment, num_elements_property_->value(),
                  " elements ",
                  page_cache->IsStable(num_elements_property_)
                  ? "stable " : "unstable ");
      }
      HtmlNode* node = driver_->NewCommentNode(element->parent(), comment);
      driver_->InsertElementBeforeCurrent(node);
    }
    ++num_elements_;
  }

  virtual void EndDocument() {
    // We query IsCacheable for the HTML file only to ensure that
    // the test will crash if ComputeCaching() was never called.
    //
    // IsCacheable is true for HTML files because of kHtmlCacheTimeSec
    // above.
    EXPECT_TRUE(driver_->response_headers()->IsCacheable());

    if (num_elements_property_ != NULL) {
      PropertyCache* page_cache =
          driver_->resource_manager()->page_property_cache();
      page_cache->UpdateValue(IntegerToString(num_elements_),
                              num_elements_property_);
      num_elements_property_ = NULL;
    }
  }

  virtual const char* Name() const { return "MockFilter"; }

 private:
  RewriteDriver* driver_;
  int num_elements_;
  PropertyValue* num_elements_property_;
  GoogleString client_id_;
  AbstractClientState* client_state_;
};

// Hook provided to TestRewriteDriverFactory to add a new filter when
// a rewrite_driver is created.
class CreateFilterCallback
    : public TestRewriteDriverFactory::CreateFilterCallback {
 public:
  CreateFilterCallback() {}
  virtual ~CreateFilterCallback() {}

  virtual HtmlFilter* Done(RewriteDriver* driver) {
    return new MockFilter(driver);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(CreateFilterCallback);
};

// Subclass of AsyncFetch that adds a response header indicating whether the
// fetch is for a user-facing request, or a background rewrite.
class BackgroundFetchCheckingAsyncFetch : public SharedAsyncFetch {
 public:
  explicit BackgroundFetchCheckingAsyncFetch(AsyncFetch* base_fetch)
      : SharedAsyncFetch(base_fetch) {}
  virtual ~BackgroundFetchCheckingAsyncFetch() {}

  virtual void HandleHeadersComplete() {
    base_fetch()->HeadersComplete();
    response_headers()->Add(kBackgroundFetchHeader,
                            base_fetch()->IsBackgroundFetch() ? "1" : "0");
    // Call ComputeCaching again since Add sets cache_fields_dirty_ to true.
    response_headers()->ComputeCaching();
  }

  virtual void HandleDone(bool success) {
    base_fetch()->Done(success);
    delete this;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(BackgroundFetchCheckingAsyncFetch);
};

// Subclass of UrlAsyncFetcher that wraps the AsyncFetch with a
// BackgroundFetchCheckingAsyncFetch.
class BackgroundFetchCheckingUrlAsyncFetcher : public UrlAsyncFetcher {
 public:
  explicit BackgroundFetchCheckingUrlAsyncFetcher(UrlAsyncFetcher* fetcher)
      : base_fetcher_(fetcher),
        num_background_fetches_(0) {}
  virtual ~BackgroundFetchCheckingUrlAsyncFetcher() {}

  virtual bool Fetch(const GoogleString& url,
                     MessageHandler* message_handler,
                     AsyncFetch* fetch) {
    if (fetch->IsBackgroundFetch()) {
      num_background_fetches_++;
    }
    BackgroundFetchCheckingAsyncFetch* new_fetch =
        new BackgroundFetchCheckingAsyncFetch(fetch);
    return base_fetcher_->Fetch(url, message_handler, new_fetch);
  }

  int num_background_fetches() { return num_background_fetches_; }
  void clear_num_background_fetches() { num_background_fetches_ = 0; }

 private:
  UrlAsyncFetcher* base_fetcher_;
  int num_background_fetches_;
  DISALLOW_COPY_AND_ASSIGN(BackgroundFetchCheckingUrlAsyncFetcher);
};

}  // namespace

// TODO(morlovich): This currently relies on ResourceManagerTestBase to help
// setup fetchers; and also indirectly to prevent any rewrites from timing out
// (as it runs the tests with real scheduler but mock timer). It would probably
// be better to port this away to use TestRewriteDriverFactory directly.
class ProxyInterfaceTest : public ResourceManagerTestBase {
 public:
  // Helper function to run the fetch for ProxyInterfaceTest.HeadersSetupRace
  // in a thread so we can control it with signals using ThreadSynchronizer.
  //
  // It must be declared in the public section to be used in MakeFunction.
  void TestHeadersSetupRace() {
    mock_url_fetcher()->SetResponseFailure(AbsolutifyUrl(kPageUrl));
    TestPropertyCache(kPageUrl, true, true, false);
  }

 protected:
  static const int kHtmlCacheTimeSec = 5000;

  ProxyInterfaceTest()
      : max_age_300_("max-age=300"),
        request_start_time_ms_(-1),
        fetch_already_done_(false) {
    ConvertTimeToString(MockTimer::kApr_5_2010_ms, &start_time_string_);
    ConvertTimeToString(MockTimer::kApr_5_2010_ms + 5 * Timer::kMinuteMs,
                        &start_time_plus_300s_string_);
    ConvertTimeToString(MockTimer::kApr_5_2010_ms - 2 * Timer::kDayMs,
                        &old_time_string_);
  }
  virtual ~ProxyInterfaceTest() {}

  virtual void SetUp() {
    RewriteOptions* options = resource_manager()->global_options();
    resource_manager_->set_enable_property_cache(true);
    page_property_cache()->AddCohort(RewriteDriver::kDomCohort);
    resource_manager_->client_property_cache()->AddCohort(
        ClientState::kClientStateCohort);
    options->ClearSignatureForTesting();
    options->EnableFilter(RewriteOptions::kRewriteCss);
    options->set_max_html_cache_time_ms(kHtmlCacheTimeSec * Timer::kSecondMs);
    options->set_ajax_rewriting_enabled(true);
    options->Disallow("*blacklist*");
    resource_manager()->ComputeSignature(options);
    ResourceManagerTestBase::SetUp();
    ProxyInterface::Initialize(statistics());
    // The original url_async_fetcher() is still owned by RewriteDriverFactory.
    background_fetch_fetcher_.reset(new BackgroundFetchCheckingUrlAsyncFetcher(
        factory()->ComputeUrlAsyncFetcher()));
    resource_manager()->set_default_system_fetcher(
        background_fetch_fetcher_.get());
    proxy_interface_.reset(
        new ProxyInterface("localhost", 80, resource_manager(), statistics()));
    start_time_ms_ = mock_timer()->NowMs();

    SetResponseWithDefaultHeaders(kImageFilenameLackingExt, kContentTypeJpeg,
                                  "image data", 300);
    SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                  "<div><p></p></div>", 0);
  }

  virtual void TearDown() {
    // Make sure all the jobs are over before we check for leaks ---
    // someone might still be trying to clean themselves up.
    mock_scheduler()->AwaitQuiescence();
    EXPECT_EQ(0, resource_manager()->num_active_rewrite_drivers());
    ResourceManagerTestBase::TearDown();
  }

  // Initiates a fetch using the proxy interface, and waits for it to
  // complete.
  void FetchFromProxy(const StringPiece& url,
                      const RequestHeaders& request_headers,
                      bool expect_success,
                      GoogleString* string_out,
                      ResponseHeaders* headers_out) {
    FetchFromProxyNoWait(url, request_headers, expect_success, headers_out);
    WaitForFetch();
    *string_out = callback_->buffer();
    logging_info_.CopyFrom(*callback_->logging_info());
  }

  // TODO(jmarantz): eliminate this interface as it's annoying to have
  // the function overload just to save an empty RequestHeaders arg.
  void FetchFromProxy(const StringPiece& url,
                      bool expect_success,
                      GoogleString* string_out,
                      ResponseHeaders* headers_out) {
    RequestHeaders request_headers;
    FetchFromProxy(url, request_headers, expect_success, string_out,
                   headers_out);
  }

  // Initiates a fetch using the proxy interface, without waiting for it to
  // complete.  The usage model here is to delay callbacks and/or fetches
  // to control their order of delivery, then call WaitForFetch.
  void FetchFromProxyNoWait(const StringPiece& url,
                            const RequestHeaders& request_headers,
                            bool expect_success,
                            ResponseHeaders* headers_out) {
    sync_.reset(new WorkerTestBase::SyncPoint(
        resource_manager()->thread_system()));
    callback_.reset(new AsyncExpectStringAsyncFetch(
        expect_success, sync_.get(),
        resource_manager()->thread_synchronizer()));
    callback_->set_response_headers(headers_out);
    callback_->request_headers()->CopyFrom(request_headers);
    fetch_already_done_ = proxy_interface_->Fetch(AbsolutifyUrl(url),
                                                  message_handler(),
                                                  callback_.get());
    if (fetch_already_done_) {
      EXPECT_TRUE(callback_->done());
    }
  }

  // This must be called after FetchFromProxyNoWait, once all of the required
  // resources (fetches, cache lookups) have been released.
  void WaitForFetch() {
    if (!fetch_already_done_) {
      sync_->Wait();
    }
    mock_scheduler()->AwaitQuiescence();
  }

  void CheckHeaders(const ResponseHeaders& headers,
                    const ContentType& expect_type) {
    ASSERT_TRUE(headers.has_status_code());
    EXPECT_EQ(HttpStatus::kOK, headers.status_code());
    EXPECT_STREQ(expect_type.mime_type(),
                 headers.Lookup1(HttpAttributes::kContentType));
  }

  void CheckBackgroundFetch(const ResponseHeaders& headers,
                            bool is_background_fetch) {
    EXPECT_STREQ(is_background_fetch ? "1" : "0",
              headers.Lookup1(kBackgroundFetchHeader));
  }

  void CheckNumBackgroundFetches(int num) {
    EXPECT_EQ(num, background_fetch_fetcher_->num_background_fetches());
  }

  virtual void ClearStats() {
    ResourceManagerTestBase::ClearStats();
    background_fetch_fetcher_->clear_num_background_fetches();
  }

  RewriteOptions* GetCustomOptions(const StringPiece& url,
                                   RequestHeaders* request_headers,
                                   RewriteOptions* domain_options) {
    // The default url_namer does not yield any name-derived options, and we
    // have not specified any URL params or request-headers, so there will be
    // no custom options, and no errors.
    GoogleUrl gurl(url);
    RewriteOptions* copy_options = domain_options != NULL ?
        domain_options->Clone() : NULL;
    ProxyInterface::OptionsBoolPair query_options_success =
        proxy_interface_->GetQueryOptions(&gurl, request_headers,
                                          message_handler());
    EXPECT_TRUE(query_options_success.second);
    RewriteOptions* options =
        proxy_interface_->GetCustomOptions(&gurl, request_headers, copy_options,
                                           query_options_success.first,
                                           message_handler());
    return options;
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

  // Tests a single flow through the property-cache, optionally delaying or
  // threading property-cache lookups, and using the ThreadSynchronizer to
  // tease out race conditions.
  //
  // delay_pcache indicates that we will suspend the PropertyCache lookup
  // until after the FetchFromProxy call.  This is used in the
  // PropCacheNoWritesIfNonHtmlDelayedCache below, which tests the flow where
  // we have already detached the ProxyFetchPropertyCallbackCollector before
  // Done() is called.
  //
  // thread_pcache forces the property-cache to issue the lookup
  // callback in a different thread.  This lets us reproduce a
  // potential race conditions where a context switch in
  // ProxyFetchPropertyCallbackCollector::Done() would lead to a
  // double-deletion of the collector object.
  void TestPropertyCache(const StringPiece& url,
                         bool delay_pcache, bool thread_pcache,
                         bool expect_success) {
    scoped_ptr<QueuedWorkerPool> pool;
    QueuedWorkerPool::Sequence* sequence = NULL;

    ThreadSynchronizer* sync = resource_manager()->thread_synchronizer();
    GoogleString delay_pcache_key, delay_http_cache_key;
    if (delay_pcache || thread_pcache) {
      PropertyCache* pcache = page_property_cache();
      const PropertyCache::Cohort* cohort =
          pcache->GetCohort(RewriteDriver::kDomCohort);
      delay_http_cache_key = AbsolutifyUrl(url);
      delay_pcache_key = pcache->CacheKey(delay_http_cache_key, cohort);
      delay_cache()->DelayKey(delay_pcache_key);
      if (thread_pcache) {
        delay_cache()->DelayKey(delay_http_cache_key);
        pool.reset(new QueuedWorkerPool(
            1, resource_manager()->thread_system()));
        sequence = pool->NewSequence();
      }
    }

    CreateFilterCallback create_filter_callback;
    factory()->AddCreateFilterCallback(&create_filter_callback);

    GoogleString image_out;
    ResponseHeaders headers_out;

    if (thread_pcache) {
      RequestHeaders request_headers;
      FetchFromProxyNoWait(url, request_headers, expect_success, &headers_out);
      delay_cache()->ReleaseKeyInSequence(delay_pcache_key, sequence);

      // Wait until the property-cache-thread is in
      // ProxyFetchPropertyCallbackCollector::Done(), just after the
      // critical section when it will signal kCollectorReady, and
      // then block waiting for the test (in mainline) to signal
      // kCollectorDone.
      sync->Wait(ProxyFetch::kCollectorReady);

      // Now release the HTTPCache lookup, which allows the mock-fetch
      // to stream the bytes in the ProxyFetch and call HandleDone().
      // Note that we release this key in mainline, so that call
      // sequence happens directly from ReleaseKey.
      delay_cache()->ReleaseKey(delay_http_cache_key);

      // Now we can release the property-cache thread.
      sync->Signal(ProxyFetch::kCollectorDone);
      WaitForFetch();
      pool->ShutDown();
    } else {
      FetchFromProxy(url, expect_success, &image_out, &headers_out);
      if (delay_pcache) {
        delay_cache()->ReleaseKey(delay_pcache_key);
      }
    }

    EXPECT_EQ(1, lru_cache()->num_inserts());  // http-cache
    EXPECT_EQ(2, lru_cache()->num_misses());   // http-cache & prop-cache
  }

  void TestOptionsUsedInCacheKey() {
    GoogleUrl gurl("http://www.test.com/");
    StringAsyncFetch callback;
    scoped_ptr<ProxyFetchPropertyCallbackCollector> callback_collector(
        proxy_interface_->InitiatePropertyCacheLookup(
        false, gurl, options(), &callback));
    EXPECT_NE(static_cast<ProxyFetchPropertyCallbackCollector*>(NULL),
              callback_collector.get());
    PropertyPage* page = callback_collector->GetPropertyPageWithoutOwnership(
        ProxyFetchPropertyCallback::kPagePropertyCache);
    EXPECT_NE(static_cast<PropertyPage*>(NULL), page);
    resource_manager()->ComputeSignature(options());
    GoogleString expected = StrCat(gurl.Spec(), "_", options()->signature());
    EXPECT_EQ(expected, page->key());
  }

  void DisableAjax() {
    RewriteOptions* options = resource_manager()->global_options();
    options->ClearSignatureForTesting();
    options->set_ajax_rewriting_enabled(false);
    resource_manager()->ComputeSignature(options);
  }

  void RejectBlacklisted() {
    RewriteOptions* options = resource_manager()->global_options();
    options->ClearSignatureForTesting();
    options->set_reject_blacklisted(true);
    options->set_reject_blacklisted_status_code(HttpStatus::kImATeapot);
    resource_manager()->ComputeSignature(options);
  }

  void SetupForFlushEarlyFlow(bool enable_experimental) {
    // Setup
    ResponseHeaders headers;
    headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
    headers.SetStatusAndReason(HttpStatus::kOK);
    mock_url_fetcher_.SetResponse(kTestDomain, headers, kFlushEarlyHtml);

    // Enable FlushSubresourcesFilter filter.
    RewriteOptions* rewrite_options = resource_manager()->global_options();
    rewrite_options->ClearSignatureForTesting();
    rewrite_options->EnableFilter(RewriteOptions::kFlushSubresources);
    rewrite_options->EnableFilter(RewriteOptions::kCombineCss);
    rewrite_options->EnableFilter(RewriteOptions::kCombineJavascript);
    rewrite_options->set_enable_flush_subresources_experimental(
        enable_experimental);
    rewrite_options->EnableExtendCacheFilters();
    // Disabling the inline filters so that the resources get flushed early
    // else our dummy resources are too small and always get inlined.
    rewrite_options->DisableFilter(RewriteOptions::kInlineCss);
    rewrite_options->DisableFilter(RewriteOptions::kInlineJavascript);
    rewrite_options->ComputeSignature(hasher());

    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.css"), kContentTypeCss,
                                  kCssContent, kHtmlCacheTimeSec * 2);
    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "2.css"), kContentTypeCss,
                                  kCssContent, kHtmlCacheTimeSec * 2);
    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "3.css"), kContentTypeCss,
                                  kCssContent, kHtmlCacheTimeSec * 2);
    const char kContent[] = "function f() {alert('foo');}";
    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.js"),
                                  kContentTypeJavascript, kContent,
                                  kHtmlCacheTimeSec * 2);
    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "2.js"),
                                  kContentTypeJavascript, kContent,
                                  kHtmlCacheTimeSec * 2);
    ResponseHeaders private_headers;
    DefaultResponseHeaders(kContentTypeJavascript, kHtmlCacheTimeSec,
                           &private_headers);
    private_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                      300 * Timer::kSecondMs, ", private");
    private_headers.ComputeCaching();
    SetFetchResponse(AbsolutifyUrl("private.js"), private_headers, "a");
  }

  GoogleString FlushEarlyRewrittenHtml(
      UserAgentMatcher::PrefetchMechanism value, bool enable_experimental,
      bool defer_js_enabled) {
    GoogleString domain_prefix = enable_experimental ? "" : kTestDomain;
    GoogleString rewritten_css_url_1 = Encode("", "cf", "0", "1.css", "css");
    GoogleString rewritten_css_url_2 = Encode("", "cf", "0", "2.css", "css");
    GoogleString rewritten_css_url_3 = Encode("", "cf", "0", "3.css", "css");
    GoogleString rewritten_js_url_1 = Encode("", "jm", "0", "1.js", "js");
    GoogleString rewritten_js_url_2 = Encode("", "jm", "0", "2.js", "js");
    GoogleString combined_js_url =
        "1.js,Mjm.0.js+2.js,Mjm.0.js.pagespeed.jc.0.js";
    if (value == UserAgentMatcher::kPrefetchNotSupported) {
      return StringPrintf(kRewrittenHtml, rewritten_css_url_1.data(),
                          rewritten_css_url_2.data(), rewritten_js_url_1.data(),
                          rewritten_js_url_2.data(),
                          rewritten_css_url_3.data());
    } else if (defer_js_enabled) {
      GoogleString redirect_url = StrCat(kTestDomain, "?ModPagespeed=noscript");
      return StringPrintf(
          value == UserAgentMatcher::kPrefetchLinkRelSubresource ?
          kFlushEarlyRewrittenHtmlLinkRelSubresourceWithDeferJs :
          kFlushEarlyRewrittenHtmlImageTagWithDeferJs,
          StrCat(domain_prefix, rewritten_css_url_1).data(),
          StrCat(domain_prefix, rewritten_css_url_2).data(),
          StrCat(domain_prefix, rewritten_css_url_3).data(),
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          combined_js_url.data(),
          StringPrintf(kNoScriptRedirectFormatter, redirect_url.c_str(),
                       redirect_url.c_str()).c_str(),
          rewritten_css_url_3.data());
    } else {
      return StringPrintf(
          value == UserAgentMatcher::kPrefetchLinkRelSubresource ?
          kFlushEarlyRewrittenHtmlLinkRelSubresource :
          kFlushEarlyRewrittenHtmlImageTag,
          StrCat(domain_prefix, rewritten_css_url_1).data(),
          StrCat(domain_prefix, rewritten_css_url_2).data(),
          StrCat(domain_prefix, rewritten_js_url_1).data(),
          StrCat(domain_prefix, rewritten_js_url_2).data(),
          StrCat(domain_prefix, rewritten_css_url_3).data(),
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          rewritten_js_url_1.data(), rewritten_js_url_2.data(),
          rewritten_css_url_3.data());
    }
  }

  scoped_ptr<ProxyInterface> proxy_interface_;
  scoped_ptr<BackgroundFetchCheckingUrlAsyncFetcher> background_fetch_fetcher_;
  int64 start_time_ms_;
  GoogleString start_time_string_;
  GoogleString start_time_plus_300s_string_;
  GoogleString old_time_string_;
  LoggingInfo logging_info_;
  const GoogleString max_age_300_;
  int64 request_start_time_ms_;

  bool fetch_already_done_;
  scoped_ptr<WorkerTestBase::SyncPoint> sync_;
  scoped_ptr<AsyncExpectStringAsyncFetch> callback_;

 private:
  friend class FilterCallback;

  DISALLOW_COPY_AND_ASSIGN(ProxyInterfaceTest);
};

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTest) {
  SetupForFlushEarlyFlow(false);
  GoogleString text;
  RequestHeaders request_headers;
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  // Check total number of cache inserts.
  // 6 for 1.css, 2.css, 3.css, 1.js, 2.js and private.js.
  // 5 for I.1.css.pagespeed.cf.0.css, I.2.css.pagespeed.cf.0.css,
  //       I.3.css.pagespeed.cf.0.css, 1.js.pagespeed.jm.0.js and
  //       2.js.pagespeed.jm.0.js and
  // 12 metadata cache entries - three for cf and jm, six for ce.
  // 1 for DomCohort write in property cache.
  EXPECT_EQ(24, lru_cache()->num_inserts());

  // Fetch the url again. This time FlushEarlyFlow should not be triggered.
  // None
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchNotSupported, false, false), text);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestPrefetch) {
  SetupForFlushEarlyFlow(false);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_rel_subresource");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  // Check total number of cache inserts.
  // 6 for 1.css, 2.css, 3.css, 1.js, 2.js and private.js.
  // 5 for I.1.css.pagespeed.cf.0.css, I.2.css.pagespeed.cf.0.css,
  //       I.3.css.pagespeed.cf.0.css, 1.js.pagespeed.jm.0.js and
  //       2.js.pagespeed.jm.0.js
  // 18 metadata cache enties - three for cf and jm, six each for ce and
  //       fs.
  // 1 for DomCohort write in property cache.
  EXPECT_EQ(30, lru_cache()->num_inserts());

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkRelSubresource, false, false), text);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestImageTag) {
  SetupForFlushEarlyFlow(false);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  // Check total number of cache inserts.
  // 6 for 1.css, 2.css, 3.css, 1.js, 2.js and private.js.
  // 5 for I.1.css.pagespeed.cf.0.css, I.2.css.pagespeed.cf.0.css,
  //       I.3.css.pagespeed.cf.0.css, 1.js.pagespeed.jm.0.js and
  //       2.js.pagespeed.jm.0.js
  // 18 metadata cache enties - three for cf and jm, six each for ce and
  //       fs.
  // 1 for DomCohort write in property cache.
  EXPECT_EQ(30, lru_cache()->num_inserts());

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchImageTag, false, false), text);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestWithDeferJsImageTag) {
  SetupForFlushEarlyFlow(false);
  scoped_ptr<RewriteOptions> custom_options(
      resource_manager()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  resource_manager()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchImageTag, false, true), text);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestWithDeferJsPreferch) {
  SetupForFlushEarlyFlow(false);
  scoped_ptr<RewriteOptions> custom_options(
      resource_manager()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  resource_manager()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_rel_subresource");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkRelSubresource, false, true), text);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTest) {
  SetupForFlushEarlyFlow(true);
  GoogleString text;
  RequestHeaders request_headers;
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should not be triggered.
  // None
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchNotSupported, true, false), text);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestPrefetch) {
  SetupForFlushEarlyFlow(true);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_rel_subresource");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  // Chrome
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkRelSubresource, true, false), text);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestImageTag) {
  SetupForFlushEarlyFlow(true);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchImageTag, true, false), text);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestWithDeferJsImageTag) {
  SetupForFlushEarlyFlow(true);
  scoped_ptr<RewriteOptions> custom_options(
      resource_manager()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  resource_manager()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchImageTag, true, true), text);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestWithDeferJsPreferch) {
  SetupForFlushEarlyFlow(true);
  scoped_ptr<RewriteOptions> custom_options(
      resource_manager()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  resource_manager()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_rel_subresource");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkRelSubresource, true, true), text);
}


TEST_F(ProxyInterfaceTest, LoggingInfo) {
  GoogleString url = "http://www.example.com/";
  GoogleString text;
  RequestHeaders request_headers;
  ResponseHeaders headers;
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  mock_url_fetcher_.SetResponse("http://www.example.com/", headers,
                                "<html></html>");

  FetchFromProxy(url, request_headers, true, &text, &headers);
  CheckBackgroundFetch(headers, false);
  CheckNumBackgroundFetches(0);
  const TimingInfo timing_info_ = logging_info_.timing_info();
  ASSERT_TRUE(timing_info_.has_cache1_ms());
  EXPECT_EQ(timing_info_.cache1_ms(), 0);
  EXPECT_FALSE(timing_info_.has_cache2_ms());
  EXPECT_FALSE(timing_info_.has_header_fetch_ms());
  EXPECT_FALSE(timing_info_.has_fetch_ms());
}

TEST_F(ProxyInterfaceTest, HeadRequest) {
  // Test to check if we are handling Head requests correctly.
  GoogleString url = "http://www.example.com/";
  GoogleString set_text, get_text;
  RequestHeaders request_headers;
  ResponseHeaders set_headers, get_headers;

  set_headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  set_headers.SetStatusAndReason(HttpStatus::kOK);

  set_text = "<html></html>";

  mock_url_fetcher_.SetResponse("http://www.example.com/", set_headers,
                                set_text);
  FetchFromProxy(url, request_headers, true, &get_text, &get_headers);

  // Headers and body are correct for a Get request.
  EXPECT_EQ("HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "X-Background-Fetch: 0\r\n"
            "Date: Tue, 02 Feb 2010 18:51:26 GMT\r\n"
            "Expires: Tue, 02 Feb 2010 18:51:26 GMT\r\n"
            "Cache-Control: max-age=0, private\r\n"
            "X-Page-Speed: \r\n"
            "HeadersComplete: 1\r\n\r\n", get_headers.ToString());
  EXPECT_EQ(set_text, get_text);

  // Headers and body are correct for a Head request.
  request_headers.set_method(RequestHeaders::kHead);
  FetchFromProxy(url, request_headers, true, &get_text, &get_headers);

  EXPECT_EQ("HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "X-Background-Fetch: 0\r\n"
            "X-Page-Speed: \r\n"
            "HeadersComplete: 1\r\n\r\n", get_headers.ToString());
  EXPECT_TRUE(get_text.empty());
}

TEST_F(ProxyInterfaceTest, HeadResourceRequest) {
  // Test to check if we are handling Head requests correctly in pagespeed
  // resource flow.
  const char kCssWithEmbeddedImage[] = "*{background-image:url(%s)}";
  const char kBackgroundImage[] = "1.png";

  GoogleString text;
  RequestHeaders request_headers;
  ResponseHeaders response_headers;
  GoogleString expected_response_headers_string = "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/css\r\n"
      "X-Background-Fetch: 0\r\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\r\n"
      "Expires: Tue, 02 Feb 2010 18:56:26 GMT\r\n"
      "Cache-Control: max-age=300,private\r\n"
      "X-Page-Speed: \r\n"
      "HeadersComplete: 1\r\n\r\n";

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

  // Because cache-extension was turned off, the image in the CSS file
  // will not be changed.
  FetchFromProxy("I.embedded.css.pagespeed.cf.0.css", request_headers,
                 true, &text, &response_headers);
  EXPECT_EQ(expected_response_headers_string, response_headers.ToString());
  EXPECT_EQ(orig_css, text);
  // Headers and body are correct for a Head request.
  request_headers.set_method(RequestHeaders::kHead);
  FetchFromProxy("I.embedded.css.pagespeed.cf.0.css", request_headers,
                 true, &text, &response_headers);

  // This leads to a conditional refresh of the original resource.
  expected_response_headers_string = "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/css\r\n"
      "X-Background-Fetch: 0\r\n"
      "Etag: W/PSA-0\r\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\r\n"
      "Expires: Tue, 02 Feb 2010 18:56:26 GMT\r\n"
      "Cache-Control: max-age=300,private\r\n"
      "X-Page-Speed: \r\n"
      "HeadersComplete: 1\r\n\r\n";

  EXPECT_EQ(expected_response_headers_string, response_headers.ToString());
  EXPECT_TRUE(text.empty());
}

TEST_F(ProxyInterfaceTest, FetchFailure) {
  GoogleString text;
  ResponseHeaders headers;

  // We don't want fetcher to fail the test, merely the fetch.
  SetFetchFailOnUnexpected(false);
  FetchFromProxy("invalid", false, &text, &headers);
  CheckBackgroundFetch(headers, false);
  CheckNumBackgroundFetches(0);
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
  CheckBackgroundFetch(headers, false);
  CheckNumBackgroundFetches(0);
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

TEST_F(ProxyInterfaceTest, NotCachedIfAuthorizedAndNotPublic) {
  // We should not cache things which are default cache-control if we
  // are sending Authorization:. See RFC 2616, 14.8.
  ReflectingTestFetcher reflect;
  resource_manager()->set_default_system_fetcher(&reflect);

  RequestHeaders request_headers;
  request_headers.Add("Was", "Here");
  request_headers.Add(HttpAttributes::kAuthorization, "Secret");
  // This will get reflected as well, and hence will determine whether
  // cacheable or not.
  request_headers.Replace(HttpAttributes::kCacheControl, "max-age=600000");

  ResponseHeaders out_headers;
  GoogleString out_text;
  // Using .txt here so we don't try any AJAX rewriting.
  FetchFromProxy("http://test.com/file.txt",
                 request_headers,  true, &out_text, &out_headers);
  // We should see the request headers we sent back as the response headers
  // as we're using a ReflectingTestFetcher.
  EXPECT_STREQ("Here", out_headers.Lookup1("Was"));

  // Not cross-domain, so should propagate out header.
  EXPECT_TRUE(out_headers.Has(HttpAttributes::kAuthorization));

  // Should not have written anything to cache, due to the authorization
  // header.
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());

  ClearStats();

  // Now try again. This time no authorization header, different 'Was'.
  request_headers.Replace("Was", "There");
  request_headers.RemoveAll(HttpAttributes::kAuthorization);

  FetchFromProxy("http://test.com/file.txt",
                 request_headers,  true, &out_text, &out_headers);
  // Should get different headers since we should not be cached.
  EXPECT_STREQ("There", out_headers.Lookup1("Was"));
  EXPECT_FALSE(out_headers.Has(HttpAttributes::kAuthorization));

  // And should be a miss per stats.
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());

  mock_scheduler()->AwaitQuiescence();
}

TEST_F(ProxyInterfaceTest, CachedIfAuthorizedAndPublic) {
  // This with Cache-Control: public should be cached even if
  // we are sending Authorization:. See RFC 2616.
  ReflectingTestFetcher reflect;
  resource_manager()->set_default_system_fetcher(&reflect);

  RequestHeaders request_headers;
  request_headers.Add("Was", "Here");
  request_headers.Add(HttpAttributes::kAuthorization, "Secret");
  // This will get reflected as well, and hence will determine whether
  // cacheable or not.
  request_headers.Replace(HttpAttributes::kCacheControl, "max-age=600000");
  request_headers.Add(HttpAttributes::kCacheControl, "public");  // unlike above

  ResponseHeaders out_headers;
  GoogleString out_text;
  // Using .txt here so we don't try any AJAX rewriting.
  FetchFromProxy("http://test.com/file.txt",
                 request_headers,  true, &out_text, &out_headers);
  EXPECT_STREQ("Here", out_headers.Lookup1("Was"));

  // Not cross-domain, so should propagate out header.
  EXPECT_TRUE(out_headers.Has(HttpAttributes::kAuthorization));

  // Should have written the result to the cache, despite the request having
  // Authorization: thanks to cache-control: public,
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());

  ClearStats();

  // Now try again. This time no authorization header, different 'Was'.
  request_headers.Replace("Was", "There");
  request_headers.RemoveAll(HttpAttributes::kAuthorization);

  FetchFromProxy("http://test.com/file.txt",
                 request_headers,  true, &out_text, &out_headers);
  // Should get old headers, since original was cacheable.
  EXPECT_STREQ("Here", out_headers.Lookup1("Was"));

  // ... of course hopefully a real server won't serve secrets on a
  // cache-control: public page.
  EXPECT_STREQ("Secret", out_headers.Lookup1(HttpAttributes::kAuthorization));

  // And should be a hit per stats.
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());

  mock_scheduler()->AwaitQuiescence();
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

TEST_F(ProxyInterfaceTest, CacheableSize) {
  // Test to check that we are not caching responses which have content length >
  // max_cacheable_response_content_length.
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  mock_timer()->SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.SetDateAndCaching(MockTimer::kApr_5_2010_ms, 300 * Timer::kSecondMs);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);

  // Set the set_max_cacheable_response_content_length to 10 bytes.
  http_cache()->set_max_cacheable_response_content_length(10);

  // Fetch once.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.html", true, &text, &response_headers);

  // One lookup for ajax metadata, one for the HTTP response and one for the
  // property cache entry. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_inserts());

  // Fetch again. It has the same caching headers.
  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  // None are found as the size is bigger than
  // max_cacheable_response_content_length.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  // Set the set_max_cacheable_response_content_length to 1024 bytes.
  http_cache()->set_max_cacheable_response_content_length(1024);
  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);
  // None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_inserts());

  // Fetch again.
  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  // One hit for the HTTP response as content is smaller than
  // max_cacheable_response_content_length.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, CacheableSizeAjax) {
  // Test to check that we are not caching responses which have content length >
  // max_cacheable_response_content_length in Ajax flow.
  ResponseHeaders headers;
  mock_timer()->SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kCssContent);

  http_cache()->set_max_cacheable_response_content_length(0);
  // The first response served by the fetcher and is not rewritten. An ajax
  // rewrite should not be triggered as the content length is greater than
  // max_cacheable_response_content_length.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_EQ(kCssContent, text);
  // One lookup for ajax metadata, one for the HTTP response. None are found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_inserts());

  ClearStats();
  // Fetch again. Optimized version is not served.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_EQ(kCssContent, text);
  // None are found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
}

TEST_F(ProxyInterfaceTest, CacheableSizeResource) {
  // Test to check that we are not caching responses which have content length >
  // max_cacheable_response_content_length in resource flow.
  GoogleString text;
  ResponseHeaders headers;

  // Fetching of a rewritten resource we did not just create
  // after an HTML rewrite.
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);
  // Set the set_max_cacheable_response_content_length to 0 bytes.
  http_cache()->set_max_cacheable_response_content_length(0);
  // Fetch fails as original is not accessible.
  FetchFromProxy(Encode("", "cf", "0", "a.css", "css"), false, &text, &headers);
}

TEST_F(ProxyInterfaceTest, InvalidationForCacheableHtml) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  mock_timer()->SetTimeUs(MockTimer::kApr_5_2010_ms * Timer::kMsUs);
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.SetDateAndCaching(MockTimer::kApr_5_2010_ms, 300 * Timer::kSecondMs);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);

  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata, one for the HTTP response and one for the
  // property cache entry. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());

  // Change the response.
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, "new");

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We continue to serve the previous response since we've cached it.
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());

  // Invalidate the cache.
  scoped_ptr<RewriteOptions> custom_options(
      resource_manager()->global_options()->Clone());
  custom_options->set_cache_invalidation_timestamp(mock_timer()->NowMs());
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  resource_manager()->set_url_namer(&url_namer);

  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We get the new response since we've invalidated the cache.
  EXPECT_EQ("new", text);
  // The HTTP response is found in the LRU cache but counts as a miss in the
  // HTTPCache since it has been invalidated. Also, cache misses for the ajax
  // metadata and property cache entry.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, UrlInvalidationForCacheableHtml) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  mock_timer()->SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.SetDateAndCaching(MockTimer::kApr_5_2010_ms, 300 * Timer::kSecondMs);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);

  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata, one for the HTTP response and one for the
  // property cache entry. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());

  // Change the response.
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, "new");

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We continue to serve the previous response since we've cached it.
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());


  // Invalidate the cache for some URL other than 'text.html'.
  scoped_ptr<RewriteOptions> custom_options_1(
      resource_manager()->global_options()->Clone());
  custom_options_1->AddUrlCacheInvalidationEntry(
      AbsolutifyUrl("foo.bar"), mock_timer()->NowMs(), true);
  ProxyUrlNamer url_namer_1;
  url_namer_1.set_options(custom_options_1.get());
  resource_manager()->set_url_namer(&url_namer_1);

  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We continue to serve the previous response since we've cached it.
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());


  // Invalidate the cache.
  scoped_ptr<RewriteOptions> custom_options_2(
      resource_manager()->global_options()->Clone());
  // Strictness of URL cache invalidation entry (last argument below) does not
  // matter in this test since there is nothing cached in metadata or property
  // caches.
  custom_options_2->AddUrlCacheInvalidationEntry(
      AbsolutifyUrl("text.html"), mock_timer()->NowMs(), true);
  ProxyUrlNamer url_namer_2;
  url_namer_2.set_options(custom_options_2.get());
  resource_manager()->set_url_namer(&url_namer_2);

  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We get the new response since we've invalidated the cache.
  EXPECT_EQ("new", text);
  // The HTTP response is found in the LRU cache but counts as a miss in the
  // HTTPCache since it has been invalidated. Also, cache misses for the ajax
  // metadata and property cache entry.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());
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
  // Lookups for: (1) ajax metadata (2) HTTP response (3) Property cache.
  // None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
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
  // Lookups for: (1) ajax metadata (2) HTTP response (3) Property cache.
  // None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
}

TEST_F(ProxyInterfaceTest, ModifiedImplicitCachingHeadersForCss) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->set_implicit_cache_ttl_ms(500 * Timer::kSecondMs);
  resource_manager()->ComputeSignature(options);

  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  mock_timer()->SetTimeUs(MockTimer::kApr_5_2010_ms * Timer::kMsUs);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  // Do not call ComputeCaching before calling SetFetchResponse because it will
  // add an explicit max-age=300 cache control header. We do not want that
  // header in this test.
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kContent);

  // The first response served by the fetcher has caching headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.css", true, &text, &response_headers);

  GoogleString max_age_500 = "max-age=500";
  GoogleString start_time_plus_500s_string;
  ConvertTimeToString(MockTimer::kApr_5_2010_ms + 500 * Timer::kSecondMs,
                      &start_time_plus_500s_string);

  EXPECT_STREQ(max_age_500,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_500s_string,
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

  EXPECT_STREQ(max_age_500,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_500s_string,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One hit for ajax metadata and one for the HTTP response.
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
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
  CheckBackgroundFetch(response_headers, false);
  CheckNumBackgroundFetches(0);
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
  CheckNumBackgroundFetches(0);

  ClearStats();
  // Advance close to expiry.
  mock_timer()->AdvanceUs(270 * Timer::kSecondUs);
  // The rewrite is complete and the optimized version is served. A freshen is
  // triggered to refresh the original CSS file.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ("max-age=30",
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ("Mon, 05 Apr 2010 18:55:56 GMT",
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kMinimizedCssContent, text);
  // One hit for ajax metadata, one for the rewritten HTTP response and one for
  // the original HTTP response while freshening.
  EXPECT_EQ(3, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
  // One background fetch is triggered while freshening.
  CheckNumBackgroundFetches(1);

  // Disable ajax rewriting. We now received the response fetched while
  // freshening. This response has kBackgroundFetchHeader set to 1.
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->set_ajax_rewriting_enabled(false);
  resource_manager()->ComputeSignature(options);

  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);
  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ("Mon, 05 Apr 2010 19:00:56 GMT",
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ("Mon, 05 Apr 2010 18:55:56 GMT",
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kCssContent, text);
  CheckNumBackgroundFetches(0);
  CheckBackgroundFetch(response_headers, true);
  // Done HTTP cache hit for the original response.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, NoAjaxRewritingWhenAuthorizationSent) {
  // We should not do ajax rewriting when sending over an authorization
  // header if the original isn't cache-control: public.
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
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kAuthorization, "Paperwork");
  FetchFromProxy("text.css", request_headers, true, &text, &response_headers);
  EXPECT_EQ(kCssContent, text);

  // The second version should still be unoptimized, since original wasn't
  // cacheable.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", request_headers, true, &text, &response_headers);
  EXPECT_EQ(kCssContent, text);
}

TEST_F(ProxyInterfaceTest, AjaxRewritingWhenAuthorizationButPublic) {
  // We should do ajax rewriting when sending over an authorization
  // header if the original is cache-control: public.
  ResponseHeaders headers;
  mock_timer()->SetTimeUs(MockTimer::kApr_5_2010_ms * Timer::kMsUs);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.Add(HttpAttributes::kCacheControl, "public, max-age=400");
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kCssContent);

  // The first response served by the fetcher and is not rewritten. An ajax
  // rewrite is triggered.
  GoogleString text;
  ResponseHeaders response_headers;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kAuthorization, "Paperwork");
  FetchFromProxy("text.css", request_headers, true, &text, &response_headers);
  EXPECT_EQ(kCssContent, text);

  // The second version should be optimized in this case.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", request_headers, true, &text, &response_headers);
  EXPECT_EQ(kMinimizedCssContent, text);
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

TEST_F(ProxyInterfaceTest, AjaxRewritingBlacklistReject) {
  // Makes sure that we honor reject_blacklisted() when ajax rewriting may
  // have normally happened.
  RejectBlacklisted();

  ResponseHeaders headers;
  mock_timer()->SetTimeUs(MockTimer::kApr_5_2010_ms * Timer::kMsUs);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("blacklistCoffee.css"), headers, kCssContent);
  SetFetchResponse(AbsolutifyUrl("tea.css"), headers, kCssContent);

  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("blacklistCoffee.css", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kImATeapot, response_headers.status_code());
  EXPECT_TRUE(text.empty());

  // Non-blacklisted stuff works OK.
  FetchFromProxy("tea.css", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
  EXPECT_EQ(kCssContent, text);
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
  SetFetchResponse(AbsolutifyUrl(kPageUrl), headers, CssLinkHref("a.css"));

  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  text.clear();
  headers.Clear();
  FetchFromProxy(kPageUrl, true, &text, &headers);
  CheckBackgroundFetch(headers, false);
  CheckNumBackgroundFetches(1);
  CheckHeaders(headers, kContentTypeHtml);
  EXPECT_EQ(CssLinkHref(Encode(kTestDomain, "cf", "0", "a.css", "css")), text);
  headers.ComputeCaching();
  EXPECT_LE(start_time_ms_ + kHtmlCacheTimeSec * Timer::kSecondMs,
            headers.CacheExpirationTimeMs());
  EXPECT_EQ(NULL, headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(NULL, headers.Lookup1(HttpAttributes::kLastModified));

  // Fetch the rewritten resource as well.
  text.clear();
  headers.Clear();
  ClearStats();
  FetchFromProxy(Encode(kTestDomain, "cf", "0", "a.css", "css"), true,
                 &text, &headers);
  CheckHeaders(headers, kContentTypeCss);
  // Note that the fetch for the original resource was triggered as a result of
  // the initial HTML request. Hence, its headers indicate that it is a
  // background request
  // This response has kBackgroundFetchHeader set to 1 since a fetch was
  // triggered for it in the background while rewriting the original html.
  CheckBackgroundFetch(headers, true);
  CheckNumBackgroundFetches(0);
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

TEST_F(ProxyInterfaceTest, DontRewriteDisallowedHtmlRejectMode) {
  // If we're in reject_blacklisted mode, we should just respond with the
  // configured status.
  RejectBlacklisted();
  SetResponseWithDefaultHeaders("blacklistCoffee.html", kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2),
  SetResponseWithDefaultHeaders("tea.html", kContentTypeHtml,
                                "tasty", kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy("blacklistCoffee.html", true, &text, &headers);
  EXPECT_EQ(HttpStatus::kImATeapot, headers.status_code());
  EXPECT_TRUE(text.empty());

  // Fetching non-blacklisted one works fine.
  FetchFromProxy("tea.html", true, &text, &headers);
  EXPECT_EQ(HttpStatus::kOK, headers.status_code());
  EXPECT_STREQ("tasty", text);
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
  CheckBackgroundFetch(headers, false);
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
  ASSERT_FALSE(options()->Enabled(RewriteOptions::kExtendCachePdfs));
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
  custom_options->EnableFilter(RewriteOptions::kExtendCachePdfs);
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
      GetCustomOptions("http://example.com/", &request_headers, NULL));
  ASSERT_TRUE(options.get() == NULL);

  // Now put a query-param in, just turning on PageSpeed.  The core filters
  // should be enabled.
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeed=on",
      &request_headers, NULL));
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  CheckExtendCache(options.get(), true);
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Now explicitly enable a filter, which should disable others.
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeedFilters=extend_cache",
      &request_headers, NULL));
  ASSERT_TRUE(options.get() != NULL);
  CheckExtendCache(options.get(), true);
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Now put a request-header in, turning off pagespeed.  request-headers get
  // priority over query-params.
  request_headers.Add("ModPagespeed", "off");
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeed=on",
      &request_headers, NULL));
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_FALSE(options->enabled());

  // Now explicitly enable a bogus filter, which should will cause the
  // options to be uncomputable.
  GoogleUrl gurl("http://example.com/?ModPagespeedFilters=bogus_filter");
  EXPECT_FALSE(proxy_interface_->GetQueryOptions(&gurl, &request_headers,
                                                 message_handler()).second);

  // The default url_namer does not yield any name-derived options, and we
  // have not specified any URL params or request-headers, and kXRequestedWith
  // header is set with bogus value, so there will be no custom options, and no
  // errors.
  request_headers.Add(
      HttpAttributes::kXRequestedWith, "bogus");
  options.reset(
      GetCustomOptions("http://example.com/", &request_headers, NULL));
  ASSERT_TRUE(options.get() == NULL);

  // The default url_namer does not yield any name-derived options, and we
  // have not specified any URL params or request-headers, but kXRequestedWith
  // header is set to 'XmlHttpRequest', so there will be custom options with
  // all js inserted filters disabled.
  request_headers.RemoveAll(HttpAttributes::kXRequestedWith);
  request_headers.Add(
      HttpAttributes::kXRequestedWith, ProxyInterface::kXmlHttpRequest);
  options.reset(
      GetCustomOptions("http://example.com/", &request_headers, NULL));
  // Disable DelayImages for XmlHttpRequests.
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  EXPECT_FALSE(options->Enabled(RewriteOptions::kDelayImages));
  // As kDelayImages filter is present in the disabled list, so it will not get
  // enabled even if it is enabled via EnableFilter().
  options->EnableFilter(RewriteOptions::kDelayImages);
  EXPECT_FALSE(options->Enabled(RewriteOptions::kDelayImages));
}

TEST_F(ProxyInterfaceTest, CustomOptionsWithUrlNamerOptions) {
  // Inject a url-namer that will establish a domain configuration.
  RewriteOptions namer_options;
  namer_options.EnableFilter(RewriteOptions::kCombineJavascript);
  namer_options.EnableFilter(RewriteOptions::kDelayImages);

  RequestHeaders request_headers;
  scoped_ptr<RewriteOptions> options(
      GetCustomOptions("http://example.com/", &request_headers,
                       &namer_options));
  // Even with no query-params or request-headers, we get the custom
  // options as domain options provided as argument.
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  CheckExtendCache(options.get(), false);
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineJavascript));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kDelayImages));

  // Now combine with query params, which turns core-filters on.
  options.reset(GetCustomOptions(
      "http://example.com/?ModPagespeed=on",
      &request_headers, &namer_options));
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
      &request_headers, &namer_options));
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  CheckExtendCache(options.get(), false);
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineJavascript));

  // Now explicitly enable a bogus filter, which should will cause the
  // options to be uncomputable.
  GoogleUrl gurl("http://example.com/?ModPagespeedFilters=bogus_filter");
  EXPECT_FALSE(proxy_interface_->GetQueryOptions(&gurl, &request_headers,
                                                 message_handler()).second);

  request_headers.Add(
      HttpAttributes::kXRequestedWith, "bogus");
  options.reset(
      GetCustomOptions("http://example.com/", &request_headers,
                       &namer_options));
  // Don't disable DelayImages for Non-XmlHttpRequests.
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  CheckExtendCache(options.get(), false);
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineJavascript));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kDelayImages));

  request_headers.RemoveAll(HttpAttributes::kXRequestedWith);
  request_headers.Add(
      HttpAttributes::kXRequestedWith, ProxyInterface::kXmlHttpRequest);
  options.reset(
      GetCustomOptions("http://example.com/", &request_headers,
                       &namer_options));
  // Disable DelayImages for XmlHttpRequests.
  ASSERT_TRUE(options.get() != NULL);
  EXPECT_TRUE(options->enabled());
  CheckExtendCache(options.get(), false);
  EXPECT_FALSE(options->Enabled(RewriteOptions::kCombineCss));
  EXPECT_TRUE(options->Enabled(RewriteOptions::kCombineJavascript));
  EXPECT_FALSE(options->Enabled(RewriteOptions::kDelayImages));
}

TEST_F(ProxyInterfaceTest, MinResourceTimeZero) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  options->set_min_resource_cache_time_to_rewrite_ms(
      kHtmlCacheTimeSec * Timer::kSecondMs);
  resource_manager()->ComputeSignature(options);

  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy(kPageUrl, true, &text, &headers);
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

  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy(kPageUrl, true, &text, &headers);
  EXPECT_EQ(CssLinkHref("a.css"), text);
}

TEST_F(ProxyInterfaceTest, CacheRequests) {
  ResponseHeaders html_headers;
  DefaultResponseHeaders(kContentTypeHtml, kHtmlCacheTimeSec, &html_headers);
  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "2");
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");

  // Original response is still cached in both cases, so we do not
  // fetch the new values.
  text.clear();
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
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
                                     300 * Timer::kSecondMs, ", private");
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
                                     300 * Timer::kSecondMs, ", private");
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
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // input uncacheable memo
  EXPECT_EQ(1, lru_cache()->num_inserts());  // mapping
  EXPECT_EQ(1, lru_cache()->num_identical_reinserts());  // uncacheable memo
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
  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "2");
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");

  // HTML was not cached because of Vary: User-Agent header.
  // So we do fetch the new value.
  text.clear();
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
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
  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "2");
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");

  // Original response was not cached in either case, so we do fetch the
  // new value.
  text.clear();
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
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

  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml, content, 0);
  GoogleString text_out;
  ResponseHeaders headers_out;
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
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

TEST_F(ProxyInterfaceTest, CrossDomainAuthorization) {
  // If we're serving content from evil.com via kProxyHostUrl, we need to make
  // sure we don't propagate through any (non-proxy) authorization headers, as
  // they may have been cached from good.com (as both would look like
  // kProxyHost to the browser).
  ReflectingTestFetcher reflect;
  resource_manager()->set_default_system_fetcher(&reflect);

  ProxyUrlNamer url_namer;
  resource_manager()->set_url_namer(&url_namer);

  RequestHeaders request_headers;
  request_headers.Add("Was", "Here");
  request_headers.Add(HttpAttributes::kAuthorization, "Secret");
  request_headers.Add(HttpAttributes::kProxyAuthorization, "OurSecret");

  ResponseHeaders out_headers;
  GoogleString out_text;
  // Using .txt here so we don't try any AJAX rewriting.
  FetchFromProxy(StrCat("http://", ProxyUrlNamer::kProxyHost,
                        "/test.com/test.com/file.txt"),
                 request_headers,  true, &out_text, &out_headers);
  EXPECT_STREQ("Here", out_headers.Lookup1("Was"));
  EXPECT_FALSE(out_headers.Has(HttpAttributes::kAuthorization));
  EXPECT_FALSE(out_headers.Has(HttpAttributes::kProxyAuthorization));
  mock_scheduler()->AwaitQuiescence();
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
                                 400 * Timer::kSecondMs, ", private");
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
                                 400 * Timer::kSecondMs, ", private");
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
                                 400 * Timer::kSecondMs, ", private");
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
  CheckBackgroundFetch(out_headers, false);
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

TEST_F(ProxyInterfaceTest, PropCacheFilter) {
  CreateFilterCallback create_filter_callback;
  factory()->AddCreateFilterCallback(&create_filter_callback);

  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                "<div><p></p></div>", 0);
  GoogleString text_out;
  ResponseHeaders headers_out;

  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ("<!-- --><div><p></p></div>", text_out);

  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ("<!-- 2 elements unstable --><div><p></p></div>", text_out);

  // How many refreshes should we require before it's stable?  That
  // tuning can be done in the PropertyCacheTest.  For this
  // system-test just do a hundred blind refreshes and check again for
  // stability.
  const int kFetchIterations = 100;
  for (int i = 0; i < kFetchIterations; ++i) {
    FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  }

  // Must be stable by now!
  EXPECT_EQ("<!-- 2 elements stable --><div><p></p></div>", text_out);

  // In this algorithm we will spend a property-cache-write per fetch.
  //
  // We'll also check that we do no cache writes when there are no properties
  // to save.
  EXPECT_EQ(2 + kFetchIterations, lru_cache()->num_inserts());

  // Now change the HTML and watch the #elements change.
  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                "<div><span><p></p></span></div>", 0);
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ("<!-- 3 elements stable --><div><span><p></p></span></div>",
            text_out);

  ClearStats();

  // Finally, disable the property-cache and note that the element-count
  // annotatation reverts to "unknown mode"
  resource_manager_->set_enable_property_cache(false);
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ("<!-- --><div><span><p></p></span></div>", text_out);
}

TEST_F(ProxyInterfaceTest, DomCohortWritten) {
  // Other than the write of DomCohort, there will be no properties added to
  // the cache in this test because we have not enabled the filter with
  //     CreateFilterCallback create_filter_callback;
  //     factory()->AddCreateFilterCallback(&callback);

  DisableAjax();
  GoogleString text_out;
  ResponseHeaders headers_out;

  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_misses());  // property-cache + http-cache

  ClearStats();
  resource_manager_->set_enable_property_cache(false);
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_misses());  // http-cache only.
}

TEST_F(ProxyInterfaceTest, PropCacheNoWritesIfHtmlEndsWithTxt) {
  CreateFilterCallback create_filter_callback;
  factory()->AddCreateFilterCallback(&create_filter_callback);

  // There will be no properties added to the cache set in this test because
  // we have not enabled the filter with
  //     CreateFilterCallback create_filter_callback;
  //     factory()->AddCreateFilterCallback(&callback);

  DisableAjax();
  SetResponseWithDefaultHeaders("page.txt", kContentTypeHtml,
                                "<div><p></p></div>", 0);
  GoogleString text_out;
  ResponseHeaders headers_out;

  FetchFromProxy("page.txt", true, &text_out, &headers_out);
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_misses());  // http-cache only

  ClearStats();
  resource_manager_->set_enable_property_cache(false);
  FetchFromProxy("page.txt", true, &text_out, &headers_out);
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_misses());  // http-cache only
}

TEST_F(ProxyInterfaceTest, PropCacheNoWritesIfNonHtmlDelayedCache) {
  DisableAjax();
  TestPropertyCache(kImageFilenameLackingExt, true, false, true);
}

TEST_F(ProxyInterfaceTest, PropCacheNoWritesIfNonHtmlImmediateCache) {
  // Tests rewriting a file that turns out to be a jpeg, but lacks an
  // extension, where the property-cache lookup is delivered immediately.
  DisableAjax();
  TestPropertyCache(kImageFilenameLackingExt, false, false, true);
}

TEST_F(ProxyInterfaceTest, PropCacheNoWritesIfNonHtmlThreadedCache) {
  // Tests rewriting a file that turns out to be a jpeg, but lacks an
  // extension, where the property-cache lookup is delivered in a
  // separate thread.
  DisableAjax();
  ThreadSynchronizer* sync = resource_manager()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kCollectorPrefix);
  TestPropertyCache(kImageFilenameLackingExt, true, true, true);
}

TEST_F(ProxyInterfaceTest, ThreadedHtml) {
  // Tests rewriting HTML resource where property-cache lookup is delivered
  // in a separate thread.
  DisableAjax();
  ThreadSynchronizer* sync = resource_manager()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kCollectorPrefix);
  TestPropertyCache(kPageUrl, true, true, true);
}

TEST_F(ProxyInterfaceTest, ThreadedHtmlFetcherFailure) {
  // Tests rewriting HTML resource where property-cache lookup is delivered
  // in a separate thread, but the HTML lookup fails after emitting the
  // body.
  DisableAjax();
  mock_url_fetcher()->SetResponseFailure(AbsolutifyUrl(kPageUrl));
  TestPropertyCache(kPageUrl, true, true, false);
}

TEST_F(ProxyInterfaceTest, HtmlFetcherFailure) {
  // Tests rewriting HTML resource where property-cache lookup is
  // delivered in a blocking fashion, and the HTML lookup fails after
  // emitting the body.
  DisableAjax();
  mock_url_fetcher()->SetResponseFailure(AbsolutifyUrl(kPageUrl));
  TestPropertyCache(kPageUrl, false, false, false);
}

TEST_F(ProxyInterfaceTest, HeadersSetupRace) {
  //
  // This crash occured where an Idle-callback is used to flush HTML.
  // In this bug, we were connecting the property-cache callback to
  // the ProxyFetch and then mutating response-headers.  The property-cache
  // callback was waking up the QueuedWorkerPool::Sequence used by
  // the ProxyFetch, which was waking up and calling HeadersComplete.
  // If the implementation of HeadersComplete mutated headers itself,
  // we'd have a deadly race.
  //
  // This test uses the ThreadSynchronizer class to induce the desired
  // race, with strategically placed calls to Signal and Wait.
  //
  // Note that the fix for the race means that one of the Signals does
  // not occur at all, so we have to declare it as "Sloppy" so the
  // ThreadSynchronizer class doesn't vomit on destruction.
  const int kIdleCallbackTimeoutMs = 10;
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->set_idle_flush_time_ms(kIdleCallbackTimeoutMs);
  options->set_flush_html(true);
  resource_manager()->ComputeSignature(options);
  DisableAjax();
  ThreadSynchronizer* sync = resource_manager()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kHeadersSetupRacePrefix);
  ThreadSystem* thread_system = resource_manager()->thread_system();
  QueuedWorkerPool pool(1, thread_system);
  QueuedWorkerPool::Sequence* sequence = pool.NewSequence();
  WorkerTestBase::SyncPoint sync_point(thread_system);
  sequence->Add(MakeFunction(static_cast<ProxyInterfaceTest*>(this),
                             &ProxyInterfaceTest::TestHeadersSetupRace));
  sequence->Add(new WorkerTestBase::NotifyRunFunction(&sync_point));
  sync->TimedWait(ProxyFetch::kHeadersSetupRaceAlarmQueued,
                  ProxyFetch::kTestSignalTimeoutMs);
  {
    // Trigger the idle-callback, if it has been queued.
    ScopedMutex lock(mock_scheduler()->mutex());
    mock_scheduler()->ProcessAlarms(kIdleCallbackTimeoutMs * Timer::kMsUs);
  }
  sync->Wait(ProxyFetch::kHeadersSetupRaceDone);
  sync_point.Wait();
  pool.ShutDown();
  sync->AllowSloppyTermination(ProxyFetch::kHeadersSetupRaceAlarmQueued);
}

TEST_F(ProxyInterfaceTest, BothClientAndPropertyCache) {
  // Ensure that the ProxyFetchPropertyCallbackCollector calls its Post function
  // only once, despite the fact that we are doing two property-cache lookups.
  //
  // Note that ProxyFetchPropertyCallbackCollector::Done waits for
  // ProxyFetch::kCollectorDone.  We will signal it ahead of time so
  // if this is working properly, it won't block.  However, if the system
  // incorrectly calls Done() twice, then it will block forever on the
  // second call to Wait(ProxyFetch::kCollectorDone), since we only offer
  // one Signal here.
  ThreadSynchronizer* sync = resource_manager()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kCollectorPrefix);
  sync->Signal(ProxyFetch::kCollectorDone);

  RequestHeaders request_headers;
  ResponseHeaders response_headers;
  request_headers.Add(HttpAttributes::kXGooglePagespeedClientId, "1");

  DisableAjax();
  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                "<div><p></p></div>", 0);
  GoogleString response;
  FetchFromProxy(kPageUrl, request_headers, true, &response, &response_headers);
  sync->Wait(ProxyFetch::kCollectorReady);  // Clears Signal from PFPCC::Done.
}

// TODO(jmarantz): add a test with a simulated slow cache to see what happens
// when the rest of the system must block, buffering up incoming HTML text,
// waiting for the property-cache lookups to complete.

// Test that we set the Furious cookie up appropriately.
TEST_F(ProxyInterfaceTest, FuriousTest) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->set_ga_id("123-455-2341");
  options->set_running_furious_experiment(true);
  NullMessageHandler handler;
  options->AddFuriousSpec("id=2;enable=extend_cache;percent=100", &handler);
  resource_manager()->ComputeSignature(options);

  SetResponseWithDefaultHeaders("example.jpg", kContentTypeJpeg,
                                "image data", 300);

  ResponseHeaders headers;
  const char kContent[] = "<html><head></head><body>A very compelling "
      "article with an image: <img src=example.jpg></body></html>";
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);
  headers.Clear();

  GoogleString text;
  FetchFromProxy("text.html", true, &text, &headers);
  // Assign all visitors to a furious_spec.
  EXPECT_TRUE(headers.Has(HttpAttributes::kSetCookie));
  ConstStringStarVector values;
  headers.Lookup(HttpAttributes::kSetCookie, &values);
  bool found = false;
  for (int i = 0, n = values.size(); i < n; ++i) {
    if (values[i]->find(furious::kFuriousCookie) == 0) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
  // Image cache-extended and including furious_spec 'a'.
  EXPECT_TRUE(text.find("example.jpg.pagespeed.a.ce") != GoogleString::npos);

  headers.Clear();
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  SetFetchResponse(AbsolutifyUrl("text2.html"), headers, kContent);
  headers.Clear();
  text.clear();

  RequestHeaders req_headers;
  req_headers.Add(HttpAttributes::kCookie, "_GFURIOUS=2");

  FetchFromProxy("text2.html", req_headers, true, &text, &headers);
  // Visitor already has cookie with id=2; don't give them a new one.
  EXPECT_FALSE(headers.Has(HttpAttributes::kSetCookie));
  // Image cache-extended and including furious_spec 'a'.
  EXPECT_TRUE(text.find("example.jpg.pagespeed.a.ce") != GoogleString::npos);

  // Check that we don't include a furious_spec index in urls for the "no
  // experiment" group (id=0).
  headers.Clear();
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  SetFetchResponse(AbsolutifyUrl("text3.html"), headers, kContent);
  headers.Clear();
  text.clear();

  RequestHeaders req_headers2;
  req_headers2.Add(HttpAttributes::kCookie, "_GFURIOUS=0");

  FetchFromProxy("text3.html", req_headers2, true, &text, &headers);
  EXPECT_FALSE(headers.Has(HttpAttributes::kSetCookie));
  EXPECT_TRUE(text.find("example.jpg.pagespeed.ce") != GoogleString::npos);
}

TEST_F(ProxyInterfaceTest, UrlAttributeTest) {
  RewriteOptions* options = resource_manager()->global_options();
  options->ClearSignatureForTesting();
  options->EnableFilter(RewriteOptions::kRewriteDomains);
  options->set_domain_rewrite_hyperlinks(true);
  NullMessageHandler handler;
  options->domain_lawyer()->AddRewriteDomainMapping(
      "http://dst.example.com", "http://src.example.com", &handler);
  options->AddUrlValuedAttribute(
      "span", "src", semantic_type::kHyperlink);
  options->AddUrlValuedAttribute("hr", "imgsrc", semantic_type::kImage);
  resource_manager()->ComputeSignature(options);

  SetResponseWithDefaultHeaders(
      "http://src.example.com/null", kContentTypeHtml, "", 0);
  ResponseHeaders headers;
  const char kContent[] = "<html><head></head><body>"
      "<img src=\"http://src.example.com/null\">"
      "<hr imgsrc=\"http://src.example.com/null\">"
      "<span src=\"http://src.example.com/null\"></span>"
      "<other src=\"http://src.example.com/null\"></other></body></html>";
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);
  headers.Clear();
  GoogleString text;
  FetchFromProxy("text.html", true, &text, &headers);

  // img.src, hr.imgsrc, and span.src are all rewritten
  EXPECT_TRUE(text.find("<img src=\"http://dst.example.com/null\"") !=
              GoogleString::npos);
  EXPECT_TRUE(text.find("<hr imgsrc=\"http://dst.example.com/null\"") !=
              GoogleString::npos);
  EXPECT_TRUE(text.find("<span src=\"http://dst.example.com/null\"") !=
              GoogleString::npos);
  // other.src not rewritten
  EXPECT_TRUE(text.find("<other src=\"http://src.example.com/null\"") !=
              GoogleString::npos);
}

// Test that ClientState is properly read from the client property cache.
TEST_F(ProxyInterfaceTest, ClientStateTest) {
  CreateFilterCallback create_filter_callback;
  factory()->AddCreateFilterCallback(&create_filter_callback);

  SetResponseWithDefaultHeaders("page.html", kContentTypeHtml,
                                "<div><p></p></div>", 0);
  GoogleString text_out;
  ResponseHeaders headers_out;

  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kXGooglePagespeedClientId, "clientid");

  // First pass: Should add fake URL to cache.
  FetchFromProxy("page.html",
                 request_headers,
                 true,
                 &text_out,
                 &headers_out);
  EXPECT_EQ(StrCat("<!-- ClientID: clientid ClientStateID: ",
                   "clientid InCache: true --><div><p></p></div>"),
            text_out);

  // Second pass: Should clear fake URL from cache.
  FetchFromProxy("page.html",
                 request_headers,
                 true,
                 &text_out,
                 &headers_out);
  EXPECT_EQ(StrCat("<!-- ClientID: clientid ClientStateID: clientid ",
                   "InCache: false 2 elements unstable --><div><p></p></div>"),
            text_out);
}

TEST_F(ProxyInterfaceTest, TestOptionsUsedInCacheKey) {
  TestOptionsUsedInCacheKey();
}

}  // namespace net_instaweb
