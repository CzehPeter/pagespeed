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
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/simple_stats.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace {
// Set the cache size large enough so nothing gets evicted during this test.
const int kMaxSize = 10000;
const char kStartDate[] = "Sun, 16 Dec 1979 02:27:45 GMT";
}

namespace net_instaweb {

class MessageHandler;

class WriteThroughHTTPCacheTest : public testing::Test {
 protected:
  // Helper class for calling Get and Query methods on cache implementations
  // that are blocking in nature (e.g. in-memory LRU or blocking file-system).
  class Callback : public HTTPCache::Callback {
   public:
    Callback() { Reset(); }
    Callback* Reset() {
      called_ = false;
      result_ = HTTPCache::kNotFound;
      first_call_ = true;
      first_cache_valid_ = true;
      second_cache_valid_ = true;
      return this;
    }
    virtual void Done(HTTPCache::FindResult result) {
      called_ = true;
      result_ = result;
    }
    virtual bool IsCacheValid(const ResponseHeaders& headers) {
      bool result = first_call_? first_cache_valid_ : second_cache_valid_;
      first_call_ = false;
      return result;
    }
    bool called_;
    HTTPCache::FindResult result_;
    bool first_call_;
    bool first_cache_valid_;
    bool second_cache_valid_;
  };

  static int64 ParseDate(const char* start_date) {
    int64 time_ms;
    ResponseHeaders::ParseTime(start_date, &time_ms);
    return time_ms;
  }

  WriteThroughHTTPCacheTest() :
    mock_timer_(ParseDate(kStartDate)),
    cache1_(kMaxSize), cache2_(kMaxSize),
    http_cache_(&cache1_, &cache2_, &mock_timer_, simple_stats_),
    key_("mykey"), content_("content"), header_name_("name"),
    header_value_("value") {
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

  int GetStat(const char* stat_name) {
    return simple_stats_->FindVariable(stat_name)->Get();
  }

  static void SetUpTestCase() {
    testing::Test::SetUpTestCase();
    simple_stats_ = new SimpleStats;
    HTTPCache::Initialize(simple_stats_);
  }

  static void TearDownTestCase() {
    delete simple_stats_;
    testing::Test::TearDownTestCase();
  }

  HTTPCache::FindResult Find(const GoogleString& key, HTTPValue* value,
                             ResponseHeaders* headers,
                             MessageHandler* handler) {
    Callback callback;
    http_cache_.Find(key, handler, &callback);
    EXPECT_TRUE(callback.called_);
    if (callback.result_ == HTTPCache::kFound) {
      value->Link(callback.http_value());
    }
    headers->CopyFrom(*callback.response_headers());
    return callback.result_;
  }

  void CheckCachedValueValid() {
    HTTPValue value;
    ResponseHeaders headers;
    HTTPCache::FindResult found = Find(
        key_, &value, &headers, &message_handler_);
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
    HTTPCache::FindResult found = Find(key_, &value, &headers,
                                       &message_handler_);
    EXPECT_EQ(HTTPCache::kNotFound, found);
    EXPECT_FALSE(headers.headers_complete());
  }

  void CheckStats(int http_cache_hits, int http_cache_misses,
      int http_cache_expirations, int http_cache_inserts, int cache1_hits,
      int cache1_misses, int cache1_inserts, int cache1_deletes,
      int cache2_hits, int cache2_misses, int cache2_inserts,
      int cache2_deletes) {
    EXPECT_EQ(http_cache_hits, GetStat(HTTPCache::kCacheHits));
    EXPECT_EQ(http_cache_misses, GetStat(HTTPCache::kCacheMisses));
    EXPECT_EQ(http_cache_expirations, GetStat(HTTPCache::kCacheExpirations));
    EXPECT_EQ(http_cache_inserts, GetStat(HTTPCache::kCacheInserts));
    EXPECT_EQ(cache1_hits, cache1_.num_hits());
    EXPECT_EQ(cache1_misses, cache1_.num_misses());
    EXPECT_EQ(cache1_inserts, cache1_.num_inserts());
    EXPECT_EQ(cache1_deletes, cache1_.num_deletes());
    EXPECT_EQ(cache2_hits, cache2_.num_hits());
    EXPECT_EQ(cache2_misses, cache2_.num_misses());
    EXPECT_EQ(cache2_inserts, cache2_.num_inserts());
    EXPECT_EQ(cache2_deletes, cache2_.num_deletes());
  }

  void ClearStats() {
    cache1_.ClearStats();
    cache2_.ClearStats();
    simple_stats_->Clear();
  }

  MockTimer mock_timer_;
  LRUCache cache1_;
  LRUCache cache2_;
  WriteThroughHTTPCache http_cache_;
  GoogleMessageHandler message_handler_;
  static SimpleStats* simple_stats_;

  const GoogleString key_;
  const GoogleString content_;
  const GoogleString header_name_;
  const GoogleString header_value_;

 private:
  DISALLOW_COPY_AND_ASSIGN(WriteThroughHTTPCacheTest);
};

SimpleStats* WriteThroughHTTPCacheTest::simple_stats_ = NULL;

// Simple flow of putting in an item, getting it.
TEST_F(WriteThroughHTTPCacheTest, PutGet) {
  ClearStats();
  ResponseHeaders headers_in;
  InitHeaders(&headers_in, "max-age=300");
  http_cache_.Put(key_, &headers_in, content_, &message_handler_);
  CheckStats(0 /* HTTP cache hits */, 0 /* HTTP cache misses */,
             0 /* HTTP cache expirations */, 1 /* HTTP cache inserts */,
             0 /* Cache 1 hits */, 0 /* Cache 1 misses */,
             1 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             0 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             1 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);
  EXPECT_EQ(CacheInterface::kAvailable, http_cache_.Query(key_));
  CheckCachedValueValid();
  CheckStats(2 /* HTTP cache hits */, 0 /* HTTP cache misses */,
             0 /* HTTP cache expirations */, 1 /* HTTP cache inserts */,
             2 /* Cache 1 hits */, 0 /* Cache 1 misses */,
             1 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             0 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             1 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);

  // Remove the entry from cache1. We find it in cache2. The value is also now
  // inserted into cache1.
  cache1_.Clear();
  CheckCachedValueValid();
  CheckStats(3 /* HTTP cache hits */, 0 /* HTTP cache misses */,
             0 /* HTTP cache expirations */, 1 /* HTTP cache inserts */,
             2 /* Cache 1 hits */, 1 /* Cache 1 misses */,
             2 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             1 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             1 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);

  // Now advance time 301 seconds and the we should no longer
  // be able to fetch this resource out of the cache. Note that we check both
  // the local and remote cache in this case.
  mock_timer_.AdvanceMs(301 * 1000);
  CheckCachedValueExpired();
  CheckStats(3 /* HTTP cache hits */, 1 /* HTTP cache misses */,
             2 /* HTTP cache expirations */, 1 /* HTTP cache inserts */,
             3 /* Cache 1 hits */, 1 /* Cache 1 misses */,
             2 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             2 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             1 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);
}

// Check size-limits for the small cache
TEST_F(WriteThroughHTTPCacheTest, SizeLimit) {
  ClearStats();
  http_cache_.set_cache1_limit(130);  // Empirically based.
  ResponseHeaders headers_in;
  InitHeaders(&headers_in, "max-age=300");

  // This one will fit. (The key is 5 bytes and the HTTPValue is 122bytes).
  http_cache_.Put(key_, &headers_in, "Name", &message_handler_);
  CheckStats(0 /* HTTP cache hits */, 0/* HTTP cache misses */,
             0 /* HTTP cache expirations */, 1 /* HTTP cache inserts */,
             0 /* Cache 1 hits */, 0 /* Cache 1 misses */,
             1 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             0 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             1 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);

  // This one will not. (The key is 3 bytes and the HTTPValue is 133bytes).
  http_cache_.Put("new", &headers_in, "TooBigForCache1", &message_handler_);
  CheckStats(0 /* HTTP cache hits */, 0/* HTTP cache misses */,
             0 /* HTTP cache expirations */, 2 /* HTTP cache inserts */,
             0 /* Cache 1 hits */, 0 /* Cache 1 misses */,
             1 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             0 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             2 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);
}

// Verifies that the cache will 'remember' that a fetch should not be
// cached for 5 minutes.
TEST_F(WriteThroughHTTPCacheTest, RememberFetchFailedOrNotCacheable) {
  ClearStats();
  ResponseHeaders headers_out;
  http_cache_.RememberFetchFailed(key_, &message_handler_);
  HTTPValue value;
  EXPECT_EQ(HTTPCache::kRecentFetchFailedOrNotCacheable,
            Find(key_, &value, &headers_out, &message_handler_));

  // Now advance time 301 seconds; the cache should allow us to try fetching
  // again.
  mock_timer_.AdvanceMs(301 * 1000);
  EXPECT_EQ(HTTPCache::kNotFound,
            Find(key_, &value, &headers_out, &message_handler_));
}

// Make sure we don't remember 'non-cacheable' once we've put it into
// SetIgnoreFailurePuts() mode (but do before)
TEST_F(WriteThroughHTTPCacheTest, SetIgnoreFailurePuts) {
  ClearStats();
  http_cache_.RememberNotCacheable(key_, &message_handler_);
  http_cache_.SetIgnoreFailurePuts();
  http_cache_.RememberNotCacheable("mykey2", &message_handler_);
  ResponseHeaders headers_out;
  HTTPValue value_out;
  EXPECT_EQ(HTTPCache::kRecentFetchFailedOrNotCacheable,
            Find(key_, &value_out, &headers_out, &message_handler_));
  EXPECT_EQ(HTTPCache::kNotFound,
            Find("mykey2", &value_out, &headers_out, &message_handler_));
}

TEST_F(WriteThroughHTTPCacheTest, Uncacheable) {
  ClearStats();
  ResponseHeaders headers_in, headers_out;
  InitHeaders(&headers_in, NULL);
  http_cache_.Put(key_, &headers_in, content_, &message_handler_);
  EXPECT_EQ(CacheInterface::kNotFound, http_cache_.Query(key_));
  HTTPValue value;
  HTTPCache::FindResult found = Find(
      key_, &value, &headers_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kNotFound, found);
  ASSERT_FALSE(headers_out.headers_complete());
}

TEST_F(WriteThroughHTTPCacheTest, UncacheablePrivate) {
  ClearStats();
  ResponseHeaders headers_in, headers_out;
  InitHeaders(&headers_in, "private, max-age=300");
  http_cache_.Put(key_, &headers_in, content_, &message_handler_);
  EXPECT_EQ(CacheInterface::kNotFound, http_cache_.Query(key_));
  HTTPValue value;
  HTTPCache::FindResult found = Find(
      key_, &value, &headers_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kNotFound, found);
  ASSERT_FALSE(headers_out.headers_complete());
}

// Unit testing cache invalidation.
TEST_F(WriteThroughHTTPCacheTest, CacheInvalidation) {
  ClearStats();
  ResponseHeaders meta_data_in;
  InitHeaders(&meta_data_in, "max-age=300");
  http_cache_.Put(key_, &meta_data_in, content_, &message_handler_);
  CheckStats(0 /* HTTP cache hits */, 0 /* HTTP cache misses */,
             0 /* HTTP cache expirations */, 1 /* HTTP cache inserts */,
             0 /* Cache 1 hits */, 0 /* Cache 1 misses */,
             1 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             0 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             1 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);

  // Check with both caches valid...
  ClearStats();
  Callback callback1;
  http_cache_.Find(key_, &message_handler_, &callback1);
  EXPECT_TRUE(callback1.called_);
  // ... only goes to cache1_ and hits.
  CheckStats(1 /* HTTP cache hits */, 0 /* HTTP cache misses */,
             0 /* HTTP cache expirations */, 0 /* HTTP cache inserts */,
             1 /* Cache 1 hits */, 0 /* Cache 1 misses */,
             0 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             0 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             0 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);
  EXPECT_EQ(HTTPCache::kFound, callback1.result_);

  // Check with local cache invalid and remote cache valid...
  ClearStats();
  Callback callback2;
  callback2.first_cache_valid_ = false;
  http_cache_.Find(key_, &message_handler_, &callback2);
  EXPECT_TRUE(callback2.called_);
  // ... hits both cache1_ (invalidated later by callback2) and cache_2.
  CheckStats(1 /* HTTP cache hits */, 0 /* HTTP cache misses */,
             0 /* HTTP cache expirations */, 0 /* HTTP cache inserts */,
             1 /* Cache 1 hits */, 0 /* Cache 1 misses */,
             0 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             1 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             0 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);
  // The insert in cache1_ is a reinsert.
  EXPECT_EQ(1, cache1_.num_identical_reinserts());
  EXPECT_EQ(HTTPCache::kFound, callback2.result_);

  // Check with both caches invalid...
  ClearStats();
  Callback callback3;
  callback3.first_cache_valid_ = false;
  callback3.second_cache_valid_ = false;
  http_cache_.Find(key_, &message_handler_, &callback3);
  EXPECT_TRUE(callback3.called_);
  // ... hits both cache1_ and cache_2. Both invalidated by callback3. So
  // http_cache_ misses.
  CheckStats(0 /* HTTP cache hits */, 1 /* HTTP cache misses */,
             0 /* HTTP cache expirations */, 0 /* HTTP cache inserts */,
             1 /* Cache 1 hits */, 0 /* Cache 1 misses */,
             0 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             1 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             0 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);
  EXPECT_EQ(HTTPCache::kNotFound, callback3.result_);

  // Check with local cache valid and remote cache invalid...
  ClearStats();
  Callback callback4;
  callback4.second_cache_valid_ = false;
  http_cache_.Find(key_, &message_handler_, &callback4);
  EXPECT_TRUE(callback4.called_);
  // ... only goes to cache1_ and hits.
  CheckStats(1 /* HTTP cache hits */, 0 /* HTTP cache misses */,
             0 /* HTTP cache expirations */, 0 /* HTTP cache inserts */,
             1 /* Cache 1 hits */, 0 /* Cache 1 misses */,
             0 /* Cache 1 inserts */, 0 /* Cache 1 deletes */,
             0 /* Cache 2 hits */, 0 /* Cache 2 misses */,
             0 /* Cache 2 inserts */, 0 /* Cache 2 deletes */);
  EXPECT_EQ(HTTPCache::kFound, callback4.result_);
}

}  // namespace net_instaweb
