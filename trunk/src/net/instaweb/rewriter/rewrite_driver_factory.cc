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

// Author: sligocki@google.com (Shawn Ligocki)

#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/http/public/fake_url_async_fetcher.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/http_dump_url_fetcher.h"
#include "net/instaweb/http/public/http_dump_url_writer.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "net/instaweb/http/public/url_fetcher.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/file_system_lock_manager.h"
#include "net/instaweb/util/public/filename_encoder.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/named_lock_manager.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"

namespace net_instaweb {

class Statistics;

RewriteDriverFactory::RewriteDriverFactory(ThreadSystem* thread_system)
    : thread_system_(thread_system) {
  Init();
}

RewriteDriverFactory::RewriteDriverFactory()
    : thread_system_(ThreadSystem::CreateThreadSystem()) {
  Init();
}

void RewriteDriverFactory::Init() {
  url_fetcher_ = NULL;
  url_async_fetcher_ = NULL;
  force_caching_ = false;
  slurp_read_only_ = false;
  slurp_print_urls_ = false;
  async_rewrites_ = true;
  http_cache_backend_ = NULL;
  SetStatistics(&null_statistics_);
  resource_manager_mutex_.reset(thread_system_->NewMutex());
}

RewriteDriverFactory::~RewriteDriverFactory() {
  ShutDown();
}

void RewriteDriverFactory::set_html_parse_message_handler(
    MessageHandler* message_handler) {
  html_parse_message_handler_.reset(message_handler);
}

void RewriteDriverFactory::set_message_handler(
    MessageHandler* message_handler) {
  message_handler_.reset(message_handler);
}

bool RewriteDriverFactory::FetchersComputed() const {
  return (url_fetcher_ != NULL) || (url_async_fetcher_ != NULL);
}

void RewriteDriverFactory::set_slurp_directory(const StringPiece& dir) {
  CHECK(!FetchersComputed())
      << "Cannot call set_slurp_directory "
      << " after ComputeUrl*Fetcher has been called";
  dir.CopyToString(&slurp_directory_);
}

void RewriteDriverFactory::set_slurp_read_only(bool read_only) {
  CHECK(!FetchersComputed())
      << "Cannot call set_slurp_read_only "
      << " after ComputeUrl*Fetcher has been called";
  slurp_read_only_ = read_only;
}

void RewriteDriverFactory::set_slurp_print_urls(bool print_urls) {
  CHECK(!FetchersComputed())
      << "Cannot call set_slurp_print_urls "
      << " after ComputeUrl*Fetcher has been called";
  slurp_print_urls_ = print_urls;
}

void RewriteDriverFactory::set_file_system(FileSystem* file_system) {
  file_system_.reset(file_system);
}

// TODO(jmarantz): Change this to set_base_url_fetcher
void RewriteDriverFactory::set_base_url_fetcher(UrlFetcher* url_fetcher) {
  CHECK(!FetchersComputed())
      << "Cannot call set_base_url_fetcher "
      << " after ComputeUrl*Fetcher has been called";
  CHECK(base_url_async_fetcher_.get() == NULL)
      << "Only call one of set_base_url_fetcher and set_base_url_async_fetcher";
  base_url_fetcher_.reset(url_fetcher);
}

void RewriteDriverFactory::set_base_url_async_fetcher(
    UrlAsyncFetcher* url_async_fetcher) {
  CHECK(!FetchersComputed())
      << "Cannot call set_base_url_fetcher "
      << " after ComputeUrl*Fetcher has been called";
  CHECK(base_url_fetcher_.get() == NULL)
      << "Only call one of set_base_url_fetcher and set_base_url_async_fetcher";
  base_url_async_fetcher_.reset(url_async_fetcher);
}

void RewriteDriverFactory::set_hasher(Hasher* hasher) {
  hasher_.reset(hasher);
  if (resource_manager_.get() != NULL) {
    resource_manager_->set_hasher(hasher);
  }
}

void RewriteDriverFactory::set_timer(Timer* timer) {
  timer_.reset(timer);
}

void RewriteDriverFactory::set_filename_encoder(FilenameEncoder* e) {
  filename_encoder_.reset(e);
}

MessageHandler* RewriteDriverFactory::html_parse_message_handler() {
  if (html_parse_message_handler_ == NULL) {
    html_parse_message_handler_.reset(DefaultHtmlParseMessageHandler());
  }
  return html_parse_message_handler_.get();
}

MessageHandler* RewriteDriverFactory::message_handler() {
  if (message_handler_ == NULL) {
    message_handler_.reset(DefaultMessageHandler());
  }
  return message_handler_.get();
}

FileSystem* RewriteDriverFactory::file_system() {
  if (file_system_ == NULL) {
    file_system_.reset(DefaultFileSystem());
  }
  return file_system_.get();
}

Timer* RewriteDriverFactory::timer() {
  if (timer_ == NULL) {
    timer_.reset(DefaultTimer());
  }
  return timer_.get();
}

Hasher* RewriteDriverFactory::hasher() {
  if (hasher_ == NULL) {
    hasher_.reset(NewHasher());
  }
  return hasher_.get();
}

NamedLockManager* RewriteDriverFactory::DefaultLockManager() {
  return new FileSystemLockManager(file_system(), LockFilePrefix(),
                                   timer(), message_handler());
}

NamedLockManager* RewriteDriverFactory::lock_manager() {
  if (lock_manager_ == NULL) {
    lock_manager_.reset(DefaultLockManager());
  }
  return lock_manager_.get();
}

bool RewriteDriverFactory::set_filename_prefix(StringPiece p) {
  p.CopyToString(&filename_prefix_);
  if (file_system()->IsDir(filename_prefix_.c_str(),
                           message_handler()).is_true()) {
    return true;
  }

  if (!file_system()->RecursivelyMakeDir(filename_prefix_, message_handler())) {
    message_handler()->FatalError(
        filename_prefix_.c_str(), 0,
        "Directory does not exist and cannot be created");
    return false;
  }

  AddCreatedDirectory(filename_prefix_);
  return true;
}

StringPiece RewriteDriverFactory::filename_prefix() {
  return filename_prefix_;
}

HTTPCache* RewriteDriverFactory::http_cache() {
  if (http_cache_ == NULL) {
    http_cache_backend_ = DefaultCacheInterface();
    http_cache_.reset(new HTTPCache(
        http_cache_backend_, timer(), statistics()));
    http_cache_->set_force_caching(force_caching_);
  }
  return http_cache_.get();
}

void RewriteDriverFactory::SetAsyncRewrites(bool x) {
  async_rewrites_ = x;
  ScopedMutex lock(resource_manager_mutex_.get());
  if (resource_manager_.get() != NULL) {
    resource_manager_->set_async_rewrites(async_rewrites_);
  }
}

ResourceManager* RewriteDriverFactory::CreateResourceManager() {
  CHECK(http_cache_ != NULL) << "http_cache() must be called first";
  CHECK(!filename_prefix_.empty())
      << "Must specify --filename_prefix or call "
      << "RewriteDriverFactory::set_filename_prefix.";
  ResourceManager* resource_manager = new ResourceManager(thread_system(),
                                                          statistics(),
                                                          rewrite_stats(),
                                                          http_cache());
  resource_manager->set_filename_encoder(filename_encoder());
  resource_manager->set_file_system(file_system());
  resource_manager->set_filename_prefix(filename_prefix_);
  resource_manager->set_url_async_fetcher(ComputeUrlAsyncFetcher());
  resource_manager->set_hasher(hasher());
  resource_manager->set_http_cache(http_cache());
  resource_manager->set_metadata_cache(http_cache_backend_);
  resource_manager->set_lock_manager(lock_manager());
  resource_manager->set_message_handler(message_handler());
  resource_manager->set_store_outputs_in_file_system(
      ShouldWriteResourcesToFileSystem());
  return resource_manager;
}

ResourceManager* RewriteDriverFactory::ComputeResourceManager() {
  ScopedMutex lock(resource_manager_mutex_.get());
  if (resource_manager_ == NULL) {
    // Ensures that we lazily compute http_cache_backend_ and http_cache_.
    http_cache();

    resource_manager_.reset(CreateResourceManager());
    if (temp_options_.get() != NULL) {
      resource_manager_->options()->CopyFrom(*temp_options_.get());
      temp_options_.reset(NULL);
    }
    ResourceManagerCreatedHook();
  }
  resource_manager_->set_async_rewrites(async_rewrites_);
  return resource_manager_.get();
}

RewriteDriver* RewriteDriverFactory::NewRewriteDriver() {
  ResourceManager* resource_manager = ComputeResourceManager();
  RewriteDriver* driver = resource_manager->NewRewriteDriver();
  return driver;
}

void RewriteDriverFactory::AddPlatformSpecificRewritePasses(
    RewriteDriver* driver) {
}

UrlFetcher* RewriteDriverFactory::ComputeUrlFetcher() {
  if (url_fetcher_ == NULL) {
    // Run any hooks like setting up slurp directory.
    FetcherSetupHooks();
    if (slurp_directory_.empty()) {
      if (base_url_fetcher_.get() == NULL) {
        url_fetcher_ = DefaultUrlFetcher();
      } else {
        url_fetcher_ = base_url_fetcher_.get();
      }
    } else {
      SetupSlurpDirectories();
    }
  }
  return url_fetcher_;
}

UrlAsyncFetcher* RewriteDriverFactory::ComputeUrlAsyncFetcher() {
  if (url_async_fetcher_ == NULL) {
    // Run any hooks like setting up slurp directory.
    FetcherSetupHooks();
    if (slurp_directory_.empty()) {
      if (base_url_async_fetcher_.get() == NULL) {
        url_async_fetcher_ = DefaultAsyncUrlFetcher();
      } else {
        url_async_fetcher_ = base_url_async_fetcher_.get();
      }
    } else {
      SetupSlurpDirectories();
    }
  }
  return url_async_fetcher_;
}

void RewriteDriverFactory::SetupSlurpDirectories() {
  CHECK(!FetchersComputed());
  if (slurp_read_only_) {
    CHECK(!FetchersComputed());
    HttpDumpUrlFetcher* dump_fetcher = new HttpDumpUrlFetcher(
        slurp_directory_, file_system(), timer());
    dump_fetcher->set_print_urls(slurp_print_urls_);
    url_fetcher_ = dump_fetcher;
  } else {
    // Check to see if the factory already had set_base_url_fetcher
    // called on it.  If so, then we'll want to use that fetcher
    // as the mechanism for the dump-writer to retrieve missing
    // content from the internet so it can be saved in the slurp
    // directory.
    url_fetcher_ = base_url_fetcher_.get();
    if (url_fetcher_ == NULL) {
      url_fetcher_ = DefaultUrlFetcher();
    }
    HttpDumpUrlWriter* dump_writer = new HttpDumpUrlWriter(
        slurp_directory_, url_fetcher_, file_system(), timer());
    dump_writer->set_print_urls(slurp_print_urls_);
    url_fetcher_ = dump_writer;
  }

  // We do not use real async fetches when slurping.
  url_async_fetcher_ = new FakeUrlAsyncFetcher(url_fetcher_);
}

void RewriteDriverFactory::FetcherSetupHooks() {
}

void RewriteDriverFactory::ResourceManagerCreatedHook() {
}

StringPiece RewriteDriverFactory::LockFilePrefix() {
  return filename_prefix_;
}

void RewriteDriverFactory::ShutDown() {
  // Stop the worker thread first, as it may have outstanding requests
  // that touch various things we're about to blow up.
  if (resource_manager_.get() != NULL) {
    resource_manager_->ShutDownWorkers();
  }

  // Avoid double-destructing the url fetchers if they were not overridden
  // programmatically
  if ((url_async_fetcher_ != NULL) &&
      (url_async_fetcher_ != base_url_async_fetcher_.get())) {
    delete url_async_fetcher_;
  }
  url_async_fetcher_ = NULL;
  if ((url_fetcher_ != NULL) && (url_fetcher_ != base_url_fetcher_.get())) {
    delete url_fetcher_;
  }
  url_fetcher_ = NULL;

  resource_manager_.reset(NULL);

  // Do not reset the timer, file_system, hasher, encoder,
  // html_parse_message_handler, or cache.  Those are deleted when
  // the factory is deleted.
}

// Return a writable RewriteOptions.  If the ResourceManager has
// not yet been created, we lazily create a temp RewriteOptions to
// receive any Options changes, e.g. from flags or config-file parsing.
// Once the ResourceManager is created, which might require some of
// those options to be parsed already, we can transfer the temp
// options to the ResourceManager and get rid of them.
RewriteOptions* RewriteDriverFactory::options() {
  ScopedMutex lock(resource_manager_mutex_.get());
  if (resource_manager_.get() == NULL) {
    if (temp_options_.get() == NULL) {
      temp_options_.reset(new RewriteOptions);
    }
    return temp_options_.get();
  }
  DCHECK(temp_options_.get() == NULL);
  return resource_manager_->options();
}

void RewriteDriverFactory::AddCreatedDirectory(const GoogleString& dir) {
  created_directories_.insert(dir);
}

void RewriteDriverFactory::Initialize(Statistics* statistics) {
  if (statistics != NULL) {
    RewriteStats::Initialize(statistics);
    HTTPCache::Initialize(statistics);
    RewriteDriver::Initialize(statistics);
  }
}

void RewriteDriverFactory::SetStatistics(Statistics* statistics) {
  statistics_ = statistics;
  rewrite_stats_.reset(NULL);
}

RewriteStats* RewriteDriverFactory::rewrite_stats() {
  if (rewrite_stats_.get() == NULL) {
    rewrite_stats_.reset(new RewriteStats(statistics_, thread_system_.get(),
                                          timer()));
  }
  return rewrite_stats_.get();
}

}  // namespace net_instaweb
