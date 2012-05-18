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

#include "base/scoped_ptr.h"
#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/mock_callback.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_manager_test_base.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/thread_system.h"

namespace net_instaweb {

// A stripped-down mock of ProxyFetch, used for testing PropertyCacheComplete().
class MockProxyFetch : public ProxyFetch {
 public:
  MockProxyFetch(AsyncFetch* async_fetch,
                 ProxyFetchFactory* factory,
                 ResourceManager* resource_manager)
      : ProxyFetch("http://www.google.com", false,
                   NULL,  // callback
                   async_fetch,
                   NULL,  // no original content fetch
                   resource_manager->NewRewriteDriver(),
                   resource_manager,
                   NULL,  // timer
                   factory),
        complete_(false) {
    response_headers()->set_status_code(HttpStatus::kOK);
  }

  ~MockProxyFetch() { }

  void PropertyCacheComplete(
      ProxyFetchPropertyCallbackCollector *callback_collector,
      bool success) {
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


class ProxyFetchPropertyCallbackCollectorTest : public ResourceManagerTestBase {
 protected:
  ProxyFetchPropertyCallbackCollectorTest() :
    thread_system_(ThreadSystem::CreateThreadSystem()),
    resource_manager_(resource_manager()) { }

  scoped_ptr<ThreadSystem> thread_system_;
  ResourceManager* resource_manager_;

  // Create a collector.
  ProxyFetchPropertyCallbackCollector* MakeCollector() {
    ProxyFetchPropertyCallbackCollector* collector =
        new ProxyFetchPropertyCallbackCollector(resource_manager_);
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
    ProxyFetchPropertyCallback* callback =
        new ProxyFetchPropertyCallback(
            cache_type, ResourceManagerTestBase::kTestDomain, collector,
            mutex);
    EXPECT_EQ(cache_type, callback->cache_type());
    collector->AddCallback(callback);
    return callback;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ProxyFetchPropertyCallbackCollectorTest);
};

TEST_F(ProxyFetchPropertyCallbackCollectorTest, EmptyCollectorTest) {
  // Test that creating an empty collector works.
  scoped_ptr<ProxyFetchPropertyCallbackCollector> collector;
  collector.reset(MakeCollector());
  // This should not fail
  collector->Detach();
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, DoneBeforeDetach) {
  // Test that calling Done() before Detach() works.
  ProxyFetchPropertyCallbackCollector* collector = MakeCollector();
  ProxyFetchPropertyCallback* callback = AddCallback(
      collector, ProxyFetchPropertyCallback::kPagePropertyCache);

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
  collector->Detach();
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, DetachBeforeDone) {
  // Test that calling Detach() before Done() works.
  ProxyFetchPropertyCallbackCollector* collector = MakeCollector();
  ProxyFetchPropertyCallback* callback = AddCallback(
      collector, ProxyFetchPropertyCallback::kPagePropertyCache);

  // Will not delete the collector since we did not call Done yet.
  collector->Detach();

  // This will delete the collector.
  callback->Done(true);
}

TEST_F(ProxyFetchPropertyCallbackCollectorTest, DoneBeforeSetProxyFetch) {
  // Test that calling Done() before SetProxyFetch() works.
  scoped_ptr<ProxyFetchPropertyCallbackCollector> collector;
  collector.reset(MakeCollector());
  ProxyFetchPropertyCallback* callback = AddCallback(
      collector.get(), ProxyFetchPropertyCallback::kPagePropertyCache);

  // Invoke the callback.
  callback->Done(true);

  // Construct mock ProxyFetch to test SetProxyFetch().
  ExpectStringAsyncFetch async_fetch(true);
  ProxyFetchFactory factory(resource_manager_);
  MockProxyFetch* mock_proxy_fetch = new MockProxyFetch(
      &async_fetch, &factory, resource_manager_);

  // Should not be complete since SetProxyFetch() called first.
  EXPECT_EQ(false, mock_proxy_fetch->complete());

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
  EXPECT_EQ(true, mock_proxy_fetch->complete());

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
  ExpectStringAsyncFetch async_fetch(true);
  ProxyFetchFactory factory(resource_manager_);
  MockProxyFetch* mock_proxy_fetch = new MockProxyFetch(
      &async_fetch, &factory, resource_manager_);

  collector.get()->ConnectProxyFetch(mock_proxy_fetch);
  // Should not be complete since SetProxyFetch() called first.
  EXPECT_EQ(false, mock_proxy_fetch->complete());

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
  EXPECT_EQ(true, mock_proxy_fetch->complete());

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
  ExpectStringAsyncFetch async_fetch(true);
  ProxyFetchFactory factory(resource_manager_);
  MockProxyFetch* mock_proxy_fetch = new MockProxyFetch(
      &async_fetch, &factory, resource_manager_);

  collector.get()->ConnectProxyFetch(mock_proxy_fetch);
  // Should not be complete since SetProxyFetch() called first.
  EXPECT_EQ(false, mock_proxy_fetch->complete());

  // Now invoke the page callback.
  page_callback->Done(true);

  // Should not be complete since both callbacks not yet done.
  EXPECT_EQ(false, mock_proxy_fetch->complete());

  // Collector should now have a page property.
  scoped_ptr<PropertyPage> page;
  page.reset(collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kPagePropertyCache));
  EXPECT_TRUE(NULL != page.get());

  // ... but not a client property.
  EXPECT_EQ(NULL, collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kClientPropertyCache));

  // Now invoke the client callback.
  client_callback->Done(true);

  // Should be complete since both callbacks are done.
  EXPECT_EQ(true, mock_proxy_fetch->complete());

  // Collector should now have a client property.
  page.reset(collector.get()->GetPropertyPage(
      ProxyFetchPropertyCallback::kClientPropertyCache));
  EXPECT_TRUE(NULL != page.get());

  // Needed for cleanup.
  mock_proxy_fetch->Done(true);
}

}  // namespace net_instaweb
