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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/rewriter/public/rewrite_stats.h"

#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/stl_util.h"
#include "net/instaweb/util/public/waveform.h"

namespace net_instaweb {

namespace {

// resource_url_domain_rejections counts the number of urls on a page that we
// could have rewritten, except that they lay in a domain that did not
// permit resource rewriting relative to the current page.
const char kResourceUrlDomainRejections[] = "resource_url_domain_rejections";
const char kCachedOutputMissedDeadline[] =
    "rewrite_cached_output_missed_deadline";
const char kCachedOutputHits[] = "rewrite_cached_output_hits";
const char kCachedOutputMisses[] = "rewrite_cached_output_misses";
const char kInstawebResource404Count[] = "resource_404_count";
const char kInstawebSlurp404Count[] = "slurp_404_count";
const char kResourceFetchesCached[] = "resource_fetches_cached";
const char kResourceFetchConstructSuccesses[] =
    "resource_fetch_construct_successes";
const char kResourceFetchConstructFailures[] =
    "resource_fetch_construct_failures";
const char kNumFlushes[] = "num_flushes";
const char kFallbackResponsesServed[] = "num_fallback_responses_served";
const char kNumConditionalRefreshes[] = "num_conditional_refreshes";

// Variables for the beacon to increment.  These are currently handled in
// mod_pagespeed_handler on apache.  The average load time in milliseconds is
// total_page_load_ms / page_load_count.  Note that these are not updated
// together atomically, so you might get a slightly bogus value.
//
// We also keep a histogram, kBeaconTimingsMsHistogram of these.
const char kTotalPageLoadMs[] = "total_page_load_ms";
const char kPageLoadCount[] = "page_load_count";

const int kNumWaveformSamples = 200;

// Histogram names.
const char kBeaconTimingsMsHistogram[] = "Beacon Reported Load Time (ms)";
const char kFetchLatencyHistogram[] = "Pagespeed Resource Latency Histogram";
const char kRewriteLatencyHistogram[] = "Rewrite Latency Histogram";
const char kBackendLatencyHistogram[] =
    "Backend Fetch First Byte Latency Histogram";

// TimedVariable names.
const char kTotalFetchCount[] = "total_fetch_count";
const char kTotalRewriteCount[] = "total_rewrite_count";
const char kRewritesExecuted[] = "num_rewrites_executed";
const char kRewritesDropped[] = "num_rewrites_dropped";

}  // namespace

// In Apache, this is called in the root process to establish shared memory
// boundaries prior to the primary initialization of RewriteDriverFactories.
//
// Note that there are other statistics owned by filters and subsystems,
// that must get the some treatment.
void RewriteStats::InitStats(Statistics* statistics) {
  statistics->AddVariable(kResourceUrlDomainRejections);
  statistics->AddVariable(kCachedOutputMissedDeadline);
  statistics->AddVariable(kCachedOutputHits);
  statistics->AddVariable(kCachedOutputMisses);
  statistics->AddVariable(kInstawebResource404Count);
  statistics->AddVariable(kInstawebSlurp404Count);
  statistics->AddVariable(kTotalPageLoadMs);
  statistics->AddVariable(kPageLoadCount);
  statistics->AddVariable(kResourceFetchesCached);
  statistics->AddVariable(kResourceFetchConstructSuccesses);
  statistics->AddVariable(kResourceFetchConstructFailures);
  statistics->AddVariable(kNumFlushes);
  statistics->AddHistogram(kBeaconTimingsMsHistogram);
  statistics->AddHistogram(kFetchLatencyHistogram);
  statistics->AddHistogram(kRewriteLatencyHistogram);
  statistics->AddHistogram(kBackendLatencyHistogram);
  statistics->AddVariable(kFallbackResponsesServed);
  statistics->AddVariable(kNumConditionalRefreshes);
  statistics->AddTimedVariable(kTotalFetchCount,
                               ServerContext::kStatisticsGroup);
  statistics->AddTimedVariable(kTotalRewriteCount,
                               ServerContext::kStatisticsGroup);
  statistics->AddTimedVariable(kRewritesExecuted,
                               ServerContext::kStatisticsGroup);
  statistics->AddTimedVariable(kRewritesDropped,
                               ServerContext::kStatisticsGroup);
}

// This is called when a RewriteDriverFactory is created, and adds
// common statistics to a public structure.
//
// Note that there are other statistics owned by filters and subsystems,
// that must get the some treatment.
RewriteStats::RewriteStats(Statistics* stats,
                           ThreadSystem* thread_system,
                           Timer* timer)
    : cached_output_hits_(
        stats->GetVariable(kCachedOutputHits)),
      cached_output_missed_deadline_(
          stats->GetVariable(kCachedOutputMissedDeadline)),
      cached_output_misses_(
          stats->GetVariable(kCachedOutputMisses)),
      cached_resource_fetches_(
          stats->GetVariable(kResourceFetchesCached)),
      failed_filter_resource_fetches_(
          stats->GetVariable(kResourceFetchConstructFailures)),
      num_flushes_(
          stats->GetVariable(kNumFlushes)),
      page_load_count_(
          stats->GetVariable(kPageLoadCount)),
      resource_404_count_(
          stats->GetVariable(kInstawebResource404Count)),
      resource_url_domain_rejections_(
          stats->GetVariable(kResourceUrlDomainRejections)),
      slurp_404_count_(
          stats->GetVariable(kInstawebSlurp404Count)),
      succeeded_filter_resource_fetches_(
          stats->GetVariable(kResourceFetchConstructSuccesses)),
      total_page_load_ms_(
          stats->GetVariable(kTotalPageLoadMs)),
      fallback_responses_served_(
          stats->GetVariable(kFallbackResponsesServed)),
      num_conditional_refreshes_(
          stats->GetVariable(kNumConditionalRefreshes)),
      beacon_timings_ms_histogram_(
          stats->GetHistogram(kBeaconTimingsMsHistogram)),
      fetch_latency_histogram_(
          stats->GetHistogram(kFetchLatencyHistogram)),
      rewrite_latency_histogram_(
          stats->GetHistogram(kRewriteLatencyHistogram)),
      backend_latency_histogram_(
          stats->GetHistogram(kBackendLatencyHistogram)),
      total_fetch_count_(stats->GetTimedVariable(kTotalFetchCount)),
      total_rewrite_count_(stats->GetTimedVariable(kTotalRewriteCount)),
      num_rewrites_executed_(stats->GetTimedVariable(kRewritesExecuted)),
      num_rewrites_dropped_(stats->GetTimedVariable(kRewritesDropped)) {
  // Timers are not guaranteed to go forward in time, however
  // Histograms will CHECK-fail given a negative value unless
  // EnableNegativeBuckets is called, allowing bars to be created with
  // negative x-axis labels in the histogram.
  // TODO(sligocki): Any reason not to set this by default for all Histograms?
  beacon_timings_ms_histogram_->EnableNegativeBuckets();
  fetch_latency_histogram_->EnableNegativeBuckets();
  rewrite_latency_histogram_->EnableNegativeBuckets();
  backend_latency_histogram_->EnableNegativeBuckets();

  for (int i = 0; i < RewriteDriverFactory::kNumWorkerPools; ++i) {
    thread_queue_depths_.push_back(
        new Waveform(thread_system, timer, kNumWaveformSamples));
  }
}

RewriteStats::~RewriteStats() {
  STLDeleteElements(&thread_queue_depths_);
}

}  // namespace net_instaweb
