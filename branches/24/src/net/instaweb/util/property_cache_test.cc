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

// Author: jmarantz@google.com (Joshua Marantz)

// Unit-test the property cache

#include "net/instaweb/util/public/property_cache.h"

#include <cstddef>
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"

namespace net_instaweb {

class AbstractMutex;

namespace {

const size_t kMaxCacheSize = 100;
const char kCohortName1[] = "cohort1";
const char kCohortName2[] = "cohort2";
const char kCacheKey1[] = "Key1";
const char kPropertyName1[] = "prop1";
const char kPropertyName2[] = "prop2";

class PropertyCacheTest : public testing::Test {
 protected:
  PropertyCacheTest()
      : lru_cache_(kMaxCacheSize),
        timer_(MockTimer::kApr_5_2010_ms),
        thread_system_(ThreadSystem::CreateThreadSystem()),
        property_cache_("test/", &lru_cache_, &timer_, thread_system_.get()) {
    cohort_ = property_cache_.AddCohort(kCohortName1);
  }

  class MockPage : public PropertyPage {
   public:
    MockPage(AbstractMutex* mutex, const StringPiece& key)
        : PropertyPage(mutex, key),
          called_(false),
          valid_(false),
          time_ms_(-1) {}
    virtual ~MockPage() {}
    virtual bool IsCacheValid(int64 write_timestamp_ms) const {
      return time_ms_ == -1 || write_timestamp_ms > time_ms_;
    }
    virtual void Done(bool valid) {
      called_ = true;
      valid_ = valid;
    }
    bool called() const { return called_; }
    bool valid() const { return valid_; }
    void set_time_ms(int64 time_ms) {
      time_ms_ = time_ms;
    }

   private:
    bool called_;
    bool valid_;
    int64 time_ms_;

    DISALLOW_COPY_AND_ASSIGN(MockPage);
  };

  // Performs a Read/Modify/Write transaction intended for a cold
  // cache, verifying that this worked.
  //
  // Returns whether the value is considered Stable or not.  In general
  // we would expect this routine to return false.
  bool ReadWriteInitial(const GoogleString& key, const GoogleString& value) {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache_.Read(&page);
    PropertyValue* property = page.GetProperty(cohort_, kPropertyName1);
    EXPECT_FALSE(page.valid());
    EXPECT_TRUE(page.called());
    property_cache_.UpdateValue(value, property);
    property_cache_.WriteCohort(cohort_, &page);
    EXPECT_TRUE(property->has_value());
    return property_cache_.IsStable(property);
  }

  // Performs a Read/Modify/Write transaction intended for a warm
  // cache, verifying that this worked, and that the old-value was
  // previously found.  Returns whether the value was considered
  // stable.
  bool ReadWriteTestStable(const GoogleString& key,
                           const GoogleString& old_value,
                           const GoogleString& new_value) {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache_.Read(&page);
    PropertyValue* property = page.GetProperty(cohort_, kPropertyName1);
    EXPECT_TRUE(page.valid());
    EXPECT_TRUE(page.called());
    EXPECT_STREQ(old_value, property->value());
    property_cache_.UpdateValue(new_value, property);
    property_cache_.WriteCohort(cohort_, &page);
    return property_cache_.IsStable(property);
  }

  // Performs a Read transaction and returns whether the value was considered
  // stable with num_writes_unchanged.
  bool ReadTestRecentlyConstant(const GoogleString& key,
                                int num_writes_unchanged) {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache_.Read(&page);
    PropertyValue* property = page.GetProperty(cohort_, kPropertyName1);
    return property->IsRecentlyConstant(num_writes_unchanged);
  }

  // Performs a Read/Modify/Write transaction and returns whether the value was
  // considered stable with num_writes_unchanged.
  bool ReadWriteTestRecentlyConstant(const GoogleString& key,
                                      const GoogleString& value,
                                      int num_writes_unchanged) {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache_.Read(&page);
    PropertyValue* property = page.GetProperty(cohort_, kPropertyName1);
    property_cache_.UpdateValue(value, property);
    property_cache_.WriteCohort(cohort_, &page);
    return property->IsRecentlyConstant(num_writes_unchanged);
  }

  LRUCache lru_cache_;
  MockTimer timer_;
  scoped_ptr<ThreadSystem> thread_system_;
  PropertyCache property_cache_;
  const PropertyCache::Cohort* cohort_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PropertyCacheTest);
};

TEST_F(PropertyCacheTest, TrackStability) {
  // Tests that the current stability heuristics work as expected.  Note
  // that I don't think the heuristic is really great yet.  It needs some
  // iteration.  The 0.3 threshold comes from
  //     const int kDefaultMutationsPer1000WritesThreshold = 300;
  // in property_cache.cc.
  EXPECT_FALSE(ReadWriteInitial(kCacheKey1, "Value1")) << "1/1 > .300";
  EXPECT_FALSE(ReadWriteTestStable(kCacheKey1, "Value1", "Value1"))
      << "1/2 > .300";
  EXPECT_FALSE(ReadWriteTestStable(kCacheKey1, "Value1", "Value1"))
      << "1/3 > .300";
  EXPECT_TRUE(ReadWriteTestStable(kCacheKey1, "Value1", "Value1"))
      << "1/4 < .300";
  EXPECT_TRUE(ReadWriteTestStable(kCacheKey1, "Value1", "Value1"))
      << "1/5 < .300";
  EXPECT_FALSE(ReadWriteTestStable(kCacheKey1, "Value1", "Value2"))
      << "2/6 > .300";
  EXPECT_TRUE(ReadWriteTestStable(kCacheKey1, "Value2", "Value2"))
      << "2/7 < .300";
  EXPECT_TRUE(ReadWriteTestStable(kCacheKey1, "Value2", "Value2"))
      << "2/8 < .300";

  // Saturate the update-count by looping 62 more times, making 64 straight
  // writes where we did not change the value.
  for (int i = 0; i < 62; ++i) {
    EXPECT_TRUE(ReadWriteTestStable(kCacheKey1, "Value2", "Value2"))
        << "2/8 < .300";
  }

  // Now, to get to less than 300/1000 we'll have to change values 20
  // times.  On the first 19 we'll consider the system stable, but on
  // the 20th, the system figures out this value looks stable enough.
  //
  // TODO(jmarantz): This feels like maybe it's not a good metric, and
  // we should give up sooner once we see the instability.  But at
  // least for now this tests the system is working as expected.
  GoogleString prev_value = "Value2";
  for (int i = 0; i < 19; ++i) {
    GoogleString new_value = StringPrintf("Value%d", i + 3);
    EXPECT_TRUE(ReadWriteTestStable(kCacheKey1, prev_value, new_value)) <<
        " still stable after " << i << " mutations";
    prev_value = new_value;
  }
  EXPECT_FALSE(ReadWriteTestStable(kCacheKey1, prev_value, "Final"))
      << " finally unstable";

  // Now that we have 20 mutations in the system, it will take 64-20=44
  // repeats to flush them out to get back to 19 instabilities.
  for (int i = 0; i < 44; ++i) {
    EXPECT_FALSE(ReadWriteTestStable(kCacheKey1, "Final", "Final"))
        << "still unstable after " << i << " mutations";
  }
  EXPECT_TRUE(ReadWriteTestStable(kCacheKey1, "Final", "Final"))
      << " stable again";
}

TEST_F(PropertyCacheTest, IsIndexOfLeastSetBitSmallerTest) {
  uint64 i = 1;
  EXPECT_FALSE(PropertyValue::IsIndexOfLeastSetBitSmaller(i, 0));
  EXPECT_FALSE(PropertyValue::IsIndexOfLeastSetBitSmaller(i<<1, 0));
  EXPECT_TRUE(PropertyValue::IsIndexOfLeastSetBitSmaller(i<<1, 3));
  EXPECT_TRUE(PropertyValue::IsIndexOfLeastSetBitSmaller(i<<44, 60));

  i = 1;
  // Index of least set bit is 64.
  EXPECT_FALSE(PropertyValue::IsIndexOfLeastSetBitSmaller(i<<63, 64));

  // There is no bit set.
  EXPECT_TRUE(PropertyValue::IsIndexOfLeastSetBitSmaller(i<<1, 64));
}

TEST_F(PropertyCacheTest, TestIsRecentlyConstant) {
  // Nothing written to property_cache so constant.
  EXPECT_TRUE(ReadTestRecentlyConstant(kCacheKey1, 1));
  EXPECT_TRUE(ReadTestRecentlyConstant(kCacheKey1, 2));

  // value1 written once.
  EXPECT_TRUE(ReadWriteTestRecentlyConstant(kCacheKey1, "value1", 1));
  EXPECT_TRUE(ReadTestRecentlyConstant(kCacheKey1, 2));

  // value1 written twice.
  EXPECT_TRUE(ReadWriteTestRecentlyConstant(kCacheKey1, "value1", 2));
  EXPECT_TRUE(ReadTestRecentlyConstant(kCacheKey1, 3));

  // value1 written thrice.
  EXPECT_TRUE(ReadWriteTestRecentlyConstant(kCacheKey1, "value1", 3));
  // A new value is written.
  EXPECT_FALSE(ReadWriteTestRecentlyConstant(kCacheKey1, "value2", 2));

  // value2 written twice.
  EXPECT_TRUE(ReadWriteTestRecentlyConstant(kCacheKey1, "value2", 2));
  EXPECT_FALSE(ReadWriteTestRecentlyConstant(kCacheKey1, "value2", 4));

  // Write same value 44 times.
  for (int i = 0; i < 44; ++i) {
    ReadWriteTestRecentlyConstant(kCacheKey1, "value3", 45);
  }
  EXPECT_TRUE(ReadTestRecentlyConstant(kCacheKey1, 44));
  EXPECT_FALSE(ReadTestRecentlyConstant(kCacheKey1, 46));

  // Write same value for 20 more times.
  for (int i = 0; i < 21; ++i) {
    EXPECT_FALSE(ReadWriteTestRecentlyConstant(kCacheKey1, "value3", 65));
  }
  EXPECT_TRUE(ReadTestRecentlyConstant(kCacheKey1, 64));
}

TEST_F(PropertyCacheTest, DropOldWrites) {
  timer_.SetTimeMs(MockTimer::kApr_5_2010_ms);
  ReadWriteInitial(kCacheKey1, "Value1");
  ReadWriteTestStable(kCacheKey1, "Value1", "Value1");

  // Now imagine we are on a second server, which is trying to write
  // an older value into the same physical cache.  Make sure we don't let it.
  MockTimer timer2(MockTimer::kApr_5_2010_ms - 100);
  PropertyCache property_cache2("test/", &lru_cache_, &timer2,
                                thread_system_.get());
  property_cache2.AddCohort(kCohortName1);
  const PropertyCache::Cohort* cohort2 = property_cache2.GetCohort(
      kCohortName1);
  {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache2.Read(&page);
    EXPECT_TRUE(page.valid());
    EXPECT_TRUE(page.called());
    PropertyValue* property = page.GetProperty(cohort2, kPropertyName1);
    property_cache2.UpdateValue("Value2", property);
    // Stale value dropped.
    property_cache2.WriteCohort(cohort2, &page);
  }
  {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache2.Read(&page);
    EXPECT_TRUE(page.valid());
    EXPECT_TRUE(page.called());
    PropertyValue* property = page.GetProperty(cohort2, kPropertyName1);
    EXPECT_STREQ("Value1", property->value());  // Value2 was dropped.
  }
}

TEST_F(PropertyCacheTest, EmptyReadNewPropertyWasRead) {
  MockPage page(thread_system_->NewMutex(), kCacheKey1);
  property_cache_.Read(&page);
  PropertyValue* property = page.GetProperty(cohort_, kPropertyName1);
  EXPECT_TRUE(property->was_read());
  EXPECT_FALSE(property->has_value());
}

TEST_F(PropertyCacheTest, TwoCohorts) {
  EXPECT_EQ(cohort_, property_cache_.GetCohort(kCohortName1));
  EXPECT_EQ(cohort_, property_cache_.AddCohort(kCohortName1));
  EXPECT_TRUE(property_cache_.GetCohort(kCohortName2) == NULL);
  const PropertyCache::Cohort* cohort2 = property_cache_.AddCohort(
      kCohortName2);
  ReadWriteInitial(kCacheKey1, "Value1");
  EXPECT_EQ(2, lru_cache_.num_misses()) << "one miss per cohort";
  EXPECT_EQ(1, lru_cache_.num_inserts()) << "only cohort1 written";
  lru_cache_.ClearStats();

  // ReadWriteInitial found something for cohort1 but no value has
  // yet been established for cohort2, so we'll get a hit and a miss.
  {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache_.Read(&page);
    EXPECT_EQ(1, lru_cache_.num_hits()) << "cohort1";
    EXPECT_EQ(1, lru_cache_.num_misses()) << "cohort2";
    PropertyValue* p2 = page.GetProperty(cohort2, kPropertyName2);
    EXPECT_TRUE(p2->was_read());
    EXPECT_FALSE(p2->has_value());
    property_cache_.UpdateValue("v2", p2);
    property_cache_.WriteCohort(cohort2, &page);
    EXPECT_EQ(1, lru_cache_.num_inserts()) << "cohort2 written";
  }

  lru_cache_.ClearStats();

  // Now a second read will get two hits, no misses, and both data elements
  // present.
  {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache_.Read(&page);
    EXPECT_EQ(2, lru_cache_.num_hits()) << "both cohorts hit";
    EXPECT_EQ(0, lru_cache_.num_misses());
    PropertyValue* p2 = page.GetProperty(cohort2, kPropertyName2);
    EXPECT_TRUE(p2->was_read());
    EXPECT_TRUE(p2->has_value());
  }
}

TEST_F(PropertyCacheTest, Expiration) {
  timer_.SetTimeMs(MockTimer::kApr_5_2010_ms);
  ReadWriteInitial(kCacheKey1, "Value1");

  // Read a value & make sure it's not expired initially, but expires when
  // we move time forward.
  {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache_.Read(&page);
    PropertyValue* property = page.GetProperty(cohort_, kPropertyName1);

    // Initially it's not expired.
    EXPECT_FALSE(property_cache_.IsExpired(property, Timer::kMinuteMs));
    timer_.AdvanceMs(30 * Timer::kSecondMs);
    EXPECT_FALSE(property_cache_.IsExpired(property, Timer::kMinuteMs));
    timer_.AdvanceMs(30 * Timer::kSecondMs);
    EXPECT_FALSE(property_cache_.IsExpired(property, Timer::kMinuteMs));
    timer_.AdvanceMs(1 * Timer::kSecondMs);
    EXPECT_TRUE(property_cache_.IsExpired(property, Timer::kMinuteMs));
  }
}

TEST_F(PropertyCacheTest, IsCacheValid) {
  timer_.SetTimeMs(MockTimer::kApr_5_2010_ms);
  ReadWriteInitial(kCacheKey1, "Value1");

  {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    // The timestamp for invalidation is older than the write time of value.  So
    // it as valid.
    page.set_time_ms(timer_.NowMs() - 1);
    property_cache_.Read(&page);
    EXPECT_TRUE(page.valid());
    EXPECT_TRUE(page.called());
    PropertyValue* property1 = page.GetProperty(cohort_, kPropertyName1);
    EXPECT_TRUE(property1->has_value());
  }

  {
    // The timestamp for invalidation is newer than the write time of value.  So
    // it as invalid.
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    page.set_time_ms(timer_.NowMs());
    property_cache_.Read(&page);
    EXPECT_FALSE(page.valid());
    EXPECT_TRUE(page.called());
    PropertyValue* property1 = page.GetProperty(cohort_, kPropertyName1);
    EXPECT_FALSE(property1->has_value());
  }
}

TEST_F(PropertyCacheTest, IsCacheValidTwoValuesInACohort) {
  timer_.SetTimeMs(MockTimer::kApr_5_2010_ms);
  MockPage page(thread_system_->NewMutex(), kCacheKey1);
  property_cache_.Read(&page);
  PropertyValue* property = page.GetProperty(cohort_, kPropertyName1);
  property_cache_.UpdateValue("Value1", property);
  timer_.AdvanceMs(2);
  property = page.GetProperty(cohort_, kPropertyName2);
  property_cache_.UpdateValue("Value2", property);
  property_cache_.WriteCohort(cohort_, &page);

  {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    // The timestamp for invalidation is older than the write times of both
    // value.  So they are treated as valid.
    page.set_time_ms(timer_.NowMs() - 3);
    property_cache_.Read(&page);
    EXPECT_TRUE(page.valid());
    EXPECT_TRUE(page.called());
    PropertyValue* property1 = page.GetProperty(cohort_, kPropertyName1);
    PropertyValue* property2 = page.GetProperty(cohort_, kPropertyName2);
    EXPECT_TRUE(property1->has_value());
    EXPECT_TRUE(property2->has_value());
  }

  {
    // The timestamp for invalidation is newer than the write time of one of the
    // values.  So both are treated as invalid.
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    page.set_time_ms(timer_.NowMs() - 1);
    property_cache_.Read(&page);
    EXPECT_FALSE(page.valid());
    EXPECT_TRUE(page.called());
    PropertyValue* property1 = page.GetProperty(cohort_, kPropertyName1);
    PropertyValue* property2 = page.GetProperty(cohort_, kPropertyName2);
    EXPECT_FALSE(property1->has_value());
    EXPECT_FALSE(property2->has_value());
  }
}

TEST_F(PropertyCacheTest, IsCacheValidTwoCohorts) {
  timer_.SetTimeMs(MockTimer::kApr_5_2010_ms);
  const PropertyCache::Cohort* cohort2 = property_cache_.AddCohort(
      kCohortName2);
  MockPage page(thread_system_->NewMutex(), kCacheKey1);
  property_cache_.Read(&page);
  PropertyValue* property = page.GetProperty(cohort_, kPropertyName1);
  property_cache_.UpdateValue("Value1", property);
  timer_.AdvanceMs(2);
  property = page.GetProperty(cohort2, kPropertyName2);
  property_cache_.UpdateValue("Value2", property);
  property_cache_.WriteCohort(cohort_, &page);
  property_cache_.WriteCohort(cohort2, &page);

  {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    // The timestamp for invalidation is older than the write times of values in
    // both cohorts.  So they are treated as valid.
    page.set_time_ms(timer_.NowMs() - 3);
    property_cache_.Read(&page);
    EXPECT_TRUE(page.valid());
    EXPECT_TRUE(page.called());
    PropertyValue* property1 = page.GetProperty(cohort_, kPropertyName1);
    PropertyValue* property2 = page.GetProperty(cohort2, kPropertyName2);
    EXPECT_TRUE(property1->has_value());
    EXPECT_TRUE(property2->has_value());
  }

  {
    // The timestamp for invalidation is newer than the write time of one of the
    // values.  But the the values are in different cohorts and so the page is
    // treated as valid.
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    page.set_time_ms(timer_.NowMs() - 1);
    property_cache_.Read(&page);
    EXPECT_TRUE(page.valid());
    EXPECT_TRUE(page.called());
    PropertyValue* property1 = page.GetProperty(cohort_, kPropertyName1);
    PropertyValue* property2 = page.GetProperty(cohort2, kPropertyName2);
    EXPECT_FALSE(property1->has_value());
    EXPECT_TRUE(property2->has_value());
  }
}

TEST_F(PropertyCacheTest, DeleteProperty) {
  ReadWriteInitial(kCacheKey1, "Value1");
  {
    MockPage page(thread_system_->NewMutex(), kCacheKey1);
    property_cache_.Read(&page);
    EXPECT_TRUE(page.valid());
    EXPECT_TRUE(page.called());
    // Deletes a property which already exists.
    PropertyValue* property = page.GetProperty(cohort_, kPropertyName1);
    EXPECT_STREQ("Value1", property->value());
    page.DeleteProperty(cohort_, kPropertyName1);
    property_cache_.WriteCohort(cohort_, &page);

    property_cache_.Read(&page);
    property = page.GetProperty(cohort_, kPropertyName1);
    EXPECT_FALSE(property->has_value());

    // Deletes a property which does not exist.
    property = page.GetProperty(cohort_, kPropertyName2);
    EXPECT_FALSE(property->has_value());
    page.DeleteProperty(cohort_, kPropertyName2);
    property = page.GetProperty(cohort_, kPropertyName2);
    EXPECT_FALSE(property->has_value());

    // Unknown Cohort. No crashes.
    scoped_ptr<PropertyCache::Cohort> unknown_cohort(new PropertyCache::Cohort);
    page.DeleteProperty(cohort_, kPropertyName2);
    EXPECT_TRUE(page.valid());
  }
}

}  // namespace

}  // namespace net_instaweb