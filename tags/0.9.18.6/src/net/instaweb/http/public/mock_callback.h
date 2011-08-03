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

// Author: sligocki@google.com (Shawn Ligocki)

// Callbacks used of testing.

#ifndef NET_INSTAWEB_HTTP_PUBLIC_MOCK_CALLBACK_H_
#define NET_INSTAWEB_HTTP_PUBLIC_MOCK_CALLBACK_H_

#include "net/instaweb/http/public/url_async_fetcher.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/gtest.h"

namespace net_instaweb {

// Callback that can be used for testing resource fetches with accessors to
// find out if it has been called and whether result was success.
// MockCallback does not delete itself and expects to be allocated on stack
// so that it can be accessed before and after Done() is called.
class MockCallback : public UrlAsyncFetcher::Callback {
 public:
  MockCallback() : success_(false), done_(false) {}

  virtual void Done(bool success) {
    success_ = success;
    done_ = true;
  }

  bool success() const { return success_; }
  bool done() const { return done_; }

 private:
  bool success_;
  bool done_;

  DISALLOW_COPY_AND_ASSIGN(MockCallback);
};

// Callback that can be used for testing resource fetches which makes sure
// that Done() is called exactly once and with the expected success value.
// Can be used multiple times by calling Reset in between.
class ExpectCallback : public UrlAsyncFetcher::Callback {
 public:
  explicit ExpectCallback(bool expect_success)
      : done_(false),
        expect_success_(expect_success) {}
  virtual ~ExpectCallback() {
    EXPECT_TRUE(done_);
  }

  virtual void Done(bool success) {
    EXPECT_FALSE(done_) << "Already Done; perhaps you reused without Reset()";
    done_ = true;
    EXPECT_EQ(expect_success_, success);
  }

  void Reset() {
    done_ = false;
  }

  bool done() const { return done_; }

 private:
  bool done_;
  bool expect_success_;

  DISALLOW_COPY_AND_ASSIGN(ExpectCallback);
};


}  // namespace net_instaweb

#endif  // NET_INSTAWEB_HTTP_PUBLIC_MOCK_CALLBACK_H_
