// Copyright 2010 Google Inc.
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

#include "net/instaweb/apache/apache_rewrite_driver_factory.h"

#include "apr_pools.h"
#include "net/instaweb/apache/apr_file_system.h"
#include "net/instaweb/apache/apr_mutex.h"
#include "net/instaweb/apache/apr_timer.h"
#include "net/instaweb/apache/html_rewriter_config.h"
#include "net/instaweb/apache/md5_hasher.h"
#include "net/instaweb/apache/pagespeed_server_context.h"
#include "net/instaweb/apache/serf_url_async_fetcher.h"
#include "net/instaweb/apache/serf_url_fetcher.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/util/public/file_cache.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/threadsafe_cache.h"
#include "net/instaweb/util/public/write_through_cache.h"

using html_rewriter::SerfUrlAsyncFetcher;

namespace net_instaweb {

ApacheRewriteDriverFactory::ApacheRewriteDriverFactory(
    html_rewriter::PageSpeedServerContext* context)
  : context_(context),
    serf_url_fetcher_(NULL),
    serf_url_async_fetcher_(NULL),
    lru_cache_kb_per_process_(0),
    lru_cache_byte_limit_(0) {
  apr_pool_create(&pool_, context->pool());
  set_filename_prefix(html_rewriter::GetCachePrefix(context_));
  set_url_prefix(html_rewriter::GetUrlPrefix(context_));
  cache_mutex_.reset(NewMutex());
  rewrite_drivers_mutex_.reset(NewMutex());
}

ApacheRewriteDriverFactory::~ApacheRewriteDriverFactory() {
  // We free all the resources before destroy the pool, because some of the
  // resource uses the sub-pool and will destroy them on destruction.
  ShutDown();
  apr_pool_destroy(pool_);
}

FileSystem* ApacheRewriteDriverFactory::DefaultFileSystem() {
  return new html_rewriter::AprFileSystem(pool_);
}

Hasher* ApacheRewriteDriverFactory::NewHasher() {
  return new html_rewriter::Md5Hasher();
}

Timer* ApacheRewriteDriverFactory::DefaultTimer() {
  return new html_rewriter::AprTimer();
}

MessageHandler* ApacheRewriteDriverFactory::DefaultHtmlParseMessageHandler() {
  return new net_instaweb::GoogleMessageHandler();
}

MessageHandler* ApacheRewriteDriverFactory::DefaultMessageHandler() {
  return new net_instaweb::GoogleMessageHandler();
}

CacheInterface* ApacheRewriteDriverFactory::DefaultCacheInterface() {
  CacheInterface* cache = new FileCache(
      html_rewriter::GetFileCachePath(context_),
      file_system(),
      filename_encoder());
  if (lru_cache_kb_per_process_ != 0) {
    // TODO(jmarantz): Allow configuration of the amount of memory to devote
    // to the LRU cache.
    LRUCache* lru_cache = new LRUCache(lru_cache_kb_per_process_ * 1024);

    // We only add the threasafe-wrapper to the LRUCache.  The FileCache
    // is naturally thread-safe because it's got no writable member variables.
    // And surrounding that slower-running class with a mutex would likely
    // cause contention.
    ThreadsafeCache* ts_cache = new ThreadsafeCache(lru_cache, cache_mutex());
    WriteThroughCache* write_through_cache =
        new WriteThroughCache(ts_cache, cache);
    // By default, WriteThroughCache does not limit the size of entries going
    // into its front cache.
    if (lru_cache_byte_limit_ != 0) {
      write_through_cache->set_cache1_limit(lru_cache_byte_limit_);
    }
    cache = write_through_cache;
  }
  return cache;
}

UrlFetcher* ApacheRewriteDriverFactory::DefaultUrlFetcher() {
  if (serf_url_fetcher_ == NULL) {
    DefaultAsyncUrlFetcher();  // Create async fetcher if necessary.
    serf_url_fetcher_ = new html_rewriter::SerfUrlFetcher(
        context_, serf_url_async_fetcher_);
  }
  return serf_url_fetcher_;
}

UrlAsyncFetcher* ApacheRewriteDriverFactory::DefaultAsyncUrlFetcher() {
  if (serf_url_async_fetcher_ == NULL) {
    serf_url_async_fetcher_ = new SerfUrlAsyncFetcher(
        html_rewriter::GetFetcherProxy(context_), pool_);
  }
  return serf_url_async_fetcher_;
}


HtmlParse* ApacheRewriteDriverFactory::DefaultHtmlParse() {
  return new HtmlParse(html_parse_message_handler());
}

AbstractMutex* ApacheRewriteDriverFactory::NewMutex() {
  return new html_rewriter::AprMutex(pool_);
}

RewriteDriver* ApacheRewriteDriverFactory::GetRewriteDriver() {
  RewriteDriver* rewrite_driver = NULL;
  if (!available_rewrite_drivers_.empty()) {
    rewrite_driver = available_rewrite_drivers_.back();
    available_rewrite_drivers_.pop_back();
  } else {
    // Create a RewriteDriver using base class.
    rewrite_driver = NewRewriteDriver();
  }
  active_rewrite_drivers_.insert(rewrite_driver);
  return rewrite_driver;
}

void ApacheRewriteDriverFactory::ReleaseRewriteDriver(
    RewriteDriver* rewrite_driver) {
  int count = active_rewrite_drivers_.erase(rewrite_driver);
  if (count != 1) {
    LOG(ERROR) << "Remove rewrite driver from the active list.";
  } else {
    available_rewrite_drivers_.push_back(rewrite_driver);
  }
}

void ApacheRewriteDriverFactory::ShutDown() {
  cache_mutex_.reset(NULL);
  rewrite_drivers_mutex_.reset(NULL);
  RewriteDriverFactory::ShutDown();
}

}  // namespace net_instaweb
