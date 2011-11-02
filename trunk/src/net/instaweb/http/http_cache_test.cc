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

// Unit-test the lru cache

#include "net/instaweb/http/public/http_cache.h"

#include <cstddef>                     // for size_t
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_hasher.h"
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

class HTTPCacheTest : public testing::Test {
 protected:
  // Helper class for calling Get and Query methods on cache implementations
  // that are blocking in nature (e.g. in-memory LRU or blocking file-system).
  class Callback : public HTTPCache::Callback {
   public:
    Callback() { Reset(); }
    Callback* Reset() {
      called_ = false;
      result_ = HTTPCache::kNotFound;
      cache_valid_ = true;
      return this;
    }
    virtual void Done(HTTPCache::FindResult result) {
      called_ = true;
      result_ = result;
    }
    virtual bool IsCacheValid(const ResponseHeaders& headers) {
      // For unit testing we are simply stubbing IsCacheValid.
      return cache_valid_;
    }
    bool called_;
    HTTPCache::FindResult result_;
    bool cache_valid_;
  };

  static int64 ParseDate(const char* start_date) {
    int64 time_ms;
    ResponseHeaders::ParseTime(start_date, &time_ms);
    return time_ms;
  }

  HTTPCacheTest()
      : mock_timer_(ParseDate(kStartDate)),
        lru_cache_(kMaxSize),
        http_cache_(&lru_cache_, &mock_timer_, &mock_hasher_, simple_stats_) { }

  void InitHeaders(ResponseHeaders* headers, const char* cache_control) {
    headers->Add("name", "value");
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

  HTTPCache::FindResult FindInternal(const GoogleString& key, HTTPValue* value,
                                     ResponseHeaders* headers,
                                     MessageHandler* handler,
                                     Callback& callback) {
    http_cache_.Find(key, handler, &callback);
    EXPECT_TRUE(callback.called_);
    if (callback.result_ == HTTPCache::kFound) {
      value->Link(callback.http_value());
    }
    headers->CopyFrom(*callback.response_headers());
    return callback.result_;
  }

  HTTPCache::FindResult Find(const GoogleString& key, HTTPValue* value,
                             ResponseHeaders* headers,
                             MessageHandler* handler) {
    Callback callback;
    return FindInternal(key, value, headers, handler, callback);
  }

  HTTPCache::FindResult Find(const GoogleString& key, HTTPValue* value,
                             ResponseHeaders* headers,
                             MessageHandler* handler, bool cache_valid) {
    Callback callback;
    callback.cache_valid_ = cache_valid;
    return FindInternal(key, value, headers, handler, callback);
  }

  MockTimer mock_timer_;
  MockHasher mock_hasher_;
  LRUCache lru_cache_;
  HTTPCache http_cache_;
  GoogleMessageHandler message_handler_;
  static SimpleStats* simple_stats_;

 private:
  DISALLOW_COPY_AND_ASSIGN(HTTPCacheTest);
};

SimpleStats* HTTPCacheTest::simple_stats_ = NULL;

// Simple flow of putting in an item, getting it.
TEST_F(HTTPCacheTest, PutGet) {
  ResponseHeaders meta_data_in, meta_data_out;
  InitHeaders(&meta_data_in, "max-age=300");
  http_cache_.Put("mykey", &meta_data_in, "content", &message_handler_);
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  EXPECT_EQ(CacheInterface::kAvailable, http_cache_.Query("mykey"));
  HTTPValue value;
  HTTPCache::FindResult found = Find(
      "mykey", &value, &meta_data_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kFound, found);
  ASSERT_TRUE(meta_data_out.headers_complete());
  StringPiece contents;
  ASSERT_TRUE(value.ExtractContents(&contents));
  ConstStringStarVector values;
  ASSERT_TRUE(meta_data_out.Lookup("name", &values));
  ASSERT_EQ(static_cast<size_t>(1), values.size());
  EXPECT_EQ(GoogleString("value"), *(values[0]));
  EXPECT_EQ("content", contents);
  EXPECT_EQ(2, GetStat(HTTPCache::kCacheHits));  // The "query" counts as a hit.

  // Now advance time 301 seconds and the we should no longer
  // be able to fetch this resource out of the cache.
  mock_timer_.AdvanceMs(301 * 1000);
  found = Find("mykey", &value, &meta_data_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kNotFound, found);
  ASSERT_FALSE(meta_data_out.headers_complete());
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheMisses));
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheExpirations));
}

TEST_F(HTTPCacheTest, EtagsAddedIfAbsent) {
  simple_stats_->Clear();
  ResponseHeaders meta_data_in, meta_data_out;
  InitHeaders(&meta_data_in, "max-age=300");
  http_cache_.Put("mykey", &meta_data_in, "content", &message_handler_);
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));

  HTTPValue value;
  HTTPCache::FindResult found = Find(
      "mykey", &value, &meta_data_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kFound, found);
  ASSERT_TRUE(meta_data_out.headers_complete());

  StringPiece contents;
  ASSERT_TRUE(value.ExtractContents(&contents));
  ConstStringStarVector values;
  ASSERT_TRUE(meta_data_out.Lookup("name", &values));
  ASSERT_EQ(static_cast<size_t>(1), values.size());
  EXPECT_EQ(GoogleString("value"), *(values[0]));
  EXPECT_STREQ("W/PSA-0", meta_data_out.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ("content", contents);
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));  // The "query" counts as a hit.
}

TEST_F(HTTPCacheTest, EtagsNotAddedIfPresent) {
  simple_stats_->Clear();
  ResponseHeaders meta_data_in, meta_data_out;
  meta_data_in.Add(HttpAttributes::kEtag, "Etag!");
  InitHeaders(&meta_data_in, "max-age=300");
  http_cache_.Put("mykey", &meta_data_in, "content", &message_handler_);
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));

  HTTPValue value;
  HTTPCache::FindResult found = Find(
      "mykey", &value, &meta_data_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kFound, found);
  ASSERT_TRUE(meta_data_out.headers_complete());

  StringPiece contents;
  ASSERT_TRUE(value.ExtractContents(&contents));
  ConstStringStarVector values;
  ASSERT_TRUE(meta_data_out.Lookup("name", &values));
  ASSERT_EQ(static_cast<size_t>(1), values.size());
  EXPECT_EQ(GoogleString("value"), *(values[0]));
  EXPECT_STREQ("Etag!", meta_data_out.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ("content", contents);
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));  // The "query" counts as a hit.
}

TEST_F(HTTPCacheTest, CookiesNotCached) {
  simple_stats_->Clear();
  ResponseHeaders meta_data_in, meta_data_out;
  meta_data_in.Add(HttpAttributes::kSetCookie, "cookies!");
  meta_data_in.Add(HttpAttributes::kSetCookie2, "more cookies!");
  InitHeaders(&meta_data_in, "max-age=300");
  http_cache_.Put("mykey", &meta_data_in, "content", &message_handler_);
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheInserts));
  EXPECT_EQ(0, GetStat(HTTPCache::kCacheHits));
  HTTPValue value;
  HTTPCache::FindResult found = Find(
      "mykey", &value, &meta_data_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kFound, found);
  ASSERT_TRUE(meta_data_out.headers_complete());
  StringPiece contents;
  ASSERT_TRUE(value.ExtractContents(&contents));
  ConstStringStarVector values;
  ASSERT_TRUE(meta_data_out.Lookup("name", &values));
  ASSERT_EQ(static_cast<size_t>(1), values.size());
  EXPECT_EQ(GoogleString("value"), *(values[0]));
  EXPECT_FALSE(meta_data_out.Lookup(HttpAttributes::kSetCookie, &values));
  EXPECT_FALSE(meta_data_out.Lookup(HttpAttributes::kSetCookie2, &values));
  EXPECT_EQ("content", contents);
  EXPECT_EQ(1, GetStat(HTTPCache::kCacheHits));  // The "query" counts as a hit.
}

// Verifies that the cache will 'remember' that a fetch failed for
// remember_fetch_failed_ttl_seconds_.
TEST_F(HTTPCacheTest, RememberFetchFailed) {
  ResponseHeaders meta_data_out;
  http_cache_.RememberFetchFailed("mykey", &message_handler_);
  HTTPValue value;
  EXPECT_EQ(HTTPCache::kRecentFetchFailedOrNotCacheable,
            Find("mykey", &value, &meta_data_out, &message_handler_));

  // Now advance time 301 seconds; the cache should allow us to try fetching
  // again.
  mock_timer_.AdvanceMs(301 * 1000);
  EXPECT_EQ(HTTPCache::kNotFound,
            Find("mykey", &value, &meta_data_out, &message_handler_));

  http_cache_.set_remember_fetch_failed_ttl_seconds(600);
  http_cache_.RememberFetchFailed("mykey", &message_handler_);
  // Now advance time 301 seconds; the cache should remember that the fetch
  // failed previously.
  mock_timer_.AdvanceMs(301 * 1000);
  EXPECT_EQ(HTTPCache::kRecentFetchFailedOrNotCacheable,
            Find("mykey", &value, &meta_data_out, &message_handler_));
}

// Verifies that the cache will 'remember' 'non-cacheable' for
// remember_not_cacheable_ttl_seconds_.
TEST_F(HTTPCacheTest, RememberNotCacheable) {
  ResponseHeaders meta_data_out;
  http_cache_.RememberNotCacheable("mykey", &message_handler_);
  HTTPValue value;
  EXPECT_EQ(HTTPCache::kRecentFetchFailedOrNotCacheable,
            Find("mykey", &value, &meta_data_out, &message_handler_));

  // Now advance time 301 seconds; the cache should allow us to try fetching
  // again.
  mock_timer_.AdvanceMs(301 * 1000);
  EXPECT_EQ(HTTPCache::kNotFound,
            Find("mykey", &value, &meta_data_out, &message_handler_));

  http_cache_.set_remember_not_cacheable_ttl_seconds(600);
  http_cache_.RememberNotCacheable("mykey", &message_handler_);
  // Now advance time 301 seconds; the cache should remember that the fetch
  // failed previously.
  mock_timer_.AdvanceMs(301 * 1000);
  EXPECT_EQ(HTTPCache::kRecentFetchFailedOrNotCacheable,
            Find("mykey", &value, &meta_data_out, &message_handler_));
}

// Make sure we don't remember 'non-cacheable' once we've put it into r/o mode.
// (but do before)
TEST_F(HTTPCacheTest, ReadOnly) {
  http_cache_.RememberNotCacheable("mykey", &message_handler_);
  http_cache_.SetReadOnly();
  http_cache_.RememberNotCacheable("mykey2", &message_handler_);
  ResponseHeaders meta_data_out;
  HTTPValue value_out;
  EXPECT_EQ(HTTPCache::kRecentFetchFailedOrNotCacheable,
            Find("mykey", &value_out, &meta_data_out, &message_handler_));
  EXPECT_EQ(HTTPCache::kNotFound,
            Find("mykey2", &value_out, &meta_data_out, &message_handler_));
}

TEST_F(HTTPCacheTest, Uncacheable) {
  ResponseHeaders meta_data_in, meta_data_out;
  InitHeaders(&meta_data_in, NULL);
  http_cache_.Put("mykey", &meta_data_in, "content", &message_handler_);
  EXPECT_EQ(CacheInterface::kNotFound, http_cache_.Query("mykey"));
  HTTPValue value;
  HTTPCache::FindResult found = Find(
      "mykey", &value, &meta_data_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kNotFound, found);
  ASSERT_FALSE(meta_data_out.headers_complete());
}

TEST_F(HTTPCacheTest, UncacheablePrivate) {
  ResponseHeaders meta_data_in, meta_data_out;
  InitHeaders(&meta_data_in, "private, max-age=300");
  http_cache_.Put("mykey", &meta_data_in, "content", &message_handler_);
  EXPECT_EQ(CacheInterface::kNotFound, http_cache_.Query("mykey"));
  HTTPValue value;
  HTTPCache::FindResult found = Find(
      "mykey", &value, &meta_data_out, &message_handler_);
  ASSERT_EQ(HTTPCache::kNotFound, found);
  ASSERT_FALSE(meta_data_out.headers_complete());
}

// Unit testing cache invalidation.
TEST_F(HTTPCacheTest, CacheInvalidation) {
  ResponseHeaders meta_data_in, meta_data_out;
  InitHeaders(&meta_data_in, "max-age=300");
  http_cache_.Put("mykey", &meta_data_in, "content", &message_handler_);
  HTTPValue value;
  // Check with cache valid.
  EXPECT_EQ(HTTPCache::kFound,
            Find("mykey", &value, &meta_data_out, &message_handler_, true));
  // Check with cache invalidated.
  EXPECT_EQ(HTTPCache::kNotFound,
            Find("mykey", &value, &meta_data_out, &message_handler_, false));
}

}  // namespace net_instaweb
