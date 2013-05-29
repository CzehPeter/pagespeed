/*
 * Copyright 2013 Google Inc.
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

// Author: jkarlin@google.com (Josh Karlin)

// Unit-test the distributed pathways through the RewriteContext class.

#include "net/instaweb/rewriter/public/rewrite_context.h"

#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/counting_url_async_fetcher.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/meta_data.h"  // for Code::kOK
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/rewrite_context_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/rewriter/public/test_distributed_fetcher.h"
#include "net/instaweb/rewriter/public/test_rewrite_driver_factory.h"
#include "net/instaweb/util/public/base64_util.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_scheduler.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "pagespeed/kernel/base/timer.h"  // for Timer, etc
#include "pagespeed/kernel/http/content_type.h"

namespace net_instaweb {

namespace {

// A class for testing the distributed paths through the rewrite context. It
// uses the RewriteContextTestBase's "other" RewriteDriver, factory, and options
// as a second task to perform distributed rewrites on. Call
// SetupDistributedTest to configure the test class.
class DistributedRewriteContextTest : public RewriteContextTestBase {
 protected:
  enum HttpRequestType {
    kHeadRequest,
    kGetRequest
  };

  DistributedRewriteContextTest() {
    distributed_rewrite_failures_ = statistics()->GetVariable(
        RewriteContext::kNumDistributedRewriteFailures);
    distributed_rewrite_successes_ = statistics()->GetVariable(
        RewriteContext::kNumDistributedRewriteSuccesses);
    distributed_metadata_failures_ = statistics()->GetVariable(
        RewriteContext::kNumDistributedMetadataFailures);
    fetch_failures_ =
        statistics()->GetVariable(RewriteStats::kNumResourceFetchFailures);
    fetch_successes_ =
        statistics()->GetVariable(RewriteStats::kNumResourceFetchSuccesses);
  }

  // Sets the options to be the same for the two tasks and configures a shared
  // LRU cache between them. Note that when a distributed call is made, the
  // fetcher will call the RewriteContextTestBase's "other" driver directly (see
  // TestDistributedFetcher).
  void SetupDistributedTest() {
    SetupSharedCache();
    options_->DistributeFilter(TrimWhitespaceRewriter::kFilterId);
    options_->set_distributed_rewrite_servers("example.com:80");
    options_->set_distributed_rewrite_key("1234123");
    // Make sure they have the same options so that they generate the same
    // metadata keys.
    other_options()->Merge(*options());
    InitTrimFilters(kRewrittenResource);
    InitResources();
    // Default to empty request headers.
    rewrite_driver()->SetRequestHeaders(request_headers_);
  }

  void InitTwoFilters(OutputResourceKind kind) {
    InitUpperFilter(kind, rewrite_driver());
    InitUpperFilter(kind, other_rewrite_driver());
    options_->DistributeFilter(UpperCaseRewriter::kFilterId);
    SetupDistributedTest();
  }

  // TODO(jkarlin): Instead of CheckDistributedFetch where we pass in the
  // expected values an alternative idea is to create a DistributedFetch
  // function that returns a struct with all of the resulting stats that we can
  // EXPECT_EQ from the calling sites. That might be more readable.

  void CheckDistributedFetch(int distributed_fetch_success_count,
                             int distributed_fetch_failure_count,
                             int local_fetch_required, int rewritten) {
    EXPECT_EQ(distributed_fetch_success_count + distributed_fetch_failure_count,
              counting_distributed_fetcher()->fetch_count());
    EXPECT_EQ(local_fetch_required,
              counting_url_async_fetcher()->fetch_count());
    EXPECT_EQ(
        0, other_factory_->counting_distributed_async_fetcher()->fetch_count());
    EXPECT_EQ(distributed_fetch_success_count,
              distributed_rewrite_successes_->Get());
    EXPECT_EQ(distributed_fetch_failure_count,
              distributed_rewrite_failures_->Get());
    EXPECT_EQ(0, trim_filter_->num_rewrites());
    EXPECT_EQ(rewritten, other_trim_filter_->num_rewrites());
    EXPECT_EQ(0, distributed_metadata_failures_->Get());
  }

  bool FetchValidatedMetadata(StringPiece key, StringPiece input_url,
                              StringPiece correct_url,
                              HttpRequestType request_type) {
    GoogleString output;
    ResponseHeaders response_headers;
    RequestHeaders req_headers;
    bool valid_metadata = false;
    req_headers.Add(HttpAttributes::kXPsaRequestMetadata, key);
    if (request_type == kHeadRequest) {
      req_headers.set_method(RequestHeaders::kHead);
    }
    rewrite_driver()->SetRequestHeaders(req_headers);
    EXPECT_TRUE(
        FetchResourceUrl(input_url, &req_headers, &output, &response_headers));

    // Check if the metadata is valid.
    if (response_headers.Has(HttpAttributes::kXPsaResponseMetadata)) {
      GoogleString encoded_serialized =
          response_headers.Lookup1(HttpAttributes::kXPsaResponseMetadata);
      GoogleString decoded_serialized;
      Mime64Decode(encoded_serialized, &decoded_serialized);
      OutputPartitions partitions;
      EXPECT_TRUE(partitions.ParseFromString(decoded_serialized));
      EXPECT_STREQ(correct_url, partitions.partition(0).url());
      valid_metadata = true;
    }

    // If we did a HEAD request we don't expect any output
    if (request_type == kHeadRequest) {
      EXPECT_STREQ("", output);
    }
    return valid_metadata;
  }

  RequestHeaders request_headers_;  // default request headers
  Variable* fetch_failures_;
  Variable* fetch_successes_;
  Variable* distributed_rewrite_failures_;
  Variable* distributed_rewrite_successes_;
  Variable* distributed_metadata_failures_;
};

}  // namespace

// Copy of the RewriteContextTest.TrimRewrittenOptimizable test modified for
// distributed rewrites.
TEST_F(DistributedRewriteContextTest, TrimRewrittenOptimizable) {
  SetupDistributedTest();

  // Ingress task: Misses on metadata and distributes.
  // Rewrite task: Misses on metadata, misses on http data, writes original
  // resources, optimized resource, and metadata.
  ValidateExpected(
      "trimmable", CssLinkHref("a.css"),
      CssLinkHref(Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
                         "a.css", "css")));

  CheckDistributedFetch(1,   // successful distributed fetches
                        0,   // unsuccessful distributed fetches
                        0,   // number of ingress fetches
                        1);  // number of rewrites
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());
  ClearStats();

  // The second time we request this URL, we should find no additional cache
  // inserts or fetches. The rewrite should complete using a single cache hit
  // for the metadata. No cache misses will occur.
  ValidateExpected(
      "trimmable", CssLinkHref("a.css"),
      CssLinkHref(Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
                         "a.css", "css")));
  CheckDistributedFetch(0,   // successful distributed fetches
                        0,   // unsuccessful distributed fetches
                        0,   // number of ingress fetches
                        0);  // number of rewrites
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
}

// Copy of the RewriteContextTest.TrimRewrittenNonOptimizable test modified for
// distributed rewrites.
TEST_F(DistributedRewriteContextTest, TrimRewrittenNonOptimizable) {
  SetupDistributedTest();

  // In this case, the resource is not optimizable.  The cache pattern is
  // exactly the same as when the resource was on-the-fly and optimizable.
  // We'll cache the successfully fetched resource, and the OutputPartitions
  // which indicates the unsuccessful optimization.
  ValidateNoChanges("no_trimmable", CssLinkHref("b.css"));
  CheckDistributedFetch(1,   // successful distributed fetches
                        0,   // unsuccessful distributed fetches
                        0,   // number of ingress fetches
                        1);  // number of rewrites
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(2, lru_cache()->num_inserts());
  ClearStats();

  // We should have cached the failed rewrite, no misses, fetches, or inserts.
  ValidateNoChanges("no_trimmable", CssLinkHref("b.css"));
  CheckDistributedFetch(0,                // successful distributed fetches
                        0,                // unsuccessful distributed fetches
                        0,                // number of ingress fetches
                        0);               // number of rewrites
  EXPECT_EQ(1, lru_cache()->num_hits());  // partition
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
}

// Copy of the RewriteContextTest.TrimRepeatedOptimizable test modified for
// distributed rewrites.
TEST_F(DistributedRewriteContextTest, TrimRepeatedOptimizable) {
  // Make sure two instances of the same link are handled properly,
  // when optimization succeeds.
  SetupDistributedTest();
  ValidateExpected(
      "trimmable2", StrCat(CssLinkHref("a.css"), CssLinkHref("a.css")),
      StrCat(CssLinkHref(Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId,
                                "0", "a.css", "css")),
             CssLinkHref(Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId,
                                "0", "a.css", "css"))));
  CheckDistributedFetch(1,   // successful distributed fetches
                        0,   // unsuccessful distributed fetches
                        0,   // number of ingress fetches
                        1);  // number of rewrites
}

TEST_F(DistributedRewriteContextTest, TwoFilters) {
  InitTwoFilters(kOnTheFlyResource);

  ValidateExpected(
      "two_filters", CssLinkHref("a.css"),
      CssLinkHref(
          Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
                 Encode("", UpperCaseRewriter::kFilterId, "0", "a.css", "css"),
                 "css")));
  EXPECT_EQ(1, distributed_rewrite_successes_->Get());
  EXPECT_EQ(0, distributed_rewrite_failures_->Get());
  EXPECT_EQ(1, trim_filter_->num_rewrites());  // not distributed
  EXPECT_EQ(0, other_trim_filter_->num_rewrites());
}

// Same as TwoFilters but this time write to HTTP cache.
TEST_F(DistributedRewriteContextTest, TwoFiltersRewritten) {
  InitTwoFilters(kRewrittenResource);

  ValidateExpected(
      "two_filters", CssLinkHref("a.css"),
      CssLinkHref(
          Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
                 Encode("", UpperCaseRewriter::kFilterId, "0", "a.css", "css"),
                 "css")));
  EXPECT_EQ(1, distributed_rewrite_successes_->Get());
  EXPECT_EQ(0, distributed_rewrite_failures_->Get());
  EXPECT_EQ(1, trim_filter_->num_rewrites());  // not distributed
  EXPECT_EQ(0, other_trim_filter_->num_rewrites());
  // num_hits = 0 proves that we didn't use the cache to pipe the output of the
  // first filter in the chain to the second, instead we used the slot like we
  // were supposed to.
  EXPECT_EQ(0, lru_cache()->num_hits());
  // Miss uc (UpperCaseRewriter) metadata on ingress and distributed task.
  // Miss http input on distributed task.
  // Miss tw metadata on ingress task (but don't distribute).
  EXPECT_EQ(4, lru_cache()->num_misses());
  // uc (UpperCaseRewriter) inserts metadata, original http content, and
  // optimized http content.
  // tw filter inserts metadata and optimized http content.
  EXPECT_EQ(5, lru_cache()->num_inserts());
}

TEST_F(DistributedRewriteContextTest, TwoFiltersDelayedFetches) {
  other_factory_->SetupWaitFetcher();
  InitTwoFilters(kOnTheFlyResource);
  test_distributed_fetcher_.set_blocking_fetch(false);

  ValidateNoChanges("trimmable1", CssLinkHref("a.css"));
  OtherCallFetcherCallbacks();
  ValidateExpected(
      "delayed_fetches", CssLinkHref("a.css"),
      CssLinkHref(
          Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
                 Encode("", UpperCaseRewriter::kFilterId, "0", "a.css", "css"),
                 "css")));
  EXPECT_EQ(1, distributed_rewrite_successes_->Get());
  EXPECT_EQ(0, distributed_rewrite_failures_->Get());
  EXPECT_EQ(1, trim_filter_->num_rewrites());  // not distributed
  EXPECT_EQ(0, other_trim_filter_->num_rewrites());
}

TEST_F(DistributedRewriteContextTest, RepeatedTwoFilters) {
  // Make sure if we have repeated URLs and chaining, it still works right. Note
  // that both trim and upper are distributed, but when chained only the first
  // should distribute.
  InitTwoFilters(kRewrittenResource);

  ValidateExpected(
      "two_filters2", StrCat(CssLinkHref("a.css"), CssLinkHref("a.css")),
      StrCat(CssLinkHref(Encode(
                 kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
                 Encode("", UpperCaseRewriter::kFilterId, "0", "a.css", "css"),
                 "css")),
             CssLinkHref(Encode(
                 kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
                 Encode("", UpperCaseRewriter::kFilterId, "0", "a.css", "css"),
                 "css"))));
  EXPECT_EQ(1, distributed_rewrite_successes_->Get());
  EXPECT_EQ(0, distributed_rewrite_failures_->Get());
  EXPECT_EQ(1, trim_filter_->num_rewrites());  // not distributed
  EXPECT_EQ(0, other_trim_filter_->num_rewrites());
}

// Simulate distributed fetch failure and ensure that we fall back to the
// original.
TEST_F(DistributedRewriteContextTest, IngressDistributedRewriteFailFallback) {
  SetupDistributedTest();
  // Break the response after the headers have written but before data is
  // complete.
  test_distributed_fetcher()->set_fail_after_headers(true);
  ValidateNoChanges("trimmable", CssLinkHref("a.css"));

  // Ingress: Misses metadata, and does not optimize after unsuccessful
  // distributed fetch.
  // Distributed task: Misses metadata and original resource. Inserts metadata,
  // original, and optimized.  Returned stream is broken.
  CheckDistributedFetch(0,                // successful distributed fetches
                        1,                // unsuccessful distributed fetches
                        0,                // number of ingress fetches
                        1);               // number of rewrites
  EXPECT_EQ(0, lru_cache()->num_hits());  // partition
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());

  ClearStats();

  // Try again, this time we should have the result in shared cache.
  ValidateExpected(
      "trimmable", CssLinkHref("a.css"),
      CssLinkHref(Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
                         "a.css", "css")));

  // Ingress: Misses metadata, and does not optimize after unsuccessful
  // distributed fetch.
  // Distributed task: Misses metadata and original resource. Inserts metadata,
  // original, and optimized.  Returned stream is broken.
  CheckDistributedFetch(0,                // successful distributed fetches
                        0,                // unsuccessful distributed fetches
                        0,                // number of ingress fetches
                        0);               // number of rewrites
  EXPECT_EQ(1, lru_cache()->num_hits());  // partition
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
}

// If the distributed fetcher returns a 404 then that's what needs to be
// returned.
TEST_F(DistributedRewriteContextTest, DistributedRewriteNotFound) {
  SetupDistributedTest();
  static const char fourofour[] = "fourofour.css";
  GoogleString orig_url = StrCat(kTestDomain, fourofour);
  SetFetchResponse404(orig_url);
  ValidateNoChanges("trimmable", CssLinkHref(fourofour));
  // Ingress task misses on metadata, gets unsuccessful fetch and returns
  // original unoptimized reference.
  // Distributed task misses on metadata and original resource fetch, fails its
  // fetch (404) and writes that back to metadata and original resource,
  // returning failure.
  CheckDistributedFetch(0,                // successful distributed fetches
                        1,                // unsuccessful distributed fetches
                        0,                // number of ingress fetches
                        0);               // number of rewrites
  EXPECT_EQ(0, lru_cache()->num_hits());  // partition
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(2, lru_cache()->num_inserts());

  ClearStats();
  // Try again, this time it should be a quick metadata lookup at the ingress
  // task.
  ValidateNoChanges("trimmable", CssLinkHref(fourofour));
  CheckDistributedFetch(0,                // successful distributed fetches
                        0,                // unsuccessful distributed fetches
                        0,                // number of ingress fetches
                        0);               // number of rewrites
  EXPECT_EQ(1, lru_cache()->num_hits());  // partition
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
}

// Similar to RewriteContextTest.TrimDelayed test but modified for distributed
// rewrites.
TEST_F(DistributedRewriteContextTest, TrimDelayed) {
  //  In this run, we will delay the URL fetcher's callback so that the initial
  //  Rewrite will not take place until after the HTML has been flushed.
  SetupDistributedTest();
  other_factory_->SetupWaitFetcher();
  test_distributed_fetcher_.set_blocking_fetch(false);

  // First time distribute but the external fetch doesn't finish by ingress task
  // (or deadline task's for that matter) deadline.
  // Ingress: metadata miss, distributed rewrite which times out, so don't
  // optimize.
  // Distributed: metadata miss and original http miss. Left fetching the http
  // and isn't done fetching before the time out on the ingress task.
  ValidateNoChanges("trimmable", CssLinkHref("a.css"));
  CheckDistributedFetch(0,                // successful distributed fetches
                        0,                // unsuccessful distributed fetches
                        0,                // number of ingress fetches
                        0);               // number of rewrites
  EXPECT_EQ(0, lru_cache()->num_hits());  // partition
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());

  // Let the distributed rewriter finish up its fetch and rewrite.
  OtherCallFetcherCallbacks();
  // Let the ingress task finish up as well.
  rewrite_driver_->WaitForShutDown();
  factory_->mock_scheduler()->AwaitQuiescence();

  // Now the rewrite is done, make sure the stats look right.
  // Ingress: same as before
  // Distributed: puts the original and optimized resource and metadata in
  // cache.
  CheckDistributedFetch(1,                // successful distributed fetches
                        0,                // unsuccessful distributed fetches
                        0,                // number of ingress fetches
                        1);               // number of rewrites
  EXPECT_EQ(0, lru_cache()->num_hits());  // partition
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());

  // Second time the ingress metadata hits and that's all that's necessary.
  ClearStats();
  ValidateExpected(
      "trimmable", CssLinkHref("a.css"),
      CssLinkHref(Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
                         "a.css", "css")));

  CheckDistributedFetch(0,                // successful distributed fetches
                        0,                // unsuccessful distributed fetches
                        0,                // number of ingress fetches
                        0);               // number of rewrites
  EXPECT_EQ(1, lru_cache()->num_hits());  // partition
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
}

// Copy of the RewriteContextTest.TrimRepeatedNonOptimizable test modified for
// distributed rewrites.
TEST_F(DistributedRewriteContextTest, TrimRepeatedNonOptimizable) {
  // Make sure two instances of the same link are handled properly when
  // optimization fails.

  SetupDistributedTest();
  ValidateNoChanges("notrimmable2",
                    StrCat(CssLinkHref("b.css"), CssLinkHref("b.css")));
  // Ingress task misses metadata and distributes rewrite.
  // Distributed task misses metadata and original resource, inserts
  // meteadata, optimized, and unoptimized resource.
  CheckDistributedFetch(1,                // successful distributed fetches
                        0,                // unsuccessful distributed fetches
                        0,                // number of ingress fetches
                        1);               // number of rewrites
  EXPECT_EQ(0, lru_cache()->num_hits());  // partition
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(2, lru_cache()->num_inserts());
}

// Distribute a .pagespeed. reconstruction.
TEST_F(DistributedRewriteContextTest, IngressDistributedRewriteFetch) {
  SetupDistributedTest();
  GoogleString encoded_url = Encode(
      kTestDomain, TrimWhitespaceRewriter::kFilterId, "0", "a.css", "css");

  // Fetch the .pagespeed. resource and ensure that the rewrite was distributed.
  GoogleString content;
  ResponseHeaders response_headers;
  RequestHeaders request_headers;
  EXPECT_TRUE(FetchResourceUrl(encoded_url, &request_headers, &content,
                               &response_headers));
  // Content should be optimized.
  EXPECT_EQ("a", content);

  // Make sure the TTL is long and the result is cacheable.
  EXPECT_EQ(Timer::kYearMs, response_headers.cache_ttl_ms());
  EXPECT_TRUE(response_headers.IsProxyCacheable());
  EXPECT_TRUE(response_headers.IsBrowserCacheable());

  CheckDistributedFetch(1,   // successful distributed fetches
                        0,   // unsuccessful distributed fetches
                        0,   // number of ingress fetches
                        1);  // number of rewrites

  // Ingress task misses on two HTTP lookups (check twice for rewritten
  // resource) and one metadata lookup.
  // Rewrite task misses on three HTTP lookups (twice for rewritten resource
  // plus once for original resource) and one metdata lookup. Then inserts
  // original resource, optimized resource, and metadata.
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(7, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(5, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, http_cache()->cache_inserts()->Get());

  // On the second .pagespeed. request the optimized resource should be in the
  // shared cache.
  ClearStats();
  EXPECT_TRUE(FetchResourceUrl(encoded_url, &request_headers, &content,
                               &response_headers));

  // Content should be optimized.
  EXPECT_EQ("a", content);

  // The distributed fetcher should not have run.
  EXPECT_EQ(0, counting_distributed_fetcher()->fetch_count());

  // Ingress task hits on one HTTP lookup and returns it.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
}

// If the distributed fetcher returns a 404 then that's what should be
// returned.
TEST_F(DistributedRewriteContextTest, IngressDistributedRewriteNotFoundFetch) {
  SetupDistributedTest();
  GoogleString orig_url = StrCat(kTestDomain, "fourofour.css");
  GoogleString encoded_url =
      Encode(kTestDomain, TrimWhitespaceRewriter::kFilterId, "0",
             "fourofour.css", "css");
  SetFetchResponse404(orig_url);

  // Fetch the .pagespeed. resource and ensure that the rewrite gets
  // distributed.
  GoogleString content;
  ResponseHeaders response_headers;
  RequestHeaders request_headers;

  EXPECT_FALSE(FetchResourceUrl(encoded_url, &request_headers, &content,
                                &response_headers));
  // Should be a 404 response.
  EXPECT_EQ(HttpStatus::kNotFound, response_headers.status_code());

  // The distributed fetcher should have run once on the ingress task and the
  // url fetcher should have run once on the rewrite task.  The result goes to
  // shared cache.
  CheckDistributedFetch(0,   // successful distributed fetches
                        1,   // unsuccessful distributed fetches
                        0,   // number of ingress fetches
                        0);  // number of rewrites

  // Ingress task misses on two HTTP lookups (check twice for rewritten
  // resource) and one metadata lookup.  Then hits on the 404'd resource.
  // Rewrite task misses on three HTTP lookups (twice for rewritten resource
  // plus once for original resource) and one metdata lookup. Then inserts
  // 404'd original resource and metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(7, lru_cache()->num_misses());
  EXPECT_EQ(2, lru_cache()->num_inserts());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(6, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());

  // Fetching again causes another reconstruction and therefore another
  // distributed rewrite, even though we hit the 404 in cache.
  //
  // ingress task: 2 .pagespeed, misses, 1 metadata hit, 1 http hit, then
  // distribute because 404, it fails (because 404) so fetch locally and hit.
  // Return.
  //
  // rewrite task: 2 .pagespeed. misses, 1 metadata hit, 1 http hit, then fetch
  // again because 404, fetch locally and hit. Return.
  ClearStats();
  EXPECT_FALSE(FetchResourceUrl(encoded_url, &request_headers, &content,
                                &response_headers));
  CheckDistributedFetch(0,   // successful distributed fetches
                        1,   // unsuccessful distributed fetches
                        0,   // number of ingress fetches
                        0);  // number of rewrites

  EXPECT_EQ(6, lru_cache()->num_hits());
  EXPECT_EQ(4, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
}

// Simulate distributed fetch failure and ensure that we fall back to the
// original.
TEST_F(DistributedRewriteContextTest,
       IngressDistributedRewriteFailFallbackFetch) {
  SetupDistributedTest();
  test_distributed_fetcher()->set_fail_after_headers(true);

  // Mock the optimized .pagespeed. response from the rewrite task.
  GoogleString encoded_url = Encode(
      kTestDomain, TrimWhitespaceRewriter::kFilterId, "0", "a.css", "css");

  GoogleString content;
  ResponseHeaders response_headers;
  RequestHeaders request_headers;
  EXPECT_TRUE(FetchResourceUrl(encoded_url, &request_headers, &content,
                               &response_headers));

  EXPECT_STREQ(" a ", content);

  // Ingress task distributes, which fails, but pick up original resource from
  // shared cache.
  CheckDistributedFetch(0,   // successful distributed fetches
                        1,   // unsuccessful distributed fetches
                        0,   // number of ingress fetches
                        1);  // number of rewrites

  // Ingress task: Misses http cache twice, then metadata. Distributed rewrite
  // fails, so fetches original (a hit because of shared cache), and returns.
  // Distributed task: Misses http cache twice, then metadata. Fetches original
  // (misses in process), writes it, optimizes, writes optimized, and writes
  // metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(7, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(5, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, http_cache()->cache_inserts()->Get());
}

TEST_F(DistributedRewriteContextTest, ReturnMetadataOnRequest) {
  // Sends a fetch that asks for metadata in the response headers and checks
  // that it's in the response.

  // We need to make distributed_rewrite_servers != "" and set a
  // distributed_rewrite_key in order to return metadata.
  options()->set_distributed_rewrite_servers("example.com");
  static const char kDistributedKey[] = "1234123";
  options()->set_distributed_rewrite_key(kDistributedKey);
  InitTrimFilters(kRewrittenResource);
  InitResources();

  GoogleString encoded_url = Encode(
      kTestDomain, TrimWhitespaceRewriter::kFilterId, "0", "a.css", "css");
  GoogleString bad_encoded_url = Encode(
      kTestDomain, TrimWhitespaceRewriter::kFilterId, "1", "a.css", "css");

  // Note that the .pagespeed. path with metadata request headers do not
  // check the http cache up front.  If they did that and hit they would
  // not have metadata to return.  Therefore the tests below have fewer
  // cache misses than you might have expected.

  // The first .pagespeed. request.  It should hit the reconstruction path.
  // We'll miss on the metadata and the input resource.  Then fetch once
  // and put optimized resource, input resource, and metadata in cache.
  EXPECT_TRUE(FetchValidatedMetadata(kDistributedKey, encoded_url, encoded_url,
                                     kGetRequest));
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(3, lru_cache()->num_inserts());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());

  // We should get metadata even though the optimized output is cached.
  ClearStats();
  EXPECT_TRUE(FetchValidatedMetadata(kDistributedKey, encoded_url, encoded_url,
                                     kGetRequest));
  EXPECT_EQ(2, lru_cache()->num_hits());  // 1 metadata and 1 http
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());

  // If we use the wrong encoding the metadata + subsequent HTTP cache will hit,
  // following the fallback path.
  ClearStats();
  EXPECT_TRUE(FetchValidatedMetadata(kDistributedKey, bad_encoded_url,
                                     encoded_url, kGetRequest));
  // Expect the bad url to miss twice (RewriteDriver::CacheCallback tries
  // twice). We should then hit the metadata and good http url.
  EXPECT_EQ(2, lru_cache()->num_hits());  // 1 metadata and 1 http
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());

  // If we clear the caches and use the wrong URL it should use the
  // reconstruction path and return the right URL and the metadata.
  ClearStats();
  lru_cache()->Clear();
  http_cache()->Delete(encoded_url);
  EXPECT_TRUE(FetchValidatedMetadata(kDistributedKey, bad_encoded_url,
                                     encoded_url, kGetRequest));
  // We should fetch once and insert the input, optimized, and metadata into
  // cache.
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(2, lru_cache()->num_misses());  // 1 metadata and 1 http input
  EXPECT_EQ(3, lru_cache()->num_inserts());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
}

TEST_F(DistributedRewriteContextTest, HeadMetadata) {
  // Verify that a HEAD request that asks for metadata returns the metadata
  // but not the content.  We don't check cache hit/miss numbers because that
  // would be redundant with RewriteContextTest.ReturnMetadataOnRequest.

  // We need to make distributed_rewrite_servers != "" in order to return
  // metedata.
  options()->set_distributed_rewrite_servers("example.com");
  static const char kDistributedKey[] = "1234123";
  options()->set_distributed_rewrite_key(kDistributedKey);
  InitTrimFilters(kRewrittenResource);
  InitResources();

  GoogleString encoded_url = Encode(
      kTestDomain, TrimWhitespaceRewriter::kFilterId, "0", "a.css", "css");
  GoogleString bad_encoded_url = Encode(
      kTestDomain, TrimWhitespaceRewriter::kFilterId, "1", "a.css", "css");

  // Reconstruction path.
  EXPECT_TRUE(FetchValidatedMetadata(kDistributedKey, encoded_url, encoded_url,
                                     kHeadRequest));

  // Second fetch, verify that we skip the initial http cache check and do
  // return metadata.
  EXPECT_TRUE(FetchValidatedMetadata(kDistributedKey, encoded_url, encoded_url,
                                     kHeadRequest));

  // Bad .pagespeed. hash but still gets resolved.
  EXPECT_TRUE(FetchValidatedMetadata(kDistributedKey, bad_encoded_url,
                                     encoded_url, kHeadRequest));

  // Bad .pagespeed. hash and empty cache but should still reconstruct properly.
  lru_cache()->Clear();
  http_cache()->Delete(encoded_url);
  EXPECT_TRUE(FetchValidatedMetadata(kDistributedKey, bad_encoded_url,
                                     encoded_url, kHeadRequest));
}

TEST_F(DistributedRewriteContextTest, NoMetadataWithoutRewriteOption) {
  // Ensure that we don't return metadata if we're not configured
  // to run with distributed rewrites.
  static const char kDistributedKey[] = "1234123";
  options()->set_distributed_rewrite_key(kDistributedKey);
  InitTrimFilters(kRewrittenResource);
  InitResources();

  GoogleString encoded_url = Encode(
      kTestDomain, TrimWhitespaceRewriter::kFilterId, "0", "a.css", "css");

  // We didn't set rewrite tasks in options, so we shouldn't get any metadata.
  EXPECT_FALSE(FetchValidatedMetadata(kDistributedKey, encoded_url, encoded_url,
                                      kGetRequest));
}

TEST_F(DistributedRewriteContextTest, NoMetadataWithoutSettingKey) {
  // Ensure that we don't return metadata if we're not configured
  // to run with distributed rewrites.
  options()->set_distributed_rewrite_servers("example.com");
  static const char kDistributedKey[] = "1234123";
  // Neglect to set the distributed rewrite key in options.
  InitTrimFilters(kRewrittenResource);
  InitResources();

  GoogleString encoded_url = Encode(
      kTestDomain, TrimWhitespaceRewriter::kFilterId, "0", "a.css", "css");

  // We didn't set a distributed rewrite key in options, so we shouldn't get any
  // metadata.
  EXPECT_FALSE(
      FetchValidatedMetadata("", encoded_url, encoded_url, kGetRequest));
  EXPECT_FALSE(FetchValidatedMetadata(kDistributedKey, encoded_url, encoded_url,
                                      kGetRequest));
}

TEST_F(DistributedRewriteContextTest, NoMetadataWithBadKeys) {
  // Ensure that we don't return metadata if we're not configured
  // to run with distributed rewrites.
  options()->set_distributed_rewrite_servers("example.com");
  static const char kDistributedKey[] = "a1234123";
  options()->set_distributed_rewrite_key(kDistributedKey);
  InitTrimFilters(kRewrittenResource);
  InitResources();

  GoogleString encoded_url = Encode(
      kTestDomain, TrimWhitespaceRewriter::kFilterId, "0", "a.css", "css");

  EXPECT_FALSE(
      FetchValidatedMetadata("", encoded_url, encoded_url, kGetRequest));
  // Changing case doesn't work.
  EXPECT_FALSE(FetchValidatedMetadata("A1234123", encoded_url, encoded_url,
                                      kGetRequest));
  // Sanity check that it does work with the correct key.
  EXPECT_TRUE(FetchValidatedMetadata("a1234123", encoded_url, encoded_url,
                                     kGetRequest));
}

// If we try to distribute an HTML rewrite for a resource whose URL is too long
// we should handle it gracefully.
TEST_F(DistributedRewriteContextTest, GracefullyHandleURLTooLong) {
  SetupDistributedTest();

  // Create a long URL that could feasibly exist but cannot be extended into a
  // .pagespeed. resource.
  GoogleString long_url = kTestDomain;
  for (int i = long_url.size(),
           max_size = options()->max_url_segment_size() - 4;
       i < max_size; ++i) {
    long_url += "a";
  }
  long_url = StrCat(long_url, ".css");

  SetResponseWithDefaultHeaders(long_url, kContentTypeCss, " hello ", 60);

  ValidateNoChanges("long_url", CssLinkHref(long_url));
  EXPECT_EQ(0, counting_distributed_fetcher()->fetch_count());
  EXPECT_EQ(0, counting_url_async_fetcher()->fetch_count());
  EXPECT_EQ(
      0, other_factory_->counting_distributed_async_fetcher()->fetch_count());
  EXPECT_EQ(0, distributed_rewrite_successes_->Get());
  // Even though we didn't have a distributed fetch, we do have a distributed
  // rewrite failure since when prepping for the fetch we failed because the URL
  // was too long.
  EXPECT_EQ(1, distributed_rewrite_failures_->Get());
  EXPECT_EQ(0, trim_filter_->num_rewrites());
  EXPECT_EQ(0, other_trim_filter_->num_rewrites());
  EXPECT_EQ(0, distributed_metadata_failures_->Get());
}

}  // namespace net_instaweb
