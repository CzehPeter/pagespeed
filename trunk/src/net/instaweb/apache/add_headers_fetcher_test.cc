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

// Author: jefftk@google.com (Jeff Kaufman)
//
// Unit tests for AddHeadersFetcher.
//
#include "net/instaweb/apache/add_headers_fetcher.h"

#include "net/instaweb/http/public/mock_callback.h"
#include "net/instaweb/http/public/reflecting_test_fetcher.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_options_test_base.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/thread_system.h"  // for ThreadSystem

namespace net_instaweb {

namespace {

class AddHeadersFetcherTest : public RewriteOptionsTestBase<RewriteOptions> {
 public:
  AddHeadersFetcherTest()
    : thread_system_(ThreadSystem::CreateThreadSystem()) {
    options_.AddCustomFetchHeader("Custom", "custom-header");
    options_.AddCustomFetchHeader("Extra", "extra-header");
    add_headers_fetcher_.reset(new AddHeadersFetcher(
        &options_, &reflecting_fetcher_));
  }

 protected:
  scoped_ptr<AddHeadersFetcher> add_headers_fetcher_;
  GoogleMessageHandler handler_;
  RewriteOptions options_;
  ReflectingTestFetcher reflecting_fetcher_;
  scoped_ptr<ThreadSystem> thread_system_;
};

TEST_F(AddHeadersFetcherTest, AddsHeaders) {
  ExpectStringAsyncFetch dest(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  add_headers_fetcher_->Fetch("http://example.com/path", &handler_, &dest);
  EXPECT_STREQ("http://example.com/path", dest.buffer());
  EXPECT_STREQ("custom-header", dest.response_headers()->Lookup1("Custom"));
  EXPECT_STREQ("extra-header", dest.response_headers()->Lookup1("Extra"));
}

TEST_F(AddHeadersFetcherTest, ReplacesHeaders) {
  RequestHeaders request_headers;
  ExpectStringAsyncFetch dest(
      true, RequestContext::NewTestRequestContext(thread_system_.get()));
  request_headers.Add("Custom", "original");
  request_headers.Add("AlsoCustom", "original");
  dest.set_request_headers(&request_headers);
  add_headers_fetcher_->Fetch("http://example.com/path", &handler_, &dest);
  EXPECT_STREQ("http://example.com/path", dest.buffer());

  // Overwritten by the add headers fetcher.
  EXPECT_STREQ("custom-header", dest.response_headers()->Lookup1("Custom"));

  // Passed through unmodified.
  EXPECT_STREQ("original", dest.response_headers()->Lookup1("AlsoCustom"));
}

}  // namespace

}  // namespace net_instaweb
