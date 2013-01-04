/*
 * Copyright 2012 Google Inc.
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

// Author: mdw@google.com (Matt Welsh)

// Unit-tests for ProxyFetch and related classes

#include "net/instaweb/automatic/public/proxy_fetch.h"

#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/mock_callback.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/thread_system.h"

namespace net_instaweb {

// A stripped-down mock of ProxyFetch, used for testing PropertyCacheComplete().
class MockProxyFetch : public ProxyFetch {
 public:
  MockProxyFetch(AsyncFetch* async_fetch,
                 ProxyFetchFactory* factory,
                 ServerContext* server_context)
      : ProxyFetch("http://www.google.com", false,
                   NULL,  // callback
                   async_fetch,
                   NULL,  // no original content fetch
                   server_context->NewRewriteDriver(
                       async_fetch->request_context()),
                   server_context,
                   NULL,  // timer
                   factory),
        complete_(false) {
    response_headers()->set_status_code(HttpStatus::kOK);
  }

  ~MockProxyFetch() { }

  void PropertyCacheComplete(
      bool success,
      ProxyFetchPropertyCallbackCollector* callback_collector) {
    complete_ = true;
  }

  void Done(bool success) {
    HandleDone(success);
  }

  bool complete() const { return complete_; }

 private:
  bool complete_;
  DISALLOW_COPY_AND_ASSIGN(MockProxyFetch);
};


class ProxyFetchPropertyCallbackCollectorTest : public RewriteTestBase {
 public:
  void PostLookupTask() {
    post_lookup_called_ = true;
  }

 protected:
  ProxyFetchPropertyCallbackCollectorTest() :
    thread_system_(ThreadSystem::CreateThreadSystem()),
    server_context_(server_context()),
    post_lookup_called_(false) {}

  scoped_ptr<ThreadSystem> thread_system_;
  ServerContext* server_context_;

  // Create a collector.
  ProxyFetchPropertyCallbackCollector* MakeCollector() {
    ProxyFetchPropertyCallbackCollector* collector =
        new ProxyFetchPropertyCallbackCollector(
            server_context_, RewriteTestBase::kTestDomain,
            RequestContext::NewTestRequestContext(thread_system_.get()),
            options(), NULL);
    // Collector should not contain any PropertyPages
    EXPECT_EQ(NULL, collector->GetPropertyPage(
        ProxyFetchPropertyCallback::kPagePropertyCache));
    EXPECT_EQ(NULL, collector->GetPropertyPage(
        ProxyFetchPropertyCallback::kClientPropertyCache));

    return collector;
  }

  // Add a callback of the given type to the collector.
  ProxyFetchPropertyCallback* AddCallback(
      ProxyFetchPropertyCallbackCollector* collector,
      ProxyFetchPropertyCallback::CacheType cache_type) {
    AbstractMutex* mutex = thread_system_->NewMutex();
    PropertyCache* property_cache =
        (cache_type == ProxyFetchPropertyCallback::kPagePropertyCache) ?
        page_property_cache() : server_context()->client_property_cache();
    ProxyFetchPropertyCallback* callback =
        new ProxyFetchPropertyCallback(
            cache_type, *property_cache, RewriteTestBase::kTestDomain,
            UserAgentMatcher::kDesktop, collector, mutex);
    EXPECT_EQ(cache_type, callback->cache_type());
    collector->AddCallback(callback);
    return callback;
  }

  void AddPostLookupConnectProxyFetchCallDone(
      ProxyFetchPropertyCallbackCollector* collector,
      MockProxyFetch* mock_proxy_fetch,
      ProxyFetchPropertyCallback* callback) {
    collector->AddPostLookupTask(MakeFunction(
        this, &ProxyFetchPropertyCallbackCollectorTest::PostLookupTask));
    collector->ConnectProxyFetch(mock_proxy_fetch);
    callback->Done(true);
  }

  void ConnectProxyFetchAddPostLookupCallDone(
      ProxyFetchPropertyCallbackCollector* collector,
      MockProxyFetch* mock_proxy_fetch,
      ProxyFetchPropertyCallback* callback) {
    collector->ConnectProxyFetch(mock_proxy_fetch);
    collector->AddPostLookupTask(MakeFunction(
        this, &ProxyFetchPropertyCallbackCollectorTest::PostLookupTask));
    callback->Done(true);
  }

  void CallDoneAddPostLookupConnectProxyFetch(
      ProxyFetchPropertyCallbackCollector* collector,
      MockProxyFetch* mock_proxy_fetch,
      ProxyFetchPropertyCallback* callback) {
    callback->Done(true);
    collector->AddPostLookupTask(MakeFunction(
        this, &ProxyFetchPropertyCallbackCollectorTest::PostLookupTask));
    collector->ConnectProxyFetch(mock_proxy_fetch);
  }

  void TestAddPostlookupTask(bool add_before_done,
                             bool add_before_proxy_fetch) {
    GoogleString kUrl("http://www.test.com/");
    scoped_ptr<ProxyFetchPropertyCallbackCollector> collector;
    collector.reset(MakeCollector());
    ProxyFetchPropertyCallback* page_callback = AddCallback(
        collector.get(), ProxyFetchPropertyCallback::kPagePropertyCache);
    ExpectStringAsyncFetch async_fetch(
        true, RequestContext::NewTestRequestContext(thread_system_.get()));
    ProxyFetchFactory factory(server_context_);
    MockProxyFetch* mock_proxy_fetch = new MockProxyFetch(
        &async_fetch, &factory, server_context_);
    if (add_before_done && add_before_proxy_fetch) {
      AddPostLookupConnectProxyFetchCallDone(
          collector.get(), mock_proxy_fetch, page_callback);
    } else if (add_before_done && !add_before_proxy_fetch) {
      ConnectProxyFetchAddPostLookupCallDone(
          collector.get(), mock_proxy_fetch, page_callback);
    } else if (!add_before_done && add_before_proxy_fetch) {
      CallDoneAddPostLookupConnectProxyFetch(
          collector.get(), mock_proxy_fetch, page_callback);
    } else {
      // Not handled. Make this fail.
    }
    EXPECT_TRUE(post_lookup_called_);
    mock_proxy_fetch->Done(true);
  }

 private:
  bool post_lookup_called_;

  DISALLOW_COPY_AND_ASSIGN(ProxyFetchPropertyCallbackCollectorTest);
};

TEST_F(ProxyFetchPropertyCallbackCollectorTest, EmptyCollectorTest) {
  // Test that creating an empty collector works.
  scoped_ptr<ProxyFetchPropertyCallbackCollector> collector;
  collector.reset(MakeCollector());
  // This should not fail
  collector->Detach(HttpStatus::kUnknownStatusCode);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, DoneBeforeDetach) {
  // Test that calling Done() before Detach() works.
  ProxyFetchPropertyCallbackCollector* collector = MakeCollector();
  ProxyFetchPropertyCallback* callback = AddCallback(
      collector, ProxyFetchPropertyCallback::kPagePropertyCache);

  // callback->IsCacheValid could be called anytime before callback->Done.
  // Will return true because there are no cache invalidation URL patterns.
  EXPECT_TRUE(callback->IsCacheValid(1L));

  // Invoke the callback.
  callback->Done(true);

  // Collector should now have a page property.
  scoped_ptr<PropertyPage> page;
  page.reset(collector->GetPropertyPage(
      ProxyFetchPropertyCallback::kPagePropertyCache));
  EXPECT_TRUE(NULL != page.get());

  // ... but not a client property.
  EXPECT_EQ(NULL, collector->GetPropertyPage(
      ProxyFetchPropertyCallback::kClientPropertyCache));

  // This should not fail - will also delete the collector.
  collector->Detach(HttpStatus::kUnknownStatusCode);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, UrlInvalidDoneBeforeDetach) {
  // Invalidate all URLs cached before timestamp 2.
  options_->AddUrlCacheInvalidationEntry("*", 2L, true);
  // Test that calling Done() before Detach() works.
  ProxyFetchPropertyCallbackCollector* collector = MakeCollector();
  ProxyFetchPropertyCallback* callback = AddCallback(
      collector, ProxyFetchPropertyCallback::kPagePropertyCache);

  // callback->IsCacheValid could be called anytime before callback->Done.
  // Will return false due to the invalidation entry.
  EXPECT_FALSE(callback->IsCacheValid(1L));

  // Invoke the callback.
  callback->Done(true);

  // Collector should now have a page property.
  scoped_ptr<PropertyPage> page;
  page.reset(collector->GetPropertyPage(
      ProxyFetchPropertyCallback::kPagePropertyCache));
  EXPECT_TRUE(NULL != page.get());

  // ... but not a client property.
  EXPECT_EQ(NULL, collector->GetPropertyPage(
      ProxyFetchPropertyCallback::kClientPropertyCache));

  // This should not fail - will also delete the collector.
  collector->Detach(HttpStatus::kUnknownStatusCode);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, DetachBeforeDone) {
  // Test that calling Detach() before Done() works.
  ProxyFetchPropertyCallbackCollector* collector = MakeCollector();
  ProxyFetchPropertyCallback* callback = AddCallback(
      collector, ProxyFetchPropertyCallback::kPagePropertyCache);

  // callback->IsCacheValid could be called anytime before callback->Done.
  // Will return true because there are no cache invalidation URL patterns.
  EXPECT_TRUE(callback->IsCacheValid(1L));

  // Will not delete the collector since we did not call Done yet.
  collector->Detach(HttpStatus::kUnknownStatusCode);

  // This call is after Detach (but before callback->Done).
  // ProxyFetchPropertyCallbackCollector::IsCacheValid returns false if
  // detached.
  EXPECT_FALSE(callback->IsCacheValid(1L));

  // This will delete the collector.
  callback->Done(true);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, DoneBeforeSetProxyFetch) {
  // Test that calling Done() before SetProxyFetch() works.
  scoped_ptr<ProxyFetchPropertyCallbackCollector> collector;
  collector.reset(MakeCollector());
  ProxyFetchPropertyCallback* callback = AddCallback(
      collector.get(), ProxyFetchPropertyCallback::kPagePropertyCache);

  // callback->IsCacheValid could be called anytime before callback->Done.
  // Will return true because there are no cache invalidation URL patterns.
  EXPECT_TRUE(callback->IsCacheValid(1L));

  // Invoke the callback.
  callback->Done(true);

  // Construct mock ProxyFetch to test SetProxyFetch().
  ExpectStringAsyncFetch async_fetch(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  ProxyFetchFactory factory(server_context_);
  MockProxyFetch* mock_proxy_fetch = new MockProxyFetch(
      &async_fetch, &factory, server_context_);

  // Should not be complete since SetProxyFetch() called first.
  EXPECT_FALSE(mock_proxy_fetch->complete());

  // Collector should now have a page property.
  scoped_ptr<PropertyPage> page;
  page.reset(collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kPagePropertyCache));
  EXPECT_TRUE(NULL != page.get());

  // ... but not a client property.
  EXPECT_EQ(NULL, collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kClientPropertyCache));

  collector.get()->ConnectProxyFetch(mock_proxy_fetch);
  // Should be complete since SetProxyFetch() called after Done().
  EXPECT_TRUE(mock_proxy_fetch->complete());

  // Needed for cleanup.
  mock_proxy_fetch->Done(true);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, SetProxyFetchBeforeDone) {
  // Test that calling SetProxyFetch() before Done() works.
  scoped_ptr<ProxyFetchPropertyCallbackCollector> collector;
  collector.reset(MakeCollector());
  ProxyFetchPropertyCallback* callback = AddCallback(
      collector.get(), ProxyFetchPropertyCallback::kPagePropertyCache);

  // Construct mock ProxyFetch to test SetProxyFetch().
  ExpectStringAsyncFetch async_fetch(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  ProxyFetchFactory factory(server_context_);
  MockProxyFetch* mock_proxy_fetch = new MockProxyFetch(
      &async_fetch, &factory, server_context_);

  // callback->IsCacheValid could be called anytime before callback->Done.
  // Will return true because there are no cache invalidation URL patterns.
  EXPECT_TRUE(callback->IsCacheValid(1L));

  collector.get()->ConnectProxyFetch(mock_proxy_fetch);
  // Should not be complete since SetProxyFetch() called first.
  EXPECT_FALSE(mock_proxy_fetch->complete());

  EXPECT_TRUE(callback->IsCacheValid(1L));

  // Now invoke the callback.
  callback->Done(true);

  // Collector should now have a page property.
  scoped_ptr<PropertyPage> page;
  page.reset(collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kPagePropertyCache));
  EXPECT_TRUE(NULL != page.get());

  // ... but not a client property.
  EXPECT_EQ(NULL, collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kClientPropertyCache));

  // Should be complete since Done() called.
  EXPECT_TRUE(mock_proxy_fetch->complete());

  // Needed for cleanup.
  mock_proxy_fetch->Done(true);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, BothCallbacksComplete) {
  scoped_ptr<ProxyFetchPropertyCallbackCollector> collector;
  collector.reset(MakeCollector());

  ProxyFetchPropertyCallback* page_callback = AddCallback(
      collector.get(), ProxyFetchPropertyCallback::kPagePropertyCache);

  ProxyFetchPropertyCallback* client_callback = AddCallback(
      collector.get(), ProxyFetchPropertyCallback::kClientPropertyCache);

  // Construct mock ProxyFetch to test SetProxyFetch().
  ExpectStringAsyncFetch async_fetch(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  ProxyFetchFactory factory(server_context_);
  MockProxyFetch* mock_proxy_fetch = new MockProxyFetch(
      &async_fetch, &factory, server_context_);

  collector.get()->ConnectProxyFetch(mock_proxy_fetch);
  // Should not be complete since SetProxyFetch() called first.
  EXPECT_FALSE(mock_proxy_fetch->complete());

  // Now invoke the page callback.
  page_callback->Done(true);

  // Should not be complete since both callbacks not yet done.
  EXPECT_FALSE(mock_proxy_fetch->complete());

  // Collector should not have a page property.
  EXPECT_EQ(NULL, collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kPagePropertyCache));

  // ... no client property as well.
  EXPECT_EQ(NULL, collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kClientPropertyCache));

  // Now invoke the client callback.
  client_callback->Done(true);

  // Should be complete since both callbacks are done.
  EXPECT_TRUE(mock_proxy_fetch->complete());

  // Collector should now have a page property.
  scoped_ptr<PropertyPage> page;
  page.reset(collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kPagePropertyCache));
  EXPECT_TRUE(NULL != page.get());

  // Collector should now have a client property.
  page.reset(collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kClientPropertyCache));
  EXPECT_TRUE(NULL != page.get());

  // Needed for cleanup.
  mock_proxy_fetch->Done(true);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, PostLookupProxyFetchDone) {
  TestAddPostlookupTask(true, true);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, DonePostLookupProxyFetch) {
  TestAddPostlookupTask(false, true);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, ProxyFetchPostLookupDone) {
  TestAddPostlookupTask(true, false);
}

}  // namespace net_instaweb
