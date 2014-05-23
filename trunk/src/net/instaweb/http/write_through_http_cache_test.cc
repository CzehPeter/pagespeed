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

// Author: nikhilmadan@google.com (Nikhil Madan)

// Unit-test the write through http cache.

#include "net/instaweb/http/public/write_through_http_cache.h"

#include <cstddef>
#include "base/logging.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_hasher.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/platform.h"
#include "net/instaweb/util/public/simple_stats.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"
#include "pagespeed/kernel/http/request_headers.h"

namespace {
// Set the cache size large enough so nothing gets evicted during this test.
const int kMaxSize = 10000;
const char kStartDate[] = "Sun, 16 Dec 1979 02:27:45 GMT";
const char kHttpsUrl[] = "https://www.test.com/";
}

namespace net_instaweb {

class MessageHandler;

// Helper class for calling Get and Query methods on cache implementations
// that are blocking in nature (e.g. in-memory LRU or blocking file-system).
class FakeHttpCacheCallback : public HTTPCache::Callback {
 public:
  explicit FakeHttpCacheCallback(ThreadSystem* thread_system)
      : HTTPCache::Callback(
          RequestContext::NewTestRequestContext(thread_system)),
        called_(false),
        result_(HTTPCache::kNotFound),
        first_call_cache_valid_(true),
        first_cache_valid_(true),
        second_cache_valid_(true),
        first_call_cache_fresh_(true),
        first_cache_fresh_(true),
        second_cache_fresh_(true) {}

  virtual void Done(HTTPCache::FindResult result) {
    called_ = true;
    result_ = result;
  }
  virtual bool IsCacheValid(const GoogleString& key,
                            const ResponseHeaders& headers) {
    bool result = first_call_cache_valid_ ?
        first_cache_valid_ : second_cache_valid_;
    first_call_cache_valid_ = false;
    return result;
  }

  virtual bool IsFresh(const ResponseHeaders& headers) {
    bool result = first_call_cache_fresh_ ?
        first_cache_fresh_ : second_cache_fresh_;
    first_call_cache_fresh_ = false;
    return result;
  }

  virtual ResponseHeaders::VaryOption RespectVaryOnResources() const {
    return ResponseHeaders::kRespectVaryOnResources;
  }

  bool called_;
  HTTPCache::FindResult result_;
  bool first_call_cache_valid_;
  bool first_cache_valid_;
  bool second_cache_valid_;
  bool first_call_cache_fresh_;
  bool first_cache_fresh_;
  bool second_cache_fresh_;
};

class WriteThroughHTTPCacheTest : public testing::Test {
 protected:
  static int64 ParseDate(const char* start_date) {
    int64 time_ms;
    CHECK(ResponseHeaders::ParseTime(start_date, &time_ms));
    return time_ms;
  }

  WriteThroughHTTPCacheTest()
      : thread_system_(Platform::CreateThreadSystem()),
        mock_timer_(thread_system_->NewMutex(), ParseDate(kStartDate)),
        cache1_(kMaxSize), cache2_(kMaxSize),
        key_("http://www.test.com/1"),
        key2_("http://www.test.com/2"),
        fragment_("www.test.com"),
        content_("content"), header_name_("name"),
        header_value_("value"),
        cache1_ms_(-1),
        cache2_ms_(-1) {
    HTTPCache::InitStats(&simple_stats_);
    http_cache_.reset(new WriteThroughHTTPCache(
        &cache1_, &cache2_, &mock_timer_, &mock_hasher_, &simple_stats_));
  }

  void InitHeaders(ResponseHeaders* headers, const char* cache_control) {
    headers->Add(header_name_, header_value_);
    headers->Add("Date", kStartDate);
    if (cache_control != NULL) {
      headers->Add("Cache-control", cache_control);
    }
    headers->SetStatusAndReason(HttpStatus::kOK);
    headers->ComputeCaching();
  }

  int GetStat(const char* name) { return simple_stats_.LookupValue(name); }

  HTTPCache::FindResult Find(const GoogleString& key,
                             const GoogleString& fragment,
                             HTTPValue* value,
                             ResponseHeaders* headers,
                             MessageHandler* handler) {
    FakeHttpCacheCallback callback(thread_system_.get());
    http_cache_->Find(key, fragment, handler, &callback);
    EXPECT_TRUE(callback.called_);
    if (callback.result_ == HTTPCache::kFound) {
      value->Link(callback.http_value());
    }
    headers->CopyFrom(*callback.response_headers());
    callback.request_context()->timing_info().GetHTTPCacheLatencyMs(
        &cache1_ms_);
    callback.request_context()->timing_info().GetL2HTTPCacheLatencyMs(
        &cache2_ms_);

    return callback.result_;
  }

  void CheckCachedValueValid() {
    HTTPValue value;
    ResponseHeaders headers;
    HTTPCache::FindResult found = Find(
        key_, fragment_, &value, &headers, &message_handler_);
    EXPECT_EQ(HTTPCache::kFound, found);
    EXPECT_TRUE(headers.headers_complete());
    StringPiece contents;
    EXPECT_TRUE(value.ExtractContents(&contents));
    EXPECT_EQ(content_, contents);
    EXPECT_EQ(header_value_, headers.Lookup1(header_name_));
  }

  void CheckCachedValueExpired() {
    HTTPValue value;
    ResponseHeaders headers;
    HTTPCache::FindResult found = Find(key_, fragment_, &value, &headers,
                                       &message_handler_);
    EXPECT_EQ(HTTPCache::kNotFound, found);
    EXPECT_FALSE(headers.headers_complete());
  }

  void ClearStats() {
    cache1_.ClearStats();
    cache2_.ClearStats();
    simple_stats_.Clear();
  }

  void Put(const GoogleString& key, const GoogleString& fragment,
           ResponseHeaders* headers, const StringPiece& content,
           MessageHandler* handler) {
    http_cache_->Put(key, fragment, RequestHeaders::Properties(),
                     ResponseHeaders::kRespectVaryOnResources,
                     headers, content, handler);
  }

  scoped_ptr<ThreadSystem> thread_system_;
  MockTimer mock_timer_;
  MockHasher mock_hasher_;
  LRUCache cache1_;
  LRUCache cache2_;
  scoped_ptr<WriteThroughHTTPCache> http_cache_;
  GoogleMessageHandler message_handler_;
  SimpleStats simple_stats_;

  const GoogleString key_;
  const GoogleString key2_;
  const GoogleString fragment_;
  const GoogleString content_;
  const GoogleString header_name_;
  const GoogleString header_value_;

  int64 cache1_ms_;
  int64 cache2_ms_;

 private:
  DISALLOW_COPY_AND_ASSIGN(WriteThroughHTTPCacheTest);
};


// Simple flow of putting in an item, getting it.
TEST_F(WriteThroughHTTPCacheTest, PutGet) {
  ClearStats();
  ResponseHeaders headers_in;
  InitHeaders(&headers_in, "max-age=300");
  Put(key_, fragment_, &headers_in, content_, &message_handler_);
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(0, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(1, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(1, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());

  CheckCachedValueValid();
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(1, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(1, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  EXPECT_EQ(0, cache1_ms_);
  EXPECT_EQ(-1, cache2_ms_);

  // Remove the entry from cache1. We find it in cache2. The value is also now
  // inserted into cache1.
  cache1_.Clear();
  CheckCachedValueValid();
  EXPECT_EQ(2, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(1, cache1_.num_misses());
  EXPECT_EQ(2, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(1, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(1, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  EXPECT_EQ(0, cache1_ms_);
  EXPECT_EQ(0, cache2_ms_);

  // Now advance time 301 seconds and the we should no longer
  // be able to fetch this resource out of the cache. Note that we check both
  // the local and remote cache in this case.
  mock_timer_.AdvanceMs(301 * 1000);
  CheckCachedValueExpired();
  EXPECT_EQ(2, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(2, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(2, cache1_.num_hits());
  EXPECT_EQ(1, cache1_.num_misses());
  EXPECT_EQ(2, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(2, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(1, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  EXPECT_EQ(0, cache1_ms_);
  EXPECT_EQ(0, cache2_ms_);

  ClearStats();
  // Test that fallback_http_value() is set correctly.
  FakeHttpCacheCallback callback(thread_system_.get());
  http_cache_->Find(key_, fragment_, &message_handler_, &callback);
  EXPECT_EQ(HTTPCache::kNotFound, callback.result_);
  EXPECT_FALSE(callback.fallback_http_value()->Empty());
  EXPECT_TRUE(callback.http_value()->Empty());
  StringPiece content;
  EXPECT_TRUE(callback.fallback_http_value()->ExtractContents(&content));
  EXPECT_STREQ(content_, content);
  // We find a stale response in the L1 cache, clear it and use the stale
  // response in the L2 cache instead.
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(2, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(1, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());

  // Create a temporary HTTPCache with just cache1 and insert a stale response
  // into it. We use the fallback from cache2.
  HTTPCache temp_l1_cache(&cache1_, &mock_timer_, &mock_hasher_,
                          &simple_stats_);
  // Force caching so that the stale response is inserted.
  temp_l1_cache.set_force_caching(true);
  temp_l1_cache.Put(key_, fragment_, RequestHeaders::Properties(),
                    ResponseHeaders::kRespectVaryOnResources,
                    &headers_in, "new", &message_handler_);
  ClearStats();
  FakeHttpCacheCallback callback2(thread_system_.get());
  http_cache_->Find(key_, fragment_, &message_handler_, &callback2);
  EXPECT_EQ(HTTPCache::kNotFound, callback2.result_);
  EXPECT_FALSE(callback2.fallback_http_value()->Empty());
  EXPECT_TRUE(callback2.http_value()->Empty());
  StringPiece content2;
  EXPECT_TRUE(callback2.fallback_http_value()->ExtractContents(&content2));
  EXPECT_STREQ(content_, content2);
  // We find a stale response in the L1 cache, clear it and use the stale
  // response in the L2 cache instead.
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(2, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(1, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());

  ClearStats();
  // Clear cache2. We now use the fallback from cache1.
  cache2_.Clear();
  FakeHttpCacheCallback callback3(thread_system_.get());
  http_cache_->Find(key_, fragment_, &message_handler_, &callback3);
  EXPECT_EQ(HTTPCache::kNotFound, callback3.result_);
  EXPECT_FALSE(callback3.fallback_http_value()->Empty());
  EXPECT_TRUE(callback3.http_value()->Empty());
  StringPiece content3;
  EXPECT_TRUE(callback3.fallback_http_value()->ExtractContents(&content3));
  EXPECT_STREQ("new", content3);
  // We find a stale response in cache1. Since we don't find anything in cache2,
  // we use the stale response from cache1
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(1, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
}

// Check size-limits for the small cache
TEST_F(WriteThroughHTTPCacheTest, SizeLimit) {
  ClearStats();
  http_cache_->set_cache1_limit(180);  // Empirically based.
  ResponseHeaders headers_in;
  InitHeaders(&headers_in, "max-age=300");

  // This one will fit. (The key is 21 bytes, the fragment is 12 bytes, there's
  // a 1-byte separator in making the composite key, and the HTTPValue is 139
  // bytes).
  Put(key_, fragment_, &headers_in, "Name", &message_handler_);
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(1, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(1, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  // This one will not. (The key is the same 34 bytes as above after combining
  // and the HTTPValue is 150 bytes).
  Put(key2_, fragment_, &headers_in, "TooBigForCache1", &message_handler_);
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(2, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(1, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(2, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
}

TEST_F(WriteThroughHTTPCacheTest, PutGetForHttps) {
  ClearStats();
  ResponseHeaders meta_data_in, meta_data_out;
  InitHeaders(&meta_data_in, "max-age=300");
  meta_data_in.Replace(HttpAttributes::kContentType,
                       kContentTypeHtml.mime_type());
  meta_data_in.ComputeCaching();
  // Disable caching of html on https.
  http_cache_->set_disable_html_caching_on_https(true);
  // The html response does not get cached.
  Put(kHttpsUrl, fragment_, &meta_data_in, "content", &message_handler_);
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  HTTPValue value;
  HTTPCache::FindResult found = Find(
      kHttpsUrl, fragment_, &value, &meta_data_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kNotFound, found);

  // However a css file is cached.
  meta_data_in.Replace(HttpAttributes::kContentType,
                       kContentTypeCss.mime_type());
  meta_data_in.ComputeCaching();
  Put(kHttpsUrl, fragment_, &meta_data_in, "content", &message_handler_);
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  found = Find(kHttpsUrl, fragment_, &value, &meta_data_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kFound, found);
  ASSERT_TRUE(meta_data_out.headers_complete());
  StringPiece contents;
  ASSERT_TRUE(value.ExtractContents(&contents));
  ConstStringStarVector values;
  ASSERT_TRUE(meta_data_out.Lookup("name", &values));
  ASSERT_EQ(static_cast<size_t>(1), values.size());
  EXPECT_EQ(GoogleString("value"), *(values[0]));
  EXPECT_EQ("content", contents);
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));
}

// Verifies that the cache will 'remember' that a fetch should not be
// cached for 5 minutes.
TEST_F(WriteThroughHTTPCacheTest, RememberFetchFailedOrNotCacheable) {
  ClearStats();
  ResponseHeaders headers_out;
  http_cache_->RememberFetchFailed(key_, fragment_, &message_handler_);
  HTTPValue value;
  EXPECT_EQ(HTTPCache::kRecentFetchFailed,
            Find(key_, fragment_, &value, &headers_out, &message_handler_));

  // Now advance time 301 seconds; the cache should allow us to try fetching
  // again.
  mock_timer_.AdvanceMs(301 * 1000);
  EXPECT_EQ(HTTPCache::kNotFound,
            Find(key_, fragment_, &value, &headers_out, &message_handler_));
}

TEST_F(WriteThroughHTTPCacheTest, RememberFetchDropped) {
  ClearStats();
  ResponseHeaders headers_out;
  http_cache_->RememberFetchDropped(key_, fragment_, &message_handler_);
  HTTPValue value;
  EXPECT_EQ(HTTPCache::kRecentFetchFailed,
            Find(key_, fragment_, &value, &headers_out, &message_handler_));

  // Now advance time 11 seconds; the cache should allow us to try fetching
  // again.
  mock_timer_.AdvanceMs(11 * Timer::kSecondMs);
  EXPECT_EQ(HTTPCache::kNotFound,
            Find(key_, fragment_, &value, &headers_out, &message_handler_));
}

// Make sure we don't remember 'non-cacheable' once we've put it into
// SetIgnoreFailurePuts() mode (but do before)
TEST_F(WriteThroughHTTPCacheTest, SetIgnoreFailurePuts) {
  ClearStats();
  http_cache_->RememberNotCacheable(key_, fragment_, false, &message_handler_);
  http_cache_->SetIgnoreFailurePuts();
  http_cache_->RememberNotCacheable(key2_, fragment_, false, &message_handler_);
  ResponseHeaders headers_out;
  HTTPValue value_out;
  EXPECT_EQ(
      HTTPCache::kRecentFetchNotCacheable,
      Find(key_, fragment_, &value_out, &headers_out, &message_handler_));
  EXPECT_EQ(
      HTTPCache::kNotFound,
      Find(key2_, fragment_, &value_out, &headers_out, &message_handler_));
}

TEST_F(WriteThroughHTTPCacheTest, Uncacheable) {
  ClearStats();
  ResponseHeaders headers_in, headers_out;
  InitHeaders(&headers_in, NULL);
  Put(key_, fragment_, &headers_in, content_, &message_handler_);
  HTTPValue value;
  HTTPCache::FindResult found = Find(
      key_, fragment_, &value, &headers_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kNotFound, found);
  ASSERT_FALSE(headers_out.headers_complete());
}

TEST_F(WriteThroughHTTPCacheTest, UncacheablePrivate) {
  ClearStats();
  ResponseHeaders headers_in, headers_out;
  InitHeaders(&headers_in, "private, max-age=300");
  Put(key_, fragment_, &headers_in, content_, &message_handler_);
  HTTPValue value;
  HTTPCache::FindResult found = Find(
      key_, fragment_, &value, &headers_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kNotFound, found);
  ASSERT_FALSE(headers_out.headers_complete());
}

// Unit testing cache invalidation.
TEST_F(WriteThroughHTTPCacheTest, CacheInvalidation) {
  ClearStats();
  ResponseHeaders meta_data_in;
  InitHeaders(&meta_data_in, "max-age=300");
  Put(key_, fragment_, &meta_data_in, content_, &message_handler_);
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(1, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(1, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());

  // Check with both caches valid...
  ClearStats();
  FakeHttpCacheCallback callback1(thread_system_.get());
  http_cache_->Find(key_, fragment_, &message_handler_, &callback1);
  EXPECT_TRUE(callback1.called_);
  // ... only goes to cache1_ and hits.
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  EXPECT_EQ(HTTPCache::kFound, callback1.result_);

  // Check with local cache invalid and remote cache valid...
  ClearStats();
  FakeHttpCacheCallback callback2(thread_system_.get());
  callback2.first_cache_valid_ = false;
  http_cache_->Find(key_, fragment_, &message_handler_, &callback2);
  EXPECT_TRUE(callback2.called_);
  // ... hits both cache1_ (invalidated later by callback2) and cache_2.
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(1, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  // The insert in cache1_ is a reinsert.
  EXPECT_EQ(1, cache1_.num_identical_reinserts());
  EXPECT_EQ(HTTPCache::kFound, callback2.result_);

  // Check with both caches invalid...
  ClearStats();
  FakeHttpCacheCallback callback3(thread_system_.get());
  callback3.first_cache_valid_ = false;
  callback3.second_cache_valid_ = false;
  http_cache_->Find(key_, fragment_, &message_handler_, &callback3);
  EXPECT_TRUE(callback3.called_);
  // ... hits both cache1_ and cache_2. Both invalidated by callback3. So
  // http_cache_ misses.
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(1, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  EXPECT_EQ(HTTPCache::kNotFound, callback3.result_);

  // Check with local cache valid and remote cache invalid...
  ClearStats();
  FakeHttpCacheCallback callback4(thread_system_.get());
  callback4.second_cache_valid_ = false;
  http_cache_->Find(key_, fragment_, &message_handler_, &callback4);
  EXPECT_TRUE(callback4.called_);
  // ... only goes to cache1_ and hits.
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  EXPECT_EQ(HTTPCache::kFound, callback4.result_);
}

// Unit testing cache freshness.
TEST_F(WriteThroughHTTPCacheTest, CacheFreshness) {
  ClearStats();
  ResponseHeaders meta_data_in;
  InitHeaders(&meta_data_in, "max-age=300");
  Put(key_, fragment_, &meta_data_in, content_, &message_handler_);
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(1, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(1, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());

  // Check with both caches freshe...
  ClearStats();
  FakeHttpCacheCallback callback1(thread_system_.get());
  http_cache_->Find(key_, fragment_, &message_handler_, &callback1);
  EXPECT_TRUE(callback1.called_);
  // ... only goes to cache1_ and hits.
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  EXPECT_EQ(HTTPCache::kFound, callback1.result_);

  // Check with local cache not fresh and remote cache fresh...
  ClearStats();
  FakeHttpCacheCallback callback2(thread_system_.get());
  callback2.first_cache_fresh_ = false;
  http_cache_->Find(key_, fragment_, &message_handler_, &callback2);
  EXPECT_TRUE(callback2.called_);
  // ... hits both cache1_ and cache_2.
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(1, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  // The insert in cache1_ is a reinsert.
  EXPECT_EQ(1, cache1_.num_identical_reinserts());
  EXPECT_EQ(HTTPCache::kFound, callback2.result_);

  // Check with both caches not fresh...
  ClearStats();
  FakeHttpCacheCallback callback3(thread_system_.get());
  callback3.first_cache_fresh_ = false;
  callback3.second_cache_fresh_ = false;
  http_cache_->Find(key_, fragment_, &message_handler_, &callback3);
  EXPECT_TRUE(callback3.called_);
  // ... hits both cache1_ and cache_2. Both aren't fresh. So http_cache_
  // misses.
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheFallbacks));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(1, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  EXPECT_EQ(HTTPCache::kNotFound, callback3.result_);
  EXPECT_FALSE(callback3.fallback_http_value()->Empty());

  // Check with local cache fresh and remote cache not fresh...
  ClearStats();
  FakeHttpCacheCallback callback4(thread_system_.get());
  callback4.second_cache_fresh_ = false;
  http_cache_->Find(key_, fragment_, &message_handler_, &callback4);
  EXPECT_TRUE(callback4.called_);
  // ... only goes to cache1_ and hits.
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheExpirations));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(1, cache1_.num_hits());
  EXPECT_EQ(0, cache1_.num_misses());
  EXPECT_EQ(0, cache1_.num_inserts());
  EXPECT_EQ(0, cache1_.num_deletes());
  EXPECT_EQ(0, cache2_.num_hits());
  EXPECT_EQ(0, cache2_.num_misses());
  EXPECT_EQ(0, cache2_.num_inserts());
  EXPECT_EQ(0, cache2_.num_deletes());
  EXPECT_EQ(HTTPCache::kFound, callback4.result_);
}

}  // namespace net_instaweb
