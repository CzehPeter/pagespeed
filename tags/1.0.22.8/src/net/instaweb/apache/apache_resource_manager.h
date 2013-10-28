// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jmarantz@google.com (Joshua Marantz)

#ifndef NET_INSTAWEB_APACHE_APACHE_RESOURCE_MANAGER_H_
#define NET_INSTAWEB_APACHE_APACHE_RESOURCE_MANAGER_H_

#include "base/scoped_ptr.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"

struct apr_pool_t;
struct request_rec;
struct server_rec;

namespace net_instaweb {

class ApacheConfig;
class ApacheMessageHandler;
class ApacheRewriteDriverFactory;
class HTTPCache;
class RewriteStats;
class SharedMemStatistics;
class Statistics;
class ThreadSystem;
class UrlPollableAsyncFetcher;
class Variable;

// Creates an Apache-specific ResourceManager.  This differs from base class
// that it incorporates by adding per-VirtualHost configuration, including:
//    - file-cache path & limits
//    - default RewriteOptions.
// Additionally, there are startup semantics for apache's prefork model
// that require a phased initialization.
class ApacheResourceManager : public ResourceManager {
 public:
  ApacheResourceManager(ApacheRewriteDriverFactory* factory,
                        server_rec* server,
                        const StringPiece& version);
  virtual ~ApacheResourceManager();

  GoogleString hostname_identifier() { return hostname_identifier_; }
  void SetStatistics(SharedMemStatistics* x);
  ApacheRewriteDriverFactory* apache_factory() { return apache_factory_; }
  ApacheConfig* config();
  bool InitFileCachePath();

  // Should be called after the child process is forked.
  void ChildInit();

  UrlPollableAsyncFetcher* subresource_fetcher() {
    return subresource_fetcher_;
  }

  bool initialized() const { return initialized_; }

  // Called on notification from Apache on child exit. Returns true
  // if this is the last ResourceManager that exists.
  bool PoolDestroyed();

  // Poll; if we haven't checked the timestamp of
  // $FILE_PREFIX/cache.flush in the past
  // cache_flush_poll_interval_sec_ (default 5) seconds do so, and if
  // the timestamp has expired then update the
  // cache_invalidation_timestamp in global_options, thus flushing the
  // cache.
  //
  // TODO(jmarantz): allow configuration of this option.
  // TODO(jmarantz): allow a URL-based mechanism to flush cache, even if
  // we implement it by simply writing the cache.flush file so other
  // servers can see it.  Note that using shared-memory is not a great
  // plan because we need the cache-invalidation to persist across server
  // restart.
  void PollFilesystemForCacheFlush();

  static void Initialize(Statistics* statistics);

  void set_cache_flush_poll_interval_sec(int num_seconds) {
    cache_flush_poll_interval_sec_ = num_seconds;
  }
  void set_cache_flush_filename(const StringPiece& sp) {
    sp.CopyToString(&cache_flush_filename_);
  }

  const server_rec* server() const { return server_rec_; }

  Variable* statistics_404_count();

  // Reports an error status to the HTTP resource request, and logs
  // the error as a Warning to the log file, and bumps a stat as
  // needed.
  void ReportResourceNotFound(StringPiece error_message, request_rec* request) {
    ReportNotFoundHelper(error_message, request,
                         rewrite_stats()->resource_404_count());
  }

  // Reports an error status to the HTTP statistics request, and logs
  // the error as a Warning to the log file, and bumps a stat as
  // needed.
  void ReportStatisticsNotFound(StringPiece error_message,
                                request_rec* request) {
    ReportNotFoundHelper(error_message, request, statistics_404_count());
  }

  // Reports an error status to the HTTP slurp request, and logs
  // the error as a Warning to the log file, and bumps a stat as
  // needed.
  void ReportSlurpNotFound(StringPiece error_message, request_rec* request) {
    ReportNotFoundHelper(error_message, request,
                         rewrite_stats()->slurp_404_count());
  }

 private:
  void ReportNotFoundHelper(StringPiece url,
                            request_rec* request,
                            Variable* error_count);

  ApacheRewriteDriverFactory* apache_factory_;
  server_rec* server_rec_;
  GoogleString version_;

  // hostname_identifier_ equals to "server_hostname:port" of Apache,
  // it's used to distinguish the name of shared memory,
  // so that each vhost has its own SharedCircularBuffer.
  GoogleString hostname_identifier_;

  bool initialized_;

  // A pollable fetcher provides a Poll() to wait for outstanding
  // fetches to complete.  This is used in
  // instaweb_handler.cc:handle_as_resource() to block the apache
  // request thread until the requested resource has been delivered.
  //
  // TODO(jmarantz): use the scheduler & condition variables to
  // accomplish this instead.
  UrlPollableAsyncFetcher* subresource_fetcher_;

  // State used to implement periodic polling of $FILE_PREFIX/cache.flush.
  // last_cache_flush_check_sec_ is ctor-initialized to 0 so the first
  // time we Poll we will read the file.  If cache_flush_poll_interval_sec_<=0
  // then we turn off polling for cache-flushes.
  scoped_ptr<AbstractMutex> cache_flush_mutex_;
  int64 last_cache_flush_check_sec_;  // seconds since 1970
  int64 cache_flush_poll_interval_sec_;
  GoogleString cache_flush_filename_;

  Variable* cache_flush_count_;

  DISALLOW_COPY_AND_ASSIGN(ApacheResourceManager);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_APACHE_APACHE_RESOURCE_MANAGER_H_
