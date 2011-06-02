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

// Author: jmarantz@google.com (Joshua Marantz)
//     and sligocki@google.com (Shawn Ligocki)

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_RESOURCE_MANAGER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_RESOURCE_MANAGER_H_

#include <set>
#include <vector>

#include "base/scoped_ptr.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/rewriter/public/blocking_behavior.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/worker.h"

namespace net_instaweb {

class AbstractLock;
class AbstractMutex;
class CacheInterface;
class ContentType;
class FileSystem;
class FilenameEncoder;
class Hasher;
class MessageHandler;
class NamedLockManager;
class QueuedWorker;
class ResourceContext;
class ResponseHeaders;
class RewriteDriver;
class RewriteDriverFactory;
class Statistics;
class ThreadSystem;
class Timer;
class UrlAsyncFetcher;
class UrlSegmentEncoder;
class Variable;

typedef RefCountedPtr<OutputResource> OutputResourcePtr;
typedef std::vector<OutputResourcePtr> OutputResourceVector;

// TODO(jmarantz): Rename this class to ServerContext, as it no longer
// contains much logic about resources -- that's been moved to RewriteDriver,
// which should be renamed RequestContext.
class ResourceManager {
 public:
  // This value is a shared constant so that it can also be used in
  // the Apache-specific code that repairs our caching headers downstream
  // of mod_headers.
  static const char kResourceEtagValue[];
  static const char kCacheKeyResourceNamePrefix[];

  ResourceManager(const StringPiece& file_prefix,
                  FileSystem* file_system,
                  FilenameEncoder* filename_encoder,
                  UrlAsyncFetcher* url_async_fetcher,
                  Hasher* hasher,
                  HTTPCache* http_cache,
                  CacheInterface* metadata_cache,
                  NamedLockManager* lock_manager,
                  MessageHandler* handler,
                  Statistics* statistics,
                  ThreadSystem* thread_system,
                  RewriteDriverFactory* factory);
  ~ResourceManager();

  // Initialize statistics gathering.
  static void Initialize(Statistics* statistics);

  // Set time and cache headers with long TTL (including Date, Last-Modified,
  // Cache-Control, Etags, Expires).
  //
  // Also sets Content-Type headers if content_type is provided.
  // If content_type is null, the Content-Type header is omitted.
  void SetDefaultLongCacheHeaders(const ContentType* content_type,
                                  ResponseHeaders* header) const;

  // Changes the content type of a pre-initialized header.
  void SetContentType(const ContentType* content_type, ResponseHeaders* header);

  StringPiece filename_prefix() const { return file_prefix_; }
  void set_filename_prefix(const StringPiece& file_prefix);
  Statistics* statistics() const { return statistics_; }
  void set_relative_path(bool x) { relative_path_ = x; }
  NamedLockManager* lock_manager() const { return lock_manager_; }

  // Writes the specified contents into the output resource, retaining
  // both a name->filename map and the filename->contents map.
  //
  // TODO(jmarantz): add last_modified arg.
  bool Write(HttpStatus::Code status_code,
             const StringPiece& contents, OutputResource* output,
             int64 origin_expire_time_ms, MessageHandler* handler);

  // Writes out a note that constructing given output resource is
  // not beneficial, and hence should not be attempted until origin's expiration
  // If your filter uses this, it should look at the ->optimizable() property
  // of resources when transforming
  void WriteUnoptimizable(OutputResource* output,
                          int64 origin_expire_time_ms, MessageHandler* handler);

  // Writes out a cache entry telling us how to get to the processed version
  // (output) of some resource given the original source URL and summary of the
  // processing done, such as the filter code and any custom information
  // stored by the filter which are all packed inside the ResourceNamer.
  // This entry expires as soon as the origin does. If no optimization
  // was possible, it records that fact.
  void CacheComputedResourceMapping(OutputResource* output,
                                    int64 origin_expire_time_ms,
                                    MessageHandler* handler);

  // Returns true if the resource with given date and TTL is going to expire
  // shortly and should hence be proactively re-fetched.
  bool IsImminentlyExpiring(int64 start_date_ms, int64 expire_ms) const;

  // TODO(jmarantz): check thread safety in Apache.
  Hasher* hasher() const { return hasher_; }
  FileSystem* file_system() { return file_system_; }
  FilenameEncoder* filename_encoder() const { return filename_encoder_; }
  UrlAsyncFetcher* url_async_fetcher() { return url_async_fetcher_; }
  Timer* timer() const { return http_cache_->timer(); }
  HTTPCache* http_cache() { return http_cache_; }

  // Cache for small non-HTTP objects.
  //
  // Note that this might share namespace with the HTTP cache, so make sure
  // your key names do not start with http://.
  CacheInterface* metadata_cache() { return metadata_cache_; }

  // Whether or not resources should hit the filesystem.
  bool store_outputs_in_file_system() { return store_outputs_in_file_system_; }
  void set_store_outputs_in_file_system(bool store) {
    store_outputs_in_file_system_ = store;
  }

  void RefreshIfImminentlyExpiring(Resource* resource,
                                   MessageHandler* handler) const;

  Variable* resource_url_domain_rejections() {
    return resource_url_domain_rejections_;
  }
  Variable* cached_output_missed_deadline() {
    return cached_output_missed_deadline_;
  }
  Variable* cached_output_hits() {
    return cached_output_hits_;
  }
  Variable* cached_output_misses() {
    return cached_output_misses_;
  }
  Variable* resource_404_count() { return resource_404_count_; }
  Variable* slurp_404_count() { return slurp_404_count_; }

  MessageHandler* message_handler() const { return message_handler_; }

  // Loads contents of resource asynchronously, calling callback when
  // done.  If the resource contents are cached, the callback will
  // be called directly, rather than asynchronously.  The resource
  // will be passed to the callback, with its contents and headers filled in.
  void ReadAsync(Resource::AsyncCallback* callback);

  // Creates a reference-counted pointer to a new OutputResource object.
  //
  // The content type is taken from the input_resource, but can be modified
  // with SetType later if that is not correct (e.g. due to image transcoding).

  // Constructs an output resource corresponding to the specified input resource
  // and encoded using the provided encoder.  Assumes permissions checking
  // occurred when the input resource was constructed, and does not do it again.
  // To avoid if-chains, tolerates a NULL input_resource (by returning NULL).
  // TODO(jmaessen, jmarantz): Do we want to permit NULL input_resources here?
  // jmarantz has evinced a distaste.
  OutputResourcePtr CreateOutputResourceFromResource(
      const RewriteOptions* options,
      const StringPiece& filter_prefix,
      const UrlSegmentEncoder* encoder,
      const ResourceContext* data,
      const ResourcePtr& input_resource,
      OutputResourceKind kind);

  // Creates an output resource where the name is provided by the rewriter.
  // The intent is to be able to derive the content from the name, for example,
  // by encoding URLs and metadata.
  //
  // This method succeeds unless the filename is too long.
  //
  // This name is prepended with path for writing hrefs, and the resulting url
  // is encoded and stored at file_prefix when working with the file system.  So
  // hrefs are:
  //    $(PATH)/$(NAME).pagespeed.$(FILTER_PREFIX).$(HASH).$(CONTENT_TYPE_EXT)
  //
  // 'type' arg can be null if it's not known, or is not in our ContentType
  // library.
  OutputResourcePtr CreateOutputResourceWithPath(
      const RewriteOptions* options, const StringPiece& path,
      const StringPiece& filter_prefix, const StringPiece& name,
      const ContentType* type, OutputResourceKind kind);

  // Attempt to obtain a named lock.  Return true if we do so.  If the
  // object is expensive to create, this lock should be held during
  // its creation to avoid multiple rewrites happening at once.  The
  // lock will be unlocked when creation_lock is reset or destructed.
  bool LockForCreation(const GoogleString& name,
                       BlockingBehavior block,
                       scoped_ptr<AbstractLock>* creation_lock);

  // Setters should probably only be used in testing.
  void set_hasher(Hasher* hasher) { hasher_ = hasher; }
  void set_url_async_fetcher(UrlAsyncFetcher* fetcher) {
    url_async_fetcher_ = fetcher;
  }

  // Handles an incoming beacon request by incrementing the appropriate
  // variables.  Returns true if the url was parsed and handled correctly; in
  // this case a 204 No Content response should be sent.  Returns false if the
  // url could not be parsed; in this case the request should be declined.
  bool HandleBeacon(const StringPiece& unparsed_url);

  RewriteDriver* decoding_driver() const { return decoding_driver_.get(); }

  RewriteOptions* options() { return &options_; }

  // Generates a new managed RewriteDriver using the RewriteOptions
  // managed by this class.  Each RewriteDriver is not thread-safe,
  // but you can generate a RewriteDriver* for each thread.  The
  // returned drivers manage themselves: when the HTML parsing and
  // rewriting is done they will be returned to the pool.
  //
  // Filters allocated using this mechanism have their filter-chain
  // already frozen (see AddFilters()).
  RewriteDriver* NewRewriteDriver();

  // Generates a new unmanaged RewriteDriver using the RewriteOptions
  // managed by this class.  Each RewriteDriver is not thread-safe,
  // but you can generate a RewriteDriver* for each thread.  The
  // returned drivers must be explicitly deleted by the caller.
  //
  // Filters allocated using this mechanism have not yet frozen their
  // filters, and so callers may explicitly enable individual filters
  // on the driver, and then call AddFilters to freeze them.
  RewriteDriver* NewUnmanagedRewriteDriver();

  // Like NewUnmanagedRewriteDriver, but adds adds all the filters
  // specified in the options.
  //
  // Filters allocated using this mechanism have their filter-chain
  // already frozen (see AddFilters()).
  //
  // Takes ownership of 'options'.
  RewriteDriver* NewCustomRewriteDriver(RewriteOptions* options);

  // Puts a RewriteDriver back on the free pool.  This is intended to
  // be called by a RewriteDriver on itself, once all pending
  // activites on it have completed, including HTML Parsing
  // (FinishParse) and all pending Rewrites.
  //
  // This can only be used with RewriteDrivers created with default
  // options.  RewiteDrivers with custom options cannot be recycled
  // and must be deleted.
  //
  // RewriteDrivers with custom options should not call this function on
  // themselves.
  //
  // TODO(jmarantz): this is a potential performance issue for Apache
  // installations that set custom options in .htaccess files, where
  // essentially every RewriteDriver will be a custom driver.  To
  // resolve this we need to make a comparator for RewriteOptions
  // so that we can determine option-equivalence and, potentially,
  // keep free-lists for each unique option-set.
  void ReleaseRewriteDriver(RewriteDriver* rewrite_driver);

  // Queues up a task to run on the Rewrite thread.
  void AddRewriteTask(Worker::Closure* task);

  ThreadSystem* thread_system() { return thread_system_; }

 private:
  GoogleString file_prefix_;
  int resource_id_;  // Sequential ids for temporary Resource filenames.
  FileSystem* file_system_;
  FilenameEncoder* filename_encoder_;
  UrlAsyncFetcher* url_async_fetcher_;
  Hasher* hasher_;
  Statistics* statistics_;

  // Counts how many URLs we reject because they come from a domain that
  // is not authorized.
  Variable* resource_url_domain_rejections_;

  // Counts how many times we had a cache-hit for the output resource
  // partitioning, but it came too late to be used for the rewrite.
  Variable* cached_output_missed_deadline_;

  // Counts how many times we had a successful cache-hit for output
  // resource partitioning.
  Variable* cached_output_hits_;

  // Counts how many times we had a cache-miss for output
  // resource partitioning.
  Variable* cached_output_misses_;

  // Tracks 404s sent to clients for resource requests.
  Variable* resource_404_count_;

  // Tracks 404s sent clients to when slurping.
  Variable* slurp_404_count_;

  // Used for recording results from beacons from 'add_instrumentation_filter'.
  Variable* total_page_load_ms_;
  Variable* page_load_count_;

  HTTPCache* http_cache_;
  CacheInterface* metadata_cache_;
  bool relative_path_;
  bool store_outputs_in_file_system_;
  NamedLockManager* lock_manager_;
  GoogleString max_age_string_;
  MessageHandler* message_handler_;
  ThreadSystem* thread_system_;

  // RewriteDrivers that were previously allocated, but have
  // been released with ReleaseRewriteDriver, and are ready
  // for re-use with NewRewriteDriver.
  std::vector<RewriteDriver*> available_rewrite_drivers_;

  // RewriteDrivers that are currently in use.  This is retained
  // as a sanity check to make sure our system is coherent,
  // and to facilitate complete cleanup if a Shutdown occurs
  // while a request is in flight.
  std::set<RewriteDriver*> active_rewrite_drivers_;

  // If set, a RewriteDriverFactory provides a mechanism to add
  // platform-specific filters to a RewriteDriver.
  RewriteDriverFactory* factory_;

  scoped_ptr<AbstractMutex> rewrite_drivers_mutex_;

  // Note: this must be before decoding_driver_ since it's needed to init it.
  RewriteOptions options_;

  // Keep around a RewriteDriver just for decoding resource URLs, using
  // the default options.  This is possible because the id->RewriteFilter
  // table is fully constructed independent of the options.
  //
  // TODO(jmarantz): If domain-sharding or domain-rewriting is
  // specified in a Directory scope or .htaccess file, the decoding
  // driver will not see them.  This is blocks effective
  // implementation of these features in environments where all
  // configuration must be done by .htaccess.
  scoped_ptr<RewriteDriver> decoding_driver_;

  scoped_ptr<QueuedWorker> rewrite_worker_;

  DISALLOW_COPY_AND_ASSIGN(ResourceManager);
};

class ResourceManagerHttpCallback : public HTTPCache::Callback {
 public:
  ResourceManagerHttpCallback(Resource::AsyncCallback* resource_callback,
                              ResourceManager* resource_manager)
      : resource_callback_(resource_callback),
        resource_manager_(resource_manager) {
  }
  virtual ~ResourceManagerHttpCallback();
  virtual void Done(HTTPCache::FindResult find_result);

 private:
  Resource::AsyncCallback* resource_callback_;
  ResourceManager* resource_manager_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_RESOURCE_MANAGER_H_
