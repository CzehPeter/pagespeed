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
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

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
  }

  void CheckDistributedFetch(int distributed_fetch_success_count,
                             bool local_fetch_required,
                             bool distributed_fetch_required, bool rewritten) {
    EXPECT_EQ(1, counting_distributed_fetcher()->fetch_count());
    EXPECT_EQ(local_fetch_required,
              counting_url_async_fetcher()->fetch_count());
    EXPECT_EQ(
        0, other_factory_->counting_distributed_async_fetcher()->fetch_count());
    EXPECT_EQ(distributed_fetch_required,
              other_factory_->counting_url_async_fetcher()->fetch_count());
    EXPECT_EQ(distributed_fetch_success_count,
              distributed_rewrite_successes_->Get());
    EXPECT_EQ(!distributed_fetch_success_count,
              distributed_rewrite_failures_->Get());
    EXPECT_EQ(0, trim_filter_->num_rewrites());
    EXPECT_EQ(rewritten, other_trim_filter_->num_rewrites());
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
    rewrite_driver()->set_request_headers(&req_headers);
    EXPECT_TRUE(FetchResourceUrl(input_url, &req_headers, &output,
                                 &response_headers));

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


  Variable* fetch_failures_;
  Variable* fetch_successes_;
  Variable* distributed_rewrite_failures_;
  Variable* distributed_rewrite_successes_;
};

}  // namespace

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

  CheckDistributedFetch(1,      // distributed fetch success count
                        false,  // ingress fetch required
                        true,   // distributed fetch required
                        true);  // rewrite

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
  CheckDistributedFetch(0,       // distributed fetch success count
                        false,   // ingress fetch required
                        true,    // distributed fetch required
                        false);  // rewrite

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
  CheckDistributedFetch(0,       // distributed fetch success count
                        false,   // ingress fetch required
                        false,   // distributed fetch required
                        false);  // rewrite
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
  CheckDistributedFetch(0,      // distributed fetch success count
                        false,  // ingress fetch required
                        true,   // distributed fetch required
                        true);  // rewrite
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
  EXPECT_EQ(2, lru_cache()->num_hits());     // 1 metadata and 1 http
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

}  // namespace net_instaweb
