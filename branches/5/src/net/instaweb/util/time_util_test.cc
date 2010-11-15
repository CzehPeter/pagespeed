/**
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

#include "net/instaweb/util/public/time_util.h"
#include "net/instaweb/util/public/gtest.h"

namespace {

const char kApr5[] = "Mon Apr  5 18:49:46 2010 GMT";

// The time-conversion functions are only accurate to the second,
// and we will not be able to test for identity transforms if we
// are not using a mulitple of 1000.
const int64 kTimestampMs = 718981 * 1000;

}  // namespace

namespace net_instaweb {

class TimeUtilTest : public testing::Test {
 protected:
  std::string GetTimeString(int64 time_ms) {
    std::string out;
    ConvertTimeToString(time_ms, &out);
    return out;
  }

  int64 GetTimeValue(const std::string& time_str) {
    int64 val;
    CHECK(ConvertStringToTime(time_str, &val)) << "Time conversion failed";
    return val;
  }

  std::string out_;
};

TEST_F(TimeUtilTest, Test1970) {
  EXPECT_EQ("Thu Jan  1 00:00:00 1970 GMT", GetTimeString(0));
  EXPECT_EQ(1270493386000LL, GetTimeValue(kApr5));
}

TEST_F(TimeUtilTest, TestIdentity) {
  EXPECT_EQ(kTimestampMs, GetTimeValue(GetTimeString(kTimestampMs)));
  EXPECT_EQ(kApr5, GetTimeString(GetTimeValue(kApr5)));
}

}  // namespace net_instaweb
