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

// Author: mmohabey@google.com (Megha Mohabey)

#include "net/instaweb/automatic/public/proxy_interface_test_base.h"

#include <cstddef>

#include "net/instaweb/automatic/public/proxy_fetch.h"
#include "net/instaweb/automatic/public/proxy_interface.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/mock_callback.h"
#include "net/instaweb/http/public/mock_url_fetcher.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/user_agent_matcher.h"
#include "net/instaweb/rewriter/public/critical_images_finder.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/test_rewrite_driver_factory.h"
#include "net/instaweb/util/public/abstract_client_state.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/delay_cache.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/mock_scheduler.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/queued_worker_pool.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/thread_synchronizer.h"
#include "net/instaweb/util/worker_test_base.h"

namespace net_instaweb {

class Statistics;

namespace {

// Like ExpectStringAsyncFetch but for asynchronous invocation -- it lets
// one specify a WorkerTestBase::SyncPoint to help block until completion.
class AsyncExpectStringAsyncFetch : public ExpectStringAsyncFetch {
 public:
  AsyncExpectStringAsyncFetch(bool expect_success,
                              bool log_flush,
                              GoogleString* buffer,
                              ResponseHeaders* response_headers,
                              bool* done_value,
                              WorkerTestBase::SyncPoint* notify,
                              ThreadSynchronizer* sync,
                              const RequestContextPtr& request_context)
      : ExpectStringAsyncFetch(expect_success, request_context),
        buffer_(buffer),
        done_value_(done_value),
        notify_(notify),
        sync_(sync),
        log_flush_(log_flush) {
    buffer->clear();
    response_headers->Clear();
    *done_value = false;
    set_response_headers(response_headers);
  }

  virtual ~AsyncExpectStringAsyncFetch() {}

  virtual void HandleHeadersComplete() {
    // Make sure we have cleaned the headers in ProxyInterface.
    EXPECT_FALSE(
        request_headers()->Has(HttpAttributes::kAcceptEncoding));

    sync_->Wait(ProxyFetch::kHeadersSetupRaceWait);
    response_headers()->Add("HeadersComplete", "1");  // Dirties caching info.
    sync_->Signal(ProxyFetch::kHeadersSetupRaceFlush);
  }

  virtual void HandleDone(bool success) {
    *buffer_ = buffer();
    *done_value_ = success;
    ExpectStringAsyncFetch::HandleDone(success);
    WorkerTestBase::SyncPoint* notify = notify_;
    delete this;
    notify->Notify();
  }

  virtual bool HandleFlush(MessageHandler* handler) {
    if (log_flush_) {
      HandleWrite("|Flush|", handler);
    }
    return true;
  }

 private:
  GoogleString* buffer_;
  bool* done_value_;
  WorkerTestBase::SyncPoint* notify_;
  ThreadSynchronizer* sync_;
  bool log_flush_;

  DISALLOW_COPY_AND_ASSIGN(AsyncExpectStringAsyncFetch);
};

class FakeCriticalImagesFinder : public CriticalImagesFinder {
 public:
  explicit FakeCriticalImagesFinder(Statistics* stats)
      : CriticalImagesFinder(stats) {}
  ~FakeCriticalImagesFinder() {}

  virtual bool IsMeaningful(const RewriteDriver* driver) const { return true; }

  virtual void UpdateCriticalImagesSetInDriver(RewriteDriver* driver) {
    if (critical_images_ != NULL) {
      StringSet* critical_images = new StringSet;
      *critical_images = *critical_images_;
      driver->set_critical_images(critical_images);
    }
    if (css_critical_images_ != NULL) {
      StringSet* css_critical_images = new StringSet;
      *css_critical_images = *css_critical_images_;
      driver->set_css_critical_images(css_critical_images);
    }
  }

  virtual void ComputeCriticalImages(StringPiece url,
                                     RewriteDriver* driver) {
    // Do Nothing
  }

  virtual const char* GetCriticalImagesCohort() const {
    return "critical_images";
  }

  void set_critical_images(StringSet* critical_images) {
    critical_images_.reset(critical_images);
  }

  void set_css_critical_images(StringSet* css_critical_images) {
    css_critical_images_.reset(css_critical_images);
  }

 private:
  scoped_ptr<StringSet> critical_images_;
  scoped_ptr<StringSet> css_critical_images_;
  DISALLOW_COPY_AND_ASSIGN(FakeCriticalImagesFinder);
};

}  // namespace

// ProxyUrlNamer.
const char ProxyUrlNamer::kProxyHost[] = "proxy_host.com";
bool ProxyUrlNamer::Decode(const GoogleUrl& gurl,
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

// MockFilter.
void MockFilter::StartDocument() {
  num_elements_ = 0;
  PropertyCache* page_cache =
      driver_->server_context()->page_property_cache();
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

void MockFilter::StartElement(HtmlElement* element) {
  if (num_elements_ == 0) {
    // Before the start of the first element, print out the number
    // of elements that we expect based on the cache.
    GoogleString comment = " ";
    PropertyCache* page_cache =
        driver_->server_context()->page_property_cache();

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

void MockFilter::EndDocument() {
  // We query IsCacheable for the HTML file only to ensure that
  // the test will crash if ComputeCaching() was never called.
  //
  // IsCacheable is true for HTML files because of kHtmlCacheTimeSec
  // above.
  EXPECT_TRUE(driver_->response_headers()->IsCacheable());

  if (num_elements_property_ != NULL) {
    PropertyCache* page_cache =
        driver_->server_context()->page_property_cache();
    page_cache->UpdateValue(IntegerToString(num_elements_),
                            num_elements_property_);
    num_elements_property_ = NULL;
  }
}

// ProxyInterfaceTestBase.
ProxyInterfaceTestBase::ProxyInterfaceTestBase()
    : callback_done_value_(false),
      fake_critical_images_finder_(
          new FakeCriticalImagesFinder(statistics())) {}

void ProxyInterfaceTestBase::TestHeadersSetupRace() {
  mock_url_fetcher()->SetResponseFailure(AbsolutifyUrl(kPageUrl));
  TestPropertyCache(kPageUrl, true, true, false);
}

void ProxyInterfaceTestBase::SetUp() {
  RewriteTestBase::SetUp();
  ProxyInterface::InitStats(statistics());
  proxy_interface_.reset(
      new ProxyInterface("localhost", 80, server_context(), statistics()));
  server_context()->set_critical_images_finder(
      fake_critical_images_finder_);
}

void ProxyInterfaceTestBase::TearDown() {
  // Make sure all the jobs are over before we check for leaks ---
  // someone might still be trying to clean themselves up.
  mock_scheduler()->AwaitQuiescence();
  EXPECT_EQ(0, server_context()->num_active_rewrite_drivers());
  RewriteTestBase::TearDown();
}

void ProxyInterfaceTestBase::SetCriticalImagesInFinder(
    StringSet* critical_images) {
  FakeCriticalImagesFinder* finder = static_cast<FakeCriticalImagesFinder*>(
      fake_critical_images_finder_);
  finder->set_critical_images(critical_images);
}

void ProxyInterfaceTestBase::SetCssCriticalImagesInFinder(
    StringSet* css_critical_images) {
  FakeCriticalImagesFinder* finder = static_cast<FakeCriticalImagesFinder*>(
      fake_critical_images_finder_);
  finder->set_css_critical_images(css_critical_images);
}

// Initiates a fetch using the proxy interface, and waits for it to
// complete.
void ProxyInterfaceTestBase::FetchFromProxy(
    const StringPiece& url,
    const RequestHeaders& request_headers,
    bool expect_success,
    GoogleString* string_out,
    ResponseHeaders* headers_out) {
  FetchFromProxyNoWait(url, request_headers, expect_success,
                       false /* log_flush*/, headers_out);
  WaitForFetch();
  *string_out = callback_buffer_;
}

// TODO(jmarantz): eliminate this interface as it's annoying to have
// the function overload just to save an empty RequestHeaders arg.
void ProxyInterfaceTestBase::FetchFromProxy(const StringPiece& url,
                                            bool expect_success,
                                            GoogleString* string_out,
                                            ResponseHeaders* headers_out) {
  RequestHeaders request_headers;
  FetchFromProxy(url, request_headers, expect_success, string_out,
                 headers_out);
}

void ProxyInterfaceTestBase::FetchFromProxyLoggingFlushes(
    const StringPiece& url, bool expect_success, GoogleString* string_out) {
  RequestHeaders request_headers;
  ResponseHeaders response_headers;
  FetchFromProxyNoWait(url, request_headers, expect_success,
                       true /* log_flush*/, &response_headers);
  WaitForFetch();
  *string_out = callback_buffer_;
}

// Initiates a fetch using the proxy interface, without waiting for it to
// complete.  The usage model here is to delay callbacks and/or fetches
// to control their order of delivery, then call WaitForFetch.
void ProxyInterfaceTestBase::FetchFromProxyNoWait(
    const StringPiece& url,
    const RequestHeaders& request_headers,
    bool expect_success,
    bool log_flush,
    ResponseHeaders* headers_out) {
  sync_.reset(new WorkerTestBase::SyncPoint(
      server_context()->thread_system()));
  AsyncFetch* fetch = new AsyncExpectStringAsyncFetch(
      expect_success, log_flush, &callback_buffer_,
      &callback_response_headers_, &callback_done_value_, sync_.get(),
      server_context()->thread_synchronizer(),
      rewrite_driver()->request_context());
  fetch->set_response_headers(headers_out);
  fetch->request_headers()->CopyFrom(request_headers);
  proxy_interface_->Fetch(AbsolutifyUrl(url), message_handler(), fetch);
}

// This must be called after FetchFromProxyNoWait, once all of the required
// resources (fetches, cache lookups) have been released.
void ProxyInterfaceTestBase::WaitForFetch() {
  sync_->Wait();
  mock_scheduler()->AwaitQuiescence();
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
void ProxyInterfaceTestBase::TestPropertyCache(const StringPiece& url,
                                               bool delay_pcache,
                                               bool thread_pcache,
                                               bool expect_success) {
  RequestHeaders request_headers;
  ResponseHeaders response_headers;
  GoogleString output;
  TestPropertyCacheWithHeadersAndOutput(
      url, delay_pcache, thread_pcache, expect_success, true, true, false,
      request_headers, &response_headers, &output);
}

void ProxyInterfaceTestBase::TestPropertyCacheWithHeadersAndOutput(
    const StringPiece& url, bool delay_pcache, bool thread_pcache,
    bool expect_success, bool check_stats, bool add_create_filter_callback,
    bool expect_detach_before_pcache, const RequestHeaders& request_headers,
    ResponseHeaders* response_headers, GoogleString* output) {
  scoped_ptr<QueuedWorkerPool> pool;
  QueuedWorkerPool::Sequence* sequence = NULL;

  ThreadSynchronizer* sync = server_context()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kCollectorDelete);
  GoogleString delay_pcache_key, delay_http_cache_key;
  if (delay_pcache || thread_pcache) {
    PropertyCache* pcache = page_property_cache();
    const PropertyCache::Cohort* cohort =
        pcache->GetCohort(RewriteDriver::kDomCohort);
    delay_http_cache_key = AbsolutifyUrl(url);
    delay_pcache_key = pcache->CacheKey(StrCat(
        delay_http_cache_key,
        UserAgentMatcher::DeviceTypeSuffix(UserAgentMatcher::kDesktop)),
        cohort);
    delay_cache()->DelayKey(delay_pcache_key);
    if (thread_pcache) {
      delay_cache()->DelayKey(delay_http_cache_key);
      pool.reset(new QueuedWorkerPool(
          1, "pcache", server_context()->thread_system()));
      sequence = pool->NewSequence();
    }
  }

  CreateFilterCallback create_filter_callback;
  if (add_create_filter_callback) {
    factory()->AddCreateFilterCallback(&create_filter_callback);
  }

  if (thread_pcache) {
    FetchFromProxyNoWait(url, request_headers, expect_success,
                         false /* don't log flushes*/, response_headers);
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
    *output = callback_buffer_;
    sync->Wait(ProxyFetch::kCollectorDelete);
    pool->ShutDown();
  } else {
    FetchFromProxyNoWait(url, request_headers, expect_success, false,
                         response_headers);
    if (expect_detach_before_pcache) {
      WaitForFetch();
    }
    if (delay_pcache) {
      delay_cache()->ReleaseKey(delay_pcache_key);
    }
    if (!expect_detach_before_pcache) {
      WaitForFetch();
    }
    *output = callback_buffer_;
    sync->Wait(ProxyFetch::kCollectorDelete);
  }

  if (check_stats) {
    EXPECT_EQ(1, lru_cache()->num_inserts());  // http-cache
    // We expect 4 misses. 1 for http-cache and 3 for prop-cache which
    // correspond to each different device type in
    // UserAgentMatcher::DeviceType.
    EXPECT_EQ(4, lru_cache()->num_misses());
  }
}

}  // namespace net_instaweb
