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
//
// Note: when making changes to this file, a very good sanity-check to run,
// once tests pass, is:
//
// valgrind --leak-check=full ..../src/out/Debug/pagespeed_automatic_test
//     "--gtest_filter=RewriteContextTest*"

#include "net/instaweb/rewriter/public/rewrite_context.h"

#include <cstddef>                     // for size_t
#include <algorithm>
#include <utility>                      // for pair
#include <vector>

#include "base/logging.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/resource_namer.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/dynamic_annotations.h"  // RunningOnValgrind
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/named_lock_manager.h"
#include "net/instaweb/util/public/proto_util.h"
#include "net/instaweb/util/public/queued_alarm.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/shared_string.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/stl_util.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/public/url_segment_encoder.h"
#include "net/instaweb/util/public/writer.h"

namespace net_instaweb {

class RewriteFilter;

namespace {

const char kRewriteContextLockPrefix[] = "rc:";

// Manages freshening of all the inputs of the given context. If any of the
// input resources change, this deletes the corresponding metadata. Otherwise,
// we update the metadata and write it out.
class FreshenMetadataUpdateManager {
 public:
  // Takes ownership of mutex.
  FreshenMetadataUpdateManager(const GoogleString& partition_key,
                               CacheInterface* metadata_cache,
                               AbstractMutex* mutex)
      : partition_key_(partition_key),
        metadata_cache_(metadata_cache),
        mutex_(mutex),
        num_pending_freshens_(0),
        all_freshens_triggered_(false),
        should_delete_cache_key_(false) {}

  ~FreshenMetadataUpdateManager() {}

  void Done(bool lock_failure, bool resource_ok) {
    bool should_cleanup = false;
    {
      ScopedMutex lock(mutex_.get());
      --num_pending_freshens_;
      if (!lock_failure && !resource_ok) {
        should_delete_cache_key_ = true;
      }
      should_cleanup = ShouldCleanup();
    }
    if (should_cleanup) {
      Cleanup();
    }
  }

  void MarkAllFreshensTriggered() {
    bool should_cleanup = false;
    {
      ScopedMutex lock(mutex_.get());
      all_freshens_triggered_ = true;
      should_cleanup = ShouldCleanup();
    }
    if (should_cleanup) {
      Cleanup();
    }
  }

  void IncrementFreshens(const OutputPartitions& partitions) {
    ScopedMutex lock(mutex_.get());
    if (partitions_.get() == NULL) {
      // Copy OutputPartitions lazily.
      OutputPartitions* cloned_partitions = new OutputPartitions;
      cloned_partitions->CopyFrom(partitions);
      partitions_.reset(cloned_partitions);
    }
    num_pending_freshens_++;
  }

  InputInfo* GetInputInfo(int partition_index, int input_index) {
    return partitions_->mutable_partition(partition_index)->
        mutable_input(input_index);
  }

 private:
  bool ShouldCleanup() {
    mutex_->DCheckLocked();
    return (num_pending_freshens_ == 0) && all_freshens_triggered_;
  }

  void Cleanup() {
    if (should_delete_cache_key_) {
      // One of the resources changed. Delete the metadata.
      metadata_cache_->Delete(partition_key_);
    } else if (partitions_.get() != NULL) {
      GoogleString buf;
      {
        StringOutputStream sstream(&buf);  // finalizes buf in destructor
        partitions_->SerializeToZeroCopyStream(&sstream);
      }
      // Write the updated partition info to the metadata cache.
      metadata_cache_->PutSwappingString(partition_key_, &buf);
    }
    delete this;
  }

  // This is copied lazily.
  scoped_ptr<OutputPartitions> partitions_;
  GoogleString partition_key_;
  CacheInterface* metadata_cache_;
  scoped_ptr<AbstractMutex> mutex_;
  int num_pending_freshens_;
  bool all_freshens_triggered_;
  bool should_delete_cache_key_;

  DISALLOW_COPY_AND_ASSIGN(FreshenMetadataUpdateManager);
};

}  // namespace

// Used to pass the result of the metadata cache lookup from OutputCacheCallback
// to RewriteContext.  These are deleted by RewriteContext.
struct RewriteContext::CacheLookupResult {
  CacheLookupResult()
      : cache_ok(false),
        can_revalidate(false),
        partitions(new OutputPartitions) {}

  bool cache_ok;
  bool can_revalidate;
  InputInfoStarVector revalidate;
  scoped_ptr<OutputPartitions> partitions;
};

// Two callback classes for completed caches & fetches.  These gaskets
// help RewriteContext, which knows about all the pending inputs,
// trigger the rewrite once the data is available.  There are two
// versions of the callback.

// Callback to wake up the RewriteContext when the partitioning is looked up
// in the cache.  This takes care of parsing and validation of cached results.
// The RewriteContext can then decide whether to queue the output-resource for a
// DOM update, or re-initiate the Rewrite, depending on the metadata returned.
// Note that the parsing and validation happens in the caching thread and in
// Apache this will block other cache lookups from starting.  Hence this should
// be as streamlined as possible.
class RewriteContext::OutputCacheCallback : public CacheInterface::Callback {
 public:
  typedef void (RewriteContext::*CacheResultHandlerFunction)(
      CacheLookupResult* cache_result);

  OutputCacheCallback(RewriteContext* rc, CacheResultHandlerFunction function)
      : rewrite_context_(rc), function_(function),
        cache_result_(new CacheLookupResult) {}

  virtual ~OutputCacheCallback() {}

  virtual void Done(CacheInterface::KeyState state) {
    RewriteDriver* rewrite_driver = rewrite_context_->Driver();
    rewrite_driver->AddRewriteTask(MakeFunction(
        rewrite_context_, function_, cache_result_.release()));
    delete this;
  }

 protected:
  virtual bool ValidateCandidate(const GoogleString& key,
                                 CacheInterface::KeyState state) {
    DCHECK(!cache_result_->cache_ok);
    // The following is used to hold the cache lookup information obtained from
    // the current cache's value.  Note that the cache_ok field of this is not
    // used as we update cache_result_->cache_ok directly.
    CacheLookupResult candidate_cache_result;
    cache_result_->cache_ok = TryDecodeCacheResult(
        state, *value(), &candidate_cache_result);
    bool use_this_revalidate = (candidate_cache_result.can_revalidate &&
                                (!cache_result_->can_revalidate ||
                                 (candidate_cache_result.revalidate.size() <
                                  cache_result_->revalidate.size())));
    // For the first call to ValidateCandidate if
    // candidate_cache_result.can_revalidate is true, then use_this_revalidate
    // will also be true (since cache_result_->can_revalidate will be false from
    // CacheLookupResult construction).
    bool use_partitions = true;
    if (!cache_result_->cache_ok) {
      if (use_this_revalidate) {
        cache_result_->can_revalidate = true;
        cache_result_->revalidate.swap(candidate_cache_result.revalidate);
        // cache_result_->partitions should be set to
        // candidate_cache_result.partitions, so that the pointers in
        // cache_result_->revalidate are valid.
      } else {
        // If the current cache value is not ok and if an earlier cache value
        // has a better revalidate than the current then do not use the current
        // candidate partitions and revalidate.
        use_partitions = false;
      }
    }
    // At this point the following holds:
    // use_partitions is true iff cache_result_->cache_ok is true or revalidate
    // has been moved to cache_result_->revalidate.
    if (use_partitions) {
      cache_result_->partitions.reset(
          candidate_cache_result.partitions.release());
    }
    // We return cache_result_->cache_ok.  This means for the last call to
    // ValidateCandidate we might return false when we might actually end up
    // using the cached result via revalidate.
    return cache_result_->cache_ok;
  }

 private:
  bool AreInputInfosEqual(const InputInfo& input_info,
                          const InputInfo& fsmdc_info,
                          int64 mtime_ms) {
    return (fsmdc_info.has_last_modified_time_ms() &&
            fsmdc_info.has_input_content_hash() &&
            fsmdc_info.last_modified_time_ms() == mtime_ms &&
            fsmdc_info.input_content_hash() == input_info.input_content_hash());
  }

  // Checks if the stat() data about the input_info's file matches that in the
  // filesystem metadata cache; it needs to be for the input to be "valid".
  bool IsFilesystemMetadataCacheCurrent(CacheInterface* fsmdc,
                                        const GoogleString& file_key,
                                        const InputInfo& input_info,
                                        int64 mtime_ms) {
    //   Get the filesystem metadata cache (FSMDC) entry for the filename.
    //   If we found an entry,
    //     Extract the FSMDC timestamp and contents hash.
    //     If the FSMDC timestamp == the file's current timestamp,
    //       (the FSMDC contents hash is valid/current/correct)
    //       If the FSMDC content hash == the metadata cache's content hash,
    //         The metadata cache's entry is valid so its input_info is valid.
    //       Else
    //         Return false as the metadata cache's entry is not valid as
    //         someone has changed it on us.
    //     Else
    //       Return false as our FSMDC entry is out of date so we can't
    //       tell if the metadata cache's input_info is valid.
    //   Else
    //     Return false as we can't tell if the metadata cache's input_info is
    //     valid.
    CacheInterface::SynchronousCallback callback;
    fsmdc->Get(file_key, &callback);
    DCHECK(callback.called());
    if (callback.state() == CacheInterface::kAvailable) {
      StringPiece val_str = callback.value()->Value();
      ArrayInputStream input(val_str.data(), val_str.size());
      InputInfo fsmdc_info;
      if (fsmdc_info.ParseFromZeroCopyStream(&input)) {
        // We have a filesystem metadata cache entry: if its timestamp equals
        // the file's, and its contents hash equals the metadata caches's, then
        // the input is valid.
        return AreInputInfosEqual(input_info, fsmdc_info, mtime_ms);
      }
    }
    return false;
  }

  // Update the filesystem metadata cache with the timestamp and contents hash
  // of the given input's file (which is read from disk to compute the hash).
  // Returns false if the file cannot be read.
  bool UpdateFilesystemMetadataCache(ServerContext* server_context,
                                     const GoogleString& file_key,
                                     const InputInfo& input_info,
                                     int64 mtime_ms,
                                     CacheInterface* fsmdc,
                                     InputInfo* fsmdc_info) {
    GoogleString contents;
    if (!server_context->file_system()->ReadFile(
            input_info.filename().c_str(), &contents,
            server_context->message_handler())) {
      return false;
    }
    GoogleString contents_hash =
        server_context->contents_hasher()->Hash(contents);
    fsmdc_info->set_type(InputInfo::FILE_BASED);
    fsmdc_info->set_last_modified_time_ms(mtime_ms);
    fsmdc_info->set_input_content_hash(contents_hash);
    GoogleString buf;
    {
      // MUST be in a block so that sstream is destructed to finalize buf.
      StringOutputStream sstream(&buf);
      fsmdc_info->SerializeToZeroCopyStream(&sstream);
    }
    fsmdc->PutSwappingString(file_key, &buf);
    return true;
  }

  // Checks whether the given input is still unchanged.
  bool IsInputValid(const InputInfo& input_info) {
    switch (input_info.type()) {
      case InputInfo::CACHED: {
        // It is invalid if cacheable inputs have expired or ...
        DCHECK(input_info.has_expiration_time_ms());
        if (!input_info.has_expiration_time_ms()) {
          return false;
        }
        int64 ttl_ms = input_info.expiration_time_ms() -
            rewrite_context_->FindServerContext()->timer()->NowMs();
        if (ttl_ms > 0) {
          return true;
        } else if (ttl_ms + rewrite_context_->Options()->
                   metadata_cache_staleness_threshold_ms() > 0) {
          rewrite_context_->stale_rewrite_ = true;
          return true;
        }
        return false;
      }
      case InputInfo::FILE_BASED: {
        ServerContext* server_context = rewrite_context_->FindServerContext();

        // ... if file-based inputs have changed.
        DCHECK(input_info.has_last_modified_time_ms() &&
               input_info.has_filename());
        if (!input_info.has_last_modified_time_ms() ||
            !input_info.has_filename()) {
          return false;
        }
        int64 mtime_sec;
        server_context->file_system()->Mtime(input_info.filename(), &mtime_sec,
                                             server_context->message_handler());
        int64 mtime_ms = mtime_sec * Timer::kSecondMs;

        CacheInterface* fsmdc = server_context->filesystem_metadata_cache();
        if (fsmdc != NULL) {
          CHECK(fsmdc->IsBlocking());
          if (!input_info.has_input_content_hash()) {
            return false;
          }
          // Construct a host-specific key. The format is somewhat arbitrary,
          // all it needs to do is differentiate the same path on different
          // hosts. If the size of the key becomes a concern we can hash it
          // and hope.
          GoogleString file_key;
          StrAppend(&file_key, "file://", server_context->hostname(),
                    input_info.filename());
          if (IsFilesystemMetadataCacheCurrent(fsmdc, file_key, input_info,
                                               mtime_ms)) {
            return true;
          }
          InputInfo fsmdc_info;
          if (!UpdateFilesystemMetadataCache(server_context, file_key,
                                             input_info, mtime_ms, fsmdc,
                                             &fsmdc_info)) {
            return false;
          }
          // Check again now that we KNOW we have the most up-to-date data
          // in the filesystem metadata cache.
          return AreInputInfosEqual(input_info, fsmdc_info, mtime_ms);
        } else {
          return (mtime_ms == input_info.last_modified_time_ms());
        }
      }
      case InputInfo::ALWAYS_VALID:
        return true;
    }

    DLOG(FATAL) << "Corrupt InputInfo object !?";
    return false;
  }

  // Check that a CachedResult is valid, specifically, that all the inputs are
  // still valid/non-expired.  If return value is false, it will also check to
  // see if we should re-check validity of the CachedResult based on input
  // contents, and set *can_revalidate accordingly. If *can_revalidate is true,
  // *revalidate will contain info on resources to re-check, with the InputInfo
  // pointers being pointers into the partition.
  bool IsCachedResultValid(CachedResult* partition,
                           bool* can_revalidate,
                           InputInfoStarVector* revalidate) {
    bool valid = true;
    *can_revalidate = true;
    for (int j = 0, m = partition->input_size(); j < m; ++j) {
      const InputInfo& input_info = partition->input(j);
      if (!IsInputValid(input_info)) {
        valid = false;
        // We currently do not attempt to re-check file-based resources
        // based on contents; as mtime is a lot more reliable than
        // cache expiration, and permitting 'touch' to force recomputation
        // is potentially useful.
        if (input_info.has_input_content_hash() && input_info.has_index() &&
            (input_info.type() == InputInfo::CACHED)) {
          revalidate->push_back(partition->mutable_input(j));
        } else {
          *can_revalidate = false;
          // No point in checking further.
          return false;
        }
      }
    }
    return valid;
  }

  // Checks whether all the entries in the given partition tables' other
  // dependency table are valid.
  bool IsOtherDependencyValid(const OutputPartitions* partitions) {
    for (int j = 0, m = partitions->other_dependency_size(); j < m; ++j) {
      if (!IsInputValid(partitions->other_dependency(j))) {
        return false;
      }
    }
    return true;
  }

  // Tries to decode result of a cache lookup (which may or may not have
  // succeeded) into partitions (in result->partitions), and also checks the
  // dependency tables.
  //
  // Returns true if cache hit, and all dependencies checked out.
  //
  // May also return false, but set result->can_revalidate to true and output a
  // list of inputs (result->revalidate) to re-check if the situation may be
  // salvageable if inputs did not change.
  //
  // Will return false with result->can_revalidate = false if the cached result
  // is entirely unsalvageable.
  bool TryDecodeCacheResult(CacheInterface::KeyState state,
                            const SharedString& value,
                            CacheLookupResult* result) {
    bool* can_revalidate = &(result->can_revalidate);
    InputInfoStarVector* revalidate = &(result->revalidate);
    OutputPartitions* partitions = result->partitions.get();

    if (state != CacheInterface::kAvailable) {
      rewrite_context_->FindServerContext()->rewrite_stats()->
          cached_output_misses()->Add(1);
      *can_revalidate = false;
      return false;
    }
    // We've got a hit on the output metadata; the contents should
    // be a protobuf.  Try to parse it.
    StringPiece val_str = value.Value();
    ArrayInputStream input(val_str.data(), val_str.size());
    if (partitions->ParseFromZeroCopyStream(&input) &&
        IsOtherDependencyValid(partitions)) {
      bool ok = true;
      *can_revalidate = true;
      for (int i = 0, n = partitions->partition_size(); i < n; ++i) {
        CachedResult* partition = partitions->mutable_partition(i);
        bool can_revalidate_resource;
        if (!IsCachedResultValid(partition, &can_revalidate_resource,
                                 revalidate)) {
          ok = false;
          *can_revalidate = *can_revalidate && can_revalidate_resource;
        }
      }
      return ok;
    } else {
      // This case includes both corrupt protobufs and the case where
      // external dependencies are invalid. We do not attempt to reuse
      // rewrite results by input content hashes even in the second
      // case as that would require us to try to re-fetch those URLs as well.
      // TODO(jmarantz): count cache corruptions in a stat?
      *can_revalidate = false;
      return false;
    }
  }

  RewriteContext* rewrite_context_;
  CacheResultHandlerFunction function_;
  scoped_ptr<CacheLookupResult> cache_result_;
};

// Bridge class for routing cache callbacks to RewriteContext methods
// in rewrite thread. Note that the receiver will have to delete the callback
// (which we pass to provide access to data without copying it)
class RewriteContext::HTTPCacheCallback : public OptionsAwareHTTPCacheCallback {
 public:
  typedef void (RewriteContext::*HTTPCacheResultHandlerFunction)(
      HTTPCache::FindResult, HTTPCache::Callback* data);

  HTTPCacheCallback(RewriteContext* rc, HTTPCacheResultHandlerFunction function)
      : OptionsAwareHTTPCacheCallback(rc->Options(),
                                      rc->Driver()->request_context()),
        rewrite_context_(rc),
        function_(function) {}
  virtual ~HTTPCacheCallback() {}
  virtual void Done(HTTPCache::FindResult find_result) {
    RewriteDriver* rewrite_driver = rewrite_context_->Driver();
    rewrite_driver->AddRewriteTask(MakeFunction(
        rewrite_context_, function_, find_result,
        static_cast<HTTPCache::Callback*>(this)));
  }

 private:
  RewriteContext* rewrite_context_;
  HTTPCacheResultHandlerFunction function_;
  DISALLOW_COPY_AND_ASSIGN(HTTPCacheCallback);
};

// Common code for invoking RewriteContext::ResourceFetchDone for use
// in ResourceFetchCallback and ResourceReconstructCallback.
class RewriteContext::ResourceCallbackUtils {
 public:
  ResourceCallbackUtils(RewriteContext* rc, const ResourcePtr& resource,
                        int slot_index)
      : resource_(resource),
        rewrite_context_(rc),
        slot_index_(slot_index) {
  }

  void Done(bool success) {
    RewriteDriver* rewrite_driver = rewrite_context_->Driver();
    rewrite_driver->AddRewriteTask(
        MakeFunction(rewrite_context_, &RewriteContext::ResourceFetchDone,
                     success, resource_, slot_index_));
  }

 private:
  ResourcePtr resource_;
  RewriteContext* rewrite_context_;
  int slot_index_;
};

// Callback when reading a resource from the network.
class RewriteContext::ResourceFetchCallback : public Resource::AsyncCallback {
 public:
  ResourceFetchCallback(RewriteContext* rc, const ResourcePtr& r,
                        int slot_index)
      : Resource::AsyncCallback(r),
        delegate_(rc, r, slot_index) {
  }

  virtual ~ResourceFetchCallback() {}
  virtual void Done(bool lock_failure, bool resource_ok) {
    delegate_.Done(!lock_failure && resource_ok);
    delete this;
  }

  virtual bool EnableThreaded() const { return true; }

 private:
  ResourceCallbackUtils delegate_;
};

// Callback used when we need to reconstruct a resource we made to satisfy
// a fetch (due to rewrites being nested inside each other).
class RewriteContext::ResourceReconstructCallback
    : public AsyncFetchUsingWriter {
 public:
  // Takes ownership of the driver (e.g. will call Cleanup)
  ResourceReconstructCallback(RewriteDriver* driver, RewriteContext* rc,
                              const OutputResourcePtr& resource, int slot_index)
      : AsyncFetchUsingWriter(driver->request_context(),
                              resource->BeginWrite(driver->message_handler())),
        driver_(driver),
        delegate_(rc, ResourcePtr(resource), slot_index),
        resource_(resource) {
    set_response_headers(resource->response_headers());
  }

  virtual ~ResourceReconstructCallback() {
  }

  virtual void HandleDone(bool success) {
    // Compute the final post-write state of the object, including the hash.
    // Also takes care of dropping creation lock.
    resource_->EndWrite(driver_->message_handler());

    // Make sure to compute the URL, as we'll be killing the rewrite driver
    // shortly, and the driver is needed for URL computation.
    resource_->url();

    delegate_.Done(success);
    driver_->Cleanup();
    delete this;
  }

  virtual void HandleHeadersComplete() {}

 private:
  RewriteDriver* driver_;
  ResourceCallbackUtils delegate_;
  OutputResourcePtr resource_;
  DISALLOW_COPY_AND_ASSIGN(ResourceReconstructCallback);
};

// Callback used when we re-check validity of cached results by contents.
class RewriteContext::ResourceRevalidateCallback
    : public Resource::AsyncCallback {
 public:
  ResourceRevalidateCallback(RewriteContext* rc, const ResourcePtr& r,
                             InputInfo* input_info)
      : Resource::AsyncCallback(r),
        rewrite_context_(rc),
        input_info_(input_info) {
  }

  virtual ~ResourceRevalidateCallback() {
  }

  virtual void Done(bool lock_failure, bool resource_ok) {
    RewriteDriver* rewrite_driver = rewrite_context_->Driver();
    rewrite_driver->AddRewriteTask(
        MakeFunction(rewrite_context_, &RewriteContext::ResourceRevalidateDone,
                     input_info_, !lock_failure && resource_ok));
    delete this;
  }

  virtual bool EnableThreaded() const { return true; }

 private:
  RewriteContext* rewrite_context_;
  InputInfo* input_info_;
};

// Callback that is invoked after freshening a resource. This invokes the
// FreshenMetadataUpdateManager with the relevant updates.
class RewriteContext::RewriteFreshenCallback
    : public Resource::FreshenCallback {
 public:
  RewriteFreshenCallback(const ResourcePtr& resource,
                         int partition_index,
                         int input_index,
                         FreshenMetadataUpdateManager* manager)
      : FreshenCallback(resource),
        partition_index_(partition_index),
        input_index_(input_index),
        manager_(manager) {}

  virtual ~RewriteFreshenCallback() {}

  virtual InputInfo* input_info() {
    return manager_->GetInputInfo(partition_index_, input_index_);
  }

  virtual void Done(bool lock_failure, bool resource_ok) {
    manager_->Done(lock_failure, resource_ok);
    delete this;
  }

 private:
  int partition_index_;
  int input_index_;
  FreshenMetadataUpdateManager* manager_;

  DISALLOW_COPY_AND_ASSIGN(RewriteFreshenCallback);
};

// This class encodes a few data members used for responding to
// resource-requests when the output_resource is not in cache.
class RewriteContext::FetchContext {
 public:
  FetchContext(RewriteContext* rewrite_context,
               AsyncFetch* fetch,
               const OutputResourcePtr& output_resource,
               MessageHandler* handler)
      : rewrite_context_(rewrite_context),
        async_fetch_(fetch),
        output_resource_(output_resource),
        handler_(handler),
        deadline_alarm_(NULL),
        success_(false),
        detached_(false) {
  }

  void SetupDeadlineAlarm() {
    // No point in doing this for on-the-fly resources.
    if (rewrite_context_->kind() == kOnTheFlyResource) {
      return;
    }

    // Can't do this if a subclass forced us to be detached already.
    if (detached_) {
      return;
    }
    RewriteDriver* driver = rewrite_context_->Driver();
    Timer* timer = rewrite_context_->FindServerContext()->timer();

    // Negative rewrite deadline means unlimited.
    if (driver->rewrite_deadline_ms() >= 0) {
      // Startup an alarm which will cause us to return unrewritten content
      // rather than hold up the fetch too long on firing.
      deadline_alarm_ =
          new QueuedAlarm(
              driver->scheduler(), driver->rewrite_worker(),
              timer->NowUs() + (driver->rewrite_deadline_ms() * Timer::kMsUs),
              MakeFunction(this, &FetchContext::HandleDeadline));
    }
  }

  // Must be invoked from main rewrite thread.
  void CancelDeadlineAlarm() {
    if (deadline_alarm_ != NULL) {
      deadline_alarm_->CancelAlarm();
      deadline_alarm_ = NULL;
    }
  }

  // Fired by QueuedAlarm in main rewrite thread.
  void HandleDeadline() {
    deadline_alarm_ = NULL;  // avoid dangling reference.
    rewrite_context_->DetachFetch();
    // It's very tempting to log the output URL here, but it's not safe to do
    // so, as OutputResource::UrlEvenIfHashNotSet can write to the hash,
    // which may race against normal setting of the hash in
    // ResourceManager::Write called off low-priority thread.
    // TODO(sligocki): Log a variable for number of deadline hits.
    ResourcePtr input(rewrite_context_->slot(0)->resource());
    handler_->Message(
        kInfo, "Deadline exceeded for rewrite of resource %s with %s.",
        input->url().c_str(), rewrite_context_->id());
    FetchFallbackDoneImpl(input->contents(), input->response_headers());
  }

  // Note that the callback is called from the RewriteThread.
  void FetchDone() {
    CancelDeadlineAlarm();

    // Cache our results.
    DCHECK_EQ(1, rewrite_context_->num_output_partitions());
    rewrite_context_->WritePartition();

    // If we're running in background, that's basically all we will do.
    if (detached_) {
      rewrite_context_->Driver()->DetachedFetchComplete();
      return;
    }

    GoogleString output;
    bool ok = false;
    ResponseHeaders* response_headers = async_fetch_->response_headers();
    if (success_) {
      if (output_resource_->hash() == requested_hash_) {
        response_headers->CopyFrom(*(
            output_resource_->response_headers()));
        // Use the most conservative Cache-Control considering all inputs.
        ApplyInputCacheControl(response_headers);
        async_fetch_->HeadersComplete();
        ok = async_fetch_->Write(output_resource_->contents(), handler_);
      } else {
        // Our rewrite produced a different hash than what was requested;
        // we better not give it an ultra-long TTL.
        FetchFallbackDone(output_resource_->contents(),
                          output_resource_->response_headers());
        return;
      }
    } else {
      // Rewrite failed. If we can, fallback to the original as rewrite failing
      // may just mean the input isn't optimizable.
      if (rewrite_context_->CanFetchFallbackToOriginal(kFallbackEmergency)) {
        ResourcePtr input_resource(rewrite_context_->slot(0)->resource());
        if (input_resource.get() != NULL && input_resource->HttpStatusOk()) {
          handler_->Message(kWarning, "Rewrite %s failed while fetching %s",
                            input_resource->url().c_str(),
                            output_resource_->UrlEvenIfHashNotSet().c_str());
          // TODO(sligocki): Log variable for number of failed rewrites in
          // fetch path.

          response_headers->CopyFrom(*input_resource->response_headers());
          rewrite_context_->FixFetchFallbackHeaders(response_headers);
          // Use the most conservative Cache-Control considering all inputs.
          // Note that this is needed because FixFetchFallbackHeaders might
          // actually relax things a bit if the input was no-cache.
          ApplyInputCacheControl(response_headers);
          async_fetch_->HeadersComplete();

          ok = rewrite_context_->AbsolutifyIfNeeded(
              input_resource->contents(), async_fetch_, handler_);
        } else {
          GoogleString url = input_resource->url();
          handler_->Warning(
              output_resource_->name().as_string().c_str(), 0,
              "Resource based on %s but cannot access the original",
              url.c_str());
        }
      }
    }

    if (!ok) {
      async_fetch_->response_headers()->SetStatusAndReason(
          HttpStatus::kNotFound);
      // TODO(sligocki): We could be calling this twice if Writes fail above.
      async_fetch_->HeadersComplete();
    }
    rewrite_context_->FetchCallbackDone(ok);
  }

  // This is used in case we used a metadata cache to find an alternative URL
  // to serve --- either a version with a different hash, or that we should
  // serve the original. In this case, we serve it out, but with shorter headers
  // than usual.
  void FetchFallbackDone(const StringPiece& contents,
                         ResponseHeaders* headers) {
    CancelDeadlineAlarm();
    if (detached_) {
      rewrite_context_->Driver()->DetachedFetchComplete();
      return;
    }

    FetchFallbackDoneImpl(contents, headers);
  }

  // Backend for FetchFallbackCacheDone, but can be also invoked
  // for main rewrite when background rewrite is detached.
  void FetchFallbackDoneImpl(const StringPiece& contents,
                             const ResponseHeaders* headers) {
    async_fetch_->response_headers()->CopyFrom(*headers);
    rewrite_context_->FixFetchFallbackHeaders(async_fetch_->response_headers());
    // Use the most conservative Cache-Control considering all inputs.
    ApplyInputCacheControl(async_fetch_->response_headers());
    async_fetch_->HeadersComplete();

    bool ok = rewrite_context_->AbsolutifyIfNeeded(contents, async_fetch_,
                                                   handler_);
    rewrite_context_->FetchCallbackDone(ok);
  }

  void set_requested_hash(const StringPiece& hash) {
    hash.CopyToString(&requested_hash_);
  }

  AsyncFetch* async_fetch() { return async_fetch_; }
  bool detached() const { return detached_; }
  MessageHandler* handler() { return handler_; }
  OutputResourcePtr output_resource() { return output_resource_; }
  const GoogleString& requested_hash() const { return requested_hash_; }

  void set_success(bool success) { success_ = success; }
  void set_detached(bool value) { detached_ = value; }

 private:
  void ApplyInputCacheControl(ResponseHeaders* headers) {
    ResourceVector inputs;
    for (int i = 0; i < rewrite_context_->num_slots(); i++) {
      inputs.push_back(rewrite_context_->slot(i)->resource());
    }

    rewrite_context_->FindServerContext()->ApplyInputCacheControl(inputs,
                                                                  headers);
  }

  RewriteContext* rewrite_context_;
  AsyncFetch* async_fetch_;
  OutputResourcePtr output_resource_;
  MessageHandler* handler_;
  GoogleString requested_hash_;  // hash we were requested as. May be empty.
  QueuedAlarm* deadline_alarm_;

  bool success_;
  bool detached_;

  DISALLOW_COPY_AND_ASSIGN(FetchContext);
};

// Helper for running filter's Rewrite method in low-priority rewrite thread,
// which deals with cancellation of rewrites due to load shedding or shutdown by
// introducing a kTooBusy response if the job gets dumped.
class RewriteContext::InvokeRewriteFunction : public Function {
 public:
  InvokeRewriteFunction(RewriteContext* context, int partition,
                        const OutputResourcePtr& output)
      : context_(context), partition_(partition), output_(output) {
  }

  virtual ~InvokeRewriteFunction() {}

  virtual void Run() {
    context_->FindServerContext()->rewrite_stats()->num_rewrites_executed()
        ->IncBy(1);
    context_->Rewrite(partition_,
                      context_->partitions_->mutable_partition(partition_),
                      output_);
  }

  virtual void Cancel() {
    context_->FindServerContext()->rewrite_stats()->num_rewrites_dropped()
        ->IncBy(1);
    context_->RewriteDone(kTooBusy, partition_);
  }

 private:
  RewriteContext* context_;
  int partition_;
  OutputResourcePtr output_;
};

RewriteContext::RewriteContext(RewriteDriver* driver,
                               RewriteContext* parent,
                               ResourceContext* resource_context)
  : started_(false),
    outstanding_fetches_(0),
    outstanding_rewrites_(0),
    resource_context_(resource_context),
    num_pending_nested_(0),
    parent_(parent),
    driver_(driver),
    num_predecessors_(0),
    chained_(false),
    rewrite_done_(false),
    ok_to_write_output_partitions_(true),
    was_too_busy_(false),
    slow_(false),
    revalidate_ok_(true),
    notify_driver_on_fetch_done_(false),
    force_rewrite_(false),
    stale_rewrite_(false) {
  partitions_.reset(new OutputPartitions);
}

RewriteContext::~RewriteContext() {
  DCHECK_EQ(0, num_predecessors_);
  DCHECK_EQ(0, outstanding_fetches_);
  DCHECK(successors_.empty());
  STLDeleteElements(&nested_);
}

int RewriteContext::num_output_partitions() const {
  return partitions_->partition_size();
}

const CachedResult* RewriteContext::output_partition(int i) const {
  return &partitions_->partition(i);
}

CachedResult* RewriteContext::output_partition(int i) {
  return partitions_->mutable_partition(i);
}

void RewriteContext::AddSlot(const ResourceSlotPtr& slot) {
  CHECK(!started_);

  // TODO(jmarantz): eliminate this transitional code to allow JavascriptFilter
  // to straddle the old rewrite flow and the new async flow.
  if (slot.get() == NULL) {
    return;
  }

  slots_.push_back(slot);
  render_slots_.push_back(false);

  RewriteContext* predecessor = slot->LastContext();
  if (predecessor != NULL) {
    // Note that we don't check for duplicate connections between this and
    // predecessor.  They'll all get counted.
    DCHECK(!predecessor->started_);
    predecessor->successors_.push_back(this);
    ++num_predecessors_;
    chained_ = true;
  }
  slot->AddContext(this);
}

void RewriteContext::RemoveLastSlot() {
  int index = num_slots() - 1;
  slot(index)->DetachContext(this);
  RewriteContext* predecessor = slot(index)->LastContext();
  if (predecessor) {
    predecessor->successors_.erase(
        std::find(predecessor->successors_.begin(),
                  predecessor->successors_.end(), this));
    --num_predecessors_;
  }

  slots_.erase(slots_.begin() + index);
  render_slots_.erase(render_slots_.begin() + index);
}

void RewriteContext::Initiate() {
  CHECK(!started_);
  DCHECK_EQ(0, num_predecessors_);
  Driver()->AddRewriteTask(new MemberFunction0<RewriteContext>(
      &RewriteContext::Start, this));
}

// Initiate a Rewrite if it's ready to be started.  A Rewrite would not
// be startable if was operating on a slot that was already associated
// with another Rewrite.  We would wait for all the preceding rewrites
// to complete before starting this one.
void RewriteContext::Start() {
  DCHECK(!started_);
  DCHECK_EQ(0, num_predecessors_);
  started_ = true;

  // See if any of the input slots are marked as unsafe for use,
  // and if so bail out quickly.
  // TODO(morlovich): Add API for filters to do something more refined.
  for (int c = 0; c < num_slots(); ++c) {
    if (slot(c)->disable_further_processing()) {
      rewrite_done_ = true;
      RetireRewriteForHtml(false /* no rendering*/);
      return;
    }
  }

  // The best-case scenario for a Rewrite is that we have already done
  // it, and just need to look up in our metadata cache what the final
  // rewritten URL is.  In the simplest scenario, we are doing a
  // simple URL substitution.  In a more complex example, we have M
  // css files that get reduced to N combinations.  The
  // OutputPartitions held in the cache tells us that, and we don't
  // need to get any data about the resources that need to be
  // rewritten.  But in either case, we only need one cache lookup.
  //
  // Note that the output_key_name is not necessarily the same as the
  // name of the output.
  // Write partition to metadata cache.
  CacheInterface* metadata_cache = FindServerContext()->metadata_cache();
  SetPartitionKey();

  // See if some other handler already had to do an identical rewrite.
  RewriteContext* previous_handler =
      Driver()->RegisterForPartitionKey(partition_key_, this);
  if (previous_handler == NULL) {
    // When the cache lookup is finished, OutputCacheDone will be called.
    if (force_rewrite_) {
      // Make the metadata cache lookup fail since we want to force a rewrite.
       (new OutputCacheCallback(
           this, &RewriteContext::OutputCacheDone))->Done(
               CacheInterface::kNotFound);
    } else {
      metadata_cache->Get(
          partition_key_, new OutputCacheCallback(
              this, &RewriteContext::OutputCacheDone));
    }
  } else {
    if (previous_handler->slow()) {
      MarkSlow();
    }
    previous_handler->repeated_.push_back(this);
  }
}

namespace {

// Hashes a string into (we expect) a base-64-encoded sequence.  Then
// inserts a "/" after the first character.  The theory is that for
// inlined and combined resources, there is no useful URL hierarchy,
// and we want to avoid creating, in the file-cache, a gigantic flat
// list of names.
//
// We do this split after one character so we just get 64
// subdirectories.  If we have too many subdirectories then the
// file-system will not cache the metadata efficiently.  If we have
// too few then the directories get very large.  The main limitation
// we are working against is in pre-ext4 file systems, there are a
// maximum of 32k subdirectories per directory, and there is not an
// explicit limitation on the number of file.  Additionally,
// old file-systems may not be efficiently indexed, in which case
// adding some hierarchy should help.
GoogleString HashSplit(const Hasher* hasher, const StringPiece& str) {
  GoogleString hash_buffer = hasher->Hash(str);
  StringPiece hash(hash_buffer);
  return StrCat(hash.substr(0, 1), "/", hash.substr(1));
}

}  // namespace

// Utility to log metadata cache lookup info.
// This executes in driver's rewrite thread, i.e., all calls to this are from
// Functions added to the same QueuedWorkedPool::Sequence and so none of the
// calls will be concurrent.
void RewriteContext::LogMetadataCacheInfo(bool cache_ok, bool can_revalidate) {
  if (has_parent()) {
    // We do not log nested rewrites.
    return;
  }
  {
    LogRecord* log_record = Driver()->log_record();
    ScopedMutex lock(log_record->mutex());
    MetadataCacheInfo* metadata_log_info =
        log_record->logging_info()->mutable_metadata_cache_info();
    if (cache_ok) {
      metadata_log_info->set_num_hits(metadata_log_info->num_hits() + 1);
    } else if (can_revalidate) {
      metadata_log_info->set_num_revalidates(
          metadata_log_info->num_revalidates() + 1);
    } else {
      metadata_log_info->set_num_misses(metadata_log_info->num_misses() + 1);
    }
  }
}

void RewriteContext::SetPartitionKey() {
  // In Apache, we are populating a file-cache.  To be friendly to
  // the file system, we want to structure it as follows:
  //
  //   rname/id_signature/encoded_filename
  //
  // Performance constraints:
  //   - max 32k links (created by ".." link from subdirectories) per directory
  //   - avoid excessive high-entropy hierarchy as it will not play well with
  //     the filesystem metadata cache.
  //
  // The natural hierarchy in URLs should be exploited for single-resource
  // rewrites; and in fact the http cache uses that, so it can't be too bad.
  //
  // Data URLs & combined URLs should be encoded & hashed because they lack
  // a useful natural hierarchy to reflect in the file-system.
  //
  // We need to run the URL encoder in order to serialize the
  // resource_context_, but this flattens the hierarchy by encoding
  // slashes.  We want the FileCache hierarchies to reflect the URL
  // hierarchies if possible.  So we use a dummy URL of "" in our
  // url-list for now.
  StringVector urls;
  const Hasher* hasher = FindServerContext()->lock_hasher();
  GoogleString url;
  GoogleString signature = hasher->Hash(Options()->signature());
  GoogleString suffix = CacheKeySuffix();

  if (num_slots() == 1) {
    // Usually a resource-context-specific encoding such as the
    // image dimension will be placed ahead of the URL.  However,
    // in the cache context, we want to put it at the end, so
    // put this encoding right before any context-specific suffix.
    urls.push_back("");
    GoogleString encoding;
    encoder()->Encode(urls, resource_context_.get(), &encoding);
    suffix = StrCat(encoding, "@", suffix);

    url = slot(0)->resource()->url();
    if (StringPiece(url).starts_with("data:")) {
      url = HashSplit(hasher, url);
    }
  } else if (num_slots() == 0) {
    // Ideally we should not be writing cache entries for 0-slot
    // contexts.  However that is currently the case for
    // image-spriting.  It would be preferable to avoid creating an
    // identical empty encoding here for every degenerate sprite
    // attempt, but for the moment we can at least make all the
    // encodings the same so they can share the same cache entry.
    // Note that we clear out the suffix to avoid having separate
    // entries for each CSS files that lacks any images.
    //
    // TODO(morlovich): Maksim has a fix in progress which will
    // eliminate this case.
    suffix.clear();
    url = "empty";
  } else {
    for (int i = 0, n = num_slots(); i < n; ++i) {
      ResourcePtr resource(slot(i)->resource());
      urls.push_back(resource->url());
    }
    encoder()->Encode(urls, resource_context_.get(), &url);
    url = HashSplit(hasher, url);
  }

  partition_key_ = StrCat(ServerContext::kCacheKeyResourceNamePrefix,
                          id(), "_", signature, "/",
                          url, "@", suffix);
}

void RewriteContext::AddRecheckDependency() {
  int64 ttl_ms = Options()->implicit_cache_ttl_ms();
  if (num_slots() == 1) {
    ResourcePtr resource(slot(0)->resource());
    switch (resource->fetch_response_status()) {
      case Resource::kFetchStatusNotSet:
      case Resource::kFetchStatusOK:
      case Resource::kFetchStatusOther:
        break;
      case Resource::kFetchStatus4xxError:
        ttl_ms = Driver()->options()->metadata_input_errors_cache_ttl_ms();
        break;
      case Resource::kFetchStatusUncacheable:
        ttl_ms = FindServerContext()->http_cache()->
            remember_not_cacheable_ttl_seconds() * Timer::kSecondMs;
        break;
    }
  }
  int64 now_ms = FindServerContext()->timer()->NowMs();
  InputInfo* force_recheck = partitions_->add_other_dependency();
  force_recheck->set_type(InputInfo::CACHED);
  force_recheck->set_expiration_time_ms(now_ms + ttl_ms);
}

void RewriteContext::OutputCacheDone(CacheLookupResult* cache_result) {
  DCHECK_LE(0, outstanding_fetches_);
  DCHECK_EQ(static_cast<size_t>(0), outputs_.size());

  scoped_ptr<CacheLookupResult> owned_cache_result(cache_result);

  partitions_.reset(owned_cache_result->partitions.release());
  LogMetadataCacheInfo(owned_cache_result->cache_ok,
                       owned_cache_result->can_revalidate);
  // If OK or worth rechecking, set things up for the cache hit case.
  if (owned_cache_result->cache_ok || owned_cache_result->can_revalidate) {
    for (int i = 0, n = partitions_->partition_size(); i < n; ++i) {
      const CachedResult& partition = partitions_->partition(i);

      // Extract the further processing bit from InputInfo structures
      // back into the slots.
      for (int j = 0; j < partition.input_size(); ++j) {
        const InputInfo& input = partition.input(j);
        if (input.disable_further_processing()) {
          int slot_index = input.index();
          if (slot_index < 0 || slot_index >> slots_.size()) {
            LOG(DFATAL) << "Index of processing disabled slot out of range:"
                        << slot_index;
          } else {
            slots_[slot_index]->set_disable_further_processing(true);
          }
        }
      }

      // Create output resources, if appropriate.
      OutputResourcePtr output_resource;
      if (partition.optimizable() &&
          CreateOutputResourceForCachedOutput(&partition, &output_resource)) {
        outputs_.push_back(output_resource);
      } else {
        outputs_.push_back(OutputResourcePtr(NULL));
      }
    }
  }

  // If the cache gave a miss, or yielded unparsable data, then acquire a lock
  // and start fetching the input resources.
  if (owned_cache_result->cache_ok) {
    OutputCacheHit(false /* no need to write back to cache*/);
  } else {
    MarkSlow();
    if (owned_cache_result->can_revalidate) {
      OutputCacheRevalidate(owned_cache_result->revalidate);
    } else {
      OutputCacheMiss();
    }
  }
}

void RewriteContext::OutputCacheHit(bool write_partitions) {
  Freshen();
  for (int i = 0, n = partitions_->partition_size(); i < n; ++i) {
    if (outputs_[i].get() != NULL) {
      RenderPartitionOnDetach(i);
    }
  }
  ok_to_write_output_partitions_ = write_partitions;
  Finalize();
}

void RewriteContext::OutputCacheMiss() {
  outputs_.clear();
  partitions_->Clear();
  ServerContext* server_context = FindServerContext();
  if (server_context->shutting_down() && !RunningOnValgrind()) {
    FindServerContext()->message_handler()->Message(
        kInfo,
        "RewriteContext::OutputCacheMiss called with "
        "server_context->shutting_down(); leaking the context.");
  } else if (server_context->TryLockForCreation(Lock())) {
    FetchInputs();
  } else {
    // TODO(jmarantz): bump stat for abandoned rewrites due to lock contention.
    ok_to_write_output_partitions_ = false;
    Activate();
  }
}

void RewriteContext::OutputCacheRevalidate(
    const InputInfoStarVector& to_revalidate) {
  DCHECK(!to_revalidate.empty());
  outstanding_fetches_ = to_revalidate.size();

  for (int i = 0, n = to_revalidate.size(); i < n; ++i) {
    InputInfo* input_info = to_revalidate[i];
    ResourcePtr resource = slots_[input_info->index()]->resource();
    FindServerContext()->ReadAsync(
        Resource::kReportFailureIfNotCacheable,
        Driver()->request_context(),
        new ResourceRevalidateCallback(this, resource, input_info));
  }
}

void RewriteContext::RepeatedSuccess(const RewriteContext* primary) {
  CHECK(outputs_.empty());
  CHECK_EQ(num_slots(), primary->num_slots());
  CHECK_EQ(primary->outputs_.size(),
           static_cast<size_t>(primary->num_output_partitions()));
  // Copy over partition tables, outputs, and render_slot_ (as well as
  // was_optimized) information --- everything we can set in normal
  // OutputCacheDone.
  partitions_->CopyFrom(*primary->partitions_.get());
  for (int i = 0, n = primary->outputs_.size(); i < n; ++i) {
    outputs_.push_back(primary->outputs_[i]);
    if ((outputs_[i].get() != NULL) && !outputs_[i]->loaded()) {
      // We cannot safely alias resources that are not loaded, as the loading
      // process is threaded, and would therefore race. Therefore, recreate
      // another copy matching the cache data.
      CreateOutputResourceForCachedOutput(
          &partitions_->partition(i), &outputs_[i]);
    }
  }

  for (int i = 0, n = primary->num_slots(); i < n; ++i) {
    slot(i)->set_was_optimized(primary->slot(i)->was_optimized());
    slot(i)->set_disable_further_processing(
        primary->slot(i)->disable_further_processing());
    render_slots_[i] = primary->render_slots_[i];
  }

  ok_to_write_output_partitions_ = false;
  Finalize();
}

void RewriteContext::RepeatedFailure() {
  CHECK(outputs_.empty());
  CHECK_EQ(0, num_output_partitions());
  rewrite_done_ = true;
  ok_to_write_output_partitions_ = false;
  FinalizeRewriteForHtml();
}

NamedLock* RewriteContext::Lock() {
  NamedLock* result = lock_.get();
  if (result == NULL) {
    // NOTE: This lock is based on hashes so if you use a MockHasher, you may
    // only rewrite a single resource at a time (e.g. no rewriting resources
    // inside resources, see css_image_rewriter_test.cc for examples.)
    //
    // TODO(jmarantz): In the multi-resource rewriters that can generate more
    // than one partition, we create a lock based on the entire set of input
    // URLs, plus a lock for each individual output.  However, in
    // single-resource rewriters, we really only need one of these locks.  So
    // figure out which one we'll go with and use that.
    GoogleString lock_name = StrCat(kRewriteContextLockPrefix, partition_key_);
    result = FindServerContext()->MakeCreationLock(lock_name);
    lock_.reset(result);
  }
  return result;
}

void RewriteContext::FetchInputs() {
  ++num_predecessors_;

  for (int i = 0, n = slots_.size(); i < n; ++i) {
    const ResourceSlotPtr& slot = slots_[i];
    ResourcePtr resource(slot->resource());
    if (!(resource->loaded() && resource->HttpStatusOk())) {
      ++outstanding_fetches_;

      // Sometimes we can end up needing pagespeed resources as inputs.
      // This can happen because we are doing a fetch of something produced
      // by chained rewrites, or when handling a 2nd (or further) step of a
      // chain during an HTML rewrite if we don't have the bits inside the
      // resource object (e.g. if we got a metadata hit on the previous step).
      bool handled_internally = false;
      GoogleUrl resource_gurl(resource->url());
      if (FindServerContext()->IsPagespeedResource(resource_gurl)) {
        RewriteDriver* nested_driver = Driver()->Clone();
        RewriteFilter* filter = NULL;
        // We grab the filter now (and not just call DecodeOutputResource
        // earlier instead of IsPagespeedResource) so we get a filter that's
        // bound to the new RewriteDriver.
        OutputResourcePtr output_resource =
            nested_driver->DecodeOutputResource(resource_gurl, &filter);
        if (output_resource.get() != NULL) {
          handled_internally = true;
          slot->SetResource(ResourcePtr(output_resource));
          ResourceReconstructCallback* callback =
              new ResourceReconstructCallback(
                  nested_driver, this, output_resource, i);
          // As a temporary workaround for bugs where FetchOutputResource
          // does not fully sync OutputResource with what it gives the
          // callback, we use FetchResource here and sync to the
          // resource object in the callback.
          nested_driver->FetchResource(resource->url(), callback);
        } else {
          FindServerContext()->ReleaseRewriteDriver(nested_driver);
        }
      }

      if (!handled_internally) {
        Resource::NotCacheablePolicy noncache_policy =
            Resource::kReportFailureIfNotCacheable;
        if (fetch_.get() != NULL) {
          // This is a fetch.  We want to try to get the input resource even if
          // it was previously noted to be uncacheable. Note that this applies
          // only to top-level rewrites: anything nested will still fail.
          DCHECK(!has_parent());
          if (!has_parent()) {
            noncache_policy = Resource::kLoadEvenIfNotCacheable;
          }
        }
        FindServerContext()->ReadAsync(
            noncache_policy, Driver()->request_context(),
            new ResourceFetchCallback(this, resource, i));
      }
    }
  }

  --num_predecessors_;
  Activate();  // TODO(jmarantz): remove.
}

void RewriteContext::ResourceFetchDone(
    bool success, ResourcePtr resource, int slot_index) {
  CHECK_LT(0, outstanding_fetches_);
  --outstanding_fetches_;

  if (success) {
    ResourceSlotPtr slot(slots_[slot_index]);

    // For now, we cannot handle if someone updated our slot before us.
    DCHECK(slot.get() != NULL);
    DCHECK_EQ(resource.get(), slot->resource().get());
  }
  Activate();
}

void RewriteContext::ResourceRevalidateDone(InputInfo* input_info,
                                            bool success) {
  bool ok = false;
  if (success) {
    ResourcePtr resource = slots_[input_info->index()]->resource();
    if (resource->IsValidAndCacheable()) {
      // The reason we check IsValidAndCacheable here is in case someone
      // added a Vary: header without changing the file itself.
      ok = (resource->ContentsHash() == input_info->input_content_hash());

      // Patch up the input_info with the latest cache information on resource.
      resource->FillInPartitionInputInfo(
          Resource::kIncludeInputHash, input_info);
    }
  }

  revalidate_ok_ = revalidate_ok_ && ok;
  --outstanding_fetches_;
  if (outstanding_fetches_ == 0) {
    if (revalidate_ok_) {
      OutputCacheHit(true /* update the cache with new timestamps*/);
    } else {
      OutputCacheMiss();
    }
  }
}

bool RewriteContext::ReadyToRewrite() const {
  DCHECK(!rewrite_done_);
  bool ready = ((outstanding_fetches_ == 0) && (num_predecessors_ == 0));
  return ready;
}

void RewriteContext::Activate() {
  if (ReadyToRewrite()) {
    if (fetch_.get() == NULL) {
      DCHECK(started_);
      StartRewriteForHtml();
    } else {
      StartRewriteForFetch();
    }
  }
}

void RewriteContext::StartRewriteForHtml() {
  CHECK(has_parent() || slow_) << "slow_ not set on a rewriting job?";
  PartitionAsync(partitions_.get(), &outputs_);
}

void RewriteContext::PartitionDone(bool result) {
  if (!result) {
    partitions_->clear_partition();
    outputs_.clear();
  }

  outstanding_rewrites_ = partitions_->partition_size();
  if (outstanding_rewrites_ == 0) {
    DCHECK(fetch_.get() == NULL);
    // The partitioning succeeded, but yielded zero rewrites.  Write out the
    // empty partition table and let any successor Rewrites run.
    rewrite_done_ = true;

    // TODO(morlovich): The filters really should be doing this themselves,
    // since there may be partial failures in cases of multiple inputs which
    // we do not see here.
    AddRecheckDependency();
    FinalizeRewriteForHtml();
  } else {
    // We will let the Rewrites complete prior to writing the
    // OutputPartitions, which contain not just the partition table
    // but the content-hashes for the rewritten content.  So we must
    // rewrite before calling WritePartition.

    // Note that we run the actual rewrites in the "low priority" thread,
    // which makes it easy to cancel them if our backlog gets too horrid.
    //
    // This path corresponds either to HTML rewriting or to a rewrite nested
    // inside a fetch (top-levels for fetches are handled inside
    // StartRewriteForFetch), so failing it due to load-shedding will not
    // prevent us from serving requests.
    CHECK_EQ(outstanding_rewrites_, static_cast<int>(outputs_.size()));
    for (int i = 0, n = outstanding_rewrites_; i < n; ++i) {
      InvokeRewriteFunction* invoke_rewrite =
          new InvokeRewriteFunction(this, i, outputs_[i]);
      Driver()->AddLowPriorityRewriteTask(invoke_rewrite);
    }
  }
}

void RewriteContext::WritePartition() {
  ServerContext* manager = FindServerContext();
  if (ok_to_write_output_partitions_ && !manager->shutting_down()) {
    CacheInterface* metadata_cache = manager->metadata_cache();
    GoogleString buf;
    {
#ifndef NDEBUG
      for (int i = 0, n = partitions_->partition_size(); i < n; ++i) {
        const CachedResult& partition = partitions_->partition(i);
        if (partition.optimizable() && !partition.has_inlined_data()) {
          GoogleUrl gurl(partition.url());
          DCHECK(gurl.is_valid()) << partition.url();
        }
      }
#endif

      StringOutputStream sstream(&buf);  // finalizes buf in destructor
      partitions_->SerializeToZeroCopyStream(&sstream);
    }
    metadata_cache->PutSwappingString(partition_key_, &buf);
  } else {
    // TODO(jmarantz): if our rewrite failed due to lock contention or
    // being too busy, then cancel all successors.
  }
  lock_.reset();
}

void RewriteContext::FinalizeRewriteForHtml() {
  DCHECK(fetch_.get() == NULL);

  bool partition_ok = (partitions_->partition_size() != 0);
  // Tells each of the repeated rewrites of the same thing if we have a valid
  // result or not.
  for (int c = 0, n = repeated_.size(); c < n; ++c) {
    if (partition_ok) {
      repeated_[c]->RepeatedSuccess(this);
    } else {
      repeated_[c]->RepeatedFailure();
    }
  }
  Driver()->DeregisterForPartitionKey(partition_key_, this);
  WritePartition();

  RetireRewriteForHtml(true /* permit rendering, if attached */);
}

void RewriteContext::RetireRewriteForHtml(bool permit_render) {
  if (parent_ != NULL) {
    DCHECK(driver_ == NULL);
    Propagate(permit_render);
    parent_->NestedRewriteDone(this);
  } else {
    // The RewriteDriver is waiting for this to complete.  Defer to the
    // RewriteDriver to schedule the Rendering of this context on the main
    // thread.
    CHECK(driver_ != NULL);
    driver_->RewriteComplete(this, permit_render);
  }
}

void RewriteContext::AddNestedContext(RewriteContext* context) {
  ++num_pending_nested_;
  nested_.push_back(context);
  context->parent_ = this;
}

void RewriteContext::StartNestedTasks() {
  // StartNestedTasks() can be called from the filter, potentially from
  // a low-priority thread, but we want to run Start() in high-priority
  // thread as some of the work it does needs to be serialized with respect
  // to other tasks in that thread.
  Driver()->AddRewriteTask(
      MakeFunction(this, &RewriteContext::StartNestedTasksImpl));
}

void RewriteContext::StartNestedTasksImpl() {
  for (int i = 0, n = nested_.size(); i < n; ++i) {
    if (!nested_[i]->chained()) {
      nested_[i]->Start();
      DCHECK_EQ(n, static_cast<int>(nested_.size()))
          << "Cannot add new nested tasks once the nested tasks have started";
    }
  }
}

void RewriteContext::NestedRewriteDone(const RewriteContext* context) {
  // Record any external dependencies we have.
  // TODO(morlovich): Eliminate duplicates?
  for (int p = 0; p < context->num_output_partitions(); ++p) {
    const CachedResult* nested_result = context->output_partition(p);
    for (int i = 0; i < nested_result->input_size(); ++i) {
      InputInfo* dep = partitions_->add_other_dependency();
      dep->CopyFrom(nested_result->input(i));
      // The input index here is with respect to the nested context's inputs,
      // so would not be interpretable at top-level, and we don't use it for
      // other_dependency entries anyway, so be both defensive and frugal
      // and don't write it out.
      dep->clear_index();
    }
  }

  for (int p = 0; p < context->partitions_->other_dependency_size(); ++p) {
    InputInfo* dep = partitions_->add_other_dependency();
    dep->CopyFrom(context->partitions_->other_dependency(p));
  }

  if (context->was_too_busy_) {
    MarkTooBusy();
  }

  DCHECK_LT(0, num_pending_nested_);
  --num_pending_nested_;
  if (num_pending_nested_ == 0) {
    DCHECK(!rewrite_done_);
    Harvest();
  }
}

void RewriteContext::RewriteDone(RewriteResult result, int partition_index) {
  // RewriteDone may be called from a low-priority rewrites thread.
  // Make sure the rest of the work happens in the high priority rewrite thread.
  Driver()->AddRewriteTask(
      MakeFunction(this, &RewriteContext::RewriteDoneImpl,
                   result, partition_index));
}

void RewriteContext::RewriteDoneImpl(RewriteResult result,
                                     int partition_index) {
  if (result == kTooBusy) {
    MarkTooBusy();
  } else {
    CachedResult* partition =
        partitions_->mutable_partition(partition_index);
    bool optimizable = (result == kRewriteOk);

    // Persist disable_further_processing bits from slots in the corresponding
    // InputInfo entries in metadata cache.
    for (int i = 0; i < partition->input_size(); ++i) {
      InputInfo* input = partition->mutable_input(i);
      if (!input->has_index()) {
        LOG(DFATAL) << "No index on InputInfo. Huh?";
      } else {
        if (slot(input->index())->disable_further_processing()) {
          input->set_disable_further_processing(true);
        }
      }
    }

    partition->set_optimizable(optimizable);
    if (optimizable && (fetch_.get() == NULL)) {
      // TODO(morlovich): currently in async mode, we tie rendering of slot
      // to the optimizable bit, making it impossible to do per-slot mutation
      // that doesn't involve the output URL.
      RenderPartitionOnDetach(partition_index);
    }
  }
  --outstanding_rewrites_;
  if (outstanding_rewrites_ == 0) {
    if (fetch_.get() != NULL) {
      fetch_->set_success((result == kRewriteOk));
    }
    Finalize();
  }
}

void RewriteContext::Harvest() {
}

void RewriteContext::Render() {
}

void RewriteContext::Propagate(bool render_slots) {
  DCHECK(rewrite_done_ && (num_pending_nested_ == 0));
  if (rewrite_done_ && (num_pending_nested_ == 0)) {
    if (render_slots) {
      Render();
    }
    CHECK_EQ(num_output_partitions(), static_cast<int>(outputs_.size()));
    for (int p = 0, np = num_output_partitions(); p < np; ++p) {
      CachedResult* partition = output_partition(p);
      for (int i = 0, n = partition->input_size(); i < n; ++i) {
        int slot_index = partition->input(i).index();
        if (render_slots_[slot_index]) {
          ResourcePtr resource(outputs_[p]);
          slots_[slot_index]->SetResource(resource);
          if (render_slots && partition->url_relocatable()) {
            // This check for relocatable is potentially unsafe in that later
            // filters might still try to relocate the resource.  We deal with
            // this for the current case of javscript by having checks in each
            // potential later filter (combine and inline) that duplicate the
            // logic that went into setting url_relocatable on the partition.
            slots_[slot_index]->Render();
          }
        }
      }
    }
  }

  if (successors_.empty()) {
    for (int i = 0, n = slots_.size(); i < n; ++i) {
      slots_[i]->Finished();
    }
  }

  RunSuccessors();
}

void RewriteContext::Finalize() {
  rewrite_done_ = true;
  DCHECK_EQ(0, num_pending_nested_);
  if (fetch_.get() != NULL) {
    fetch_->FetchDone();
  } else {
    FinalizeRewriteForHtml();
  }
}

void RewriteContext::RenderPartitionOnDetach(int rewrite_index) {
  CachedResult* partition = output_partition(rewrite_index);
  for (int i = 0; i < partition->input_size(); ++i) {
    int slot_index = partition->input(i).index();
    slot(slot_index)->set_was_optimized(true);
    render_slots_[slot_index] = true;
  }
}

void RewriteContext::DetachSlots() {
  for (int i = 0, n = slots_.size(); i < n; ++i) {
    slot(i)->DetachContext(this);
  }
}

void RewriteContext::RunSuccessors() {
  DetachSlots();

  for (int i = 0, n = successors_.size(); i < n; ++i) {
    RewriteContext* successor = successors_[i];
    if (--successor->num_predecessors_ == 0) {
      successor->Initiate();
    }
  }
  successors_.clear();
  if (driver_ != NULL) {
    DCHECK(rewrite_done_ && (num_pending_nested_ == 0));
    Driver()->AddRewriteTask(
        new MemberFunction1<RewriteDriver, RewriteContext*>(
            &RewriteDriver::DeleteRewriteContext, driver_, this));
  }
}

void RewriteContext::StartRewriteForFetch() {
  // Make a fake partition that has all the inputs, since we are
  // performing the rewrite for only one output resource.
  CachedResult* partition = partitions_->add_partition();
  bool ok_to_rewrite = true;
  for (int i = 0, n = slots_.size(); i < n; ++i) {
    ResourcePtr resource(slot(i)->resource());
    if (resource->loaded() && resource->HttpStatusOk() &&
        !resource->response_headers()->HasValue(HttpAttributes::kCacheControl,
                                                "no-transform")) {
      bool on_the_fly = (kind() == kOnTheFlyResource);
      Resource::HashHint hash_hint = on_the_fly ?
          Resource::kOmitInputHash : Resource::kIncludeInputHash;
      resource->AddInputInfoToPartition(hash_hint, i, partition);
    } else {
      ok_to_rewrite = false;
      break;
    }
  }
  OutputResourcePtr output(fetch_->output_resource());

  // During normal rewrite path, Partition() is responsible for syncing up
  // the output resource's CachedResult and the partition tables. As it does
  // not get run for fetches, we take care of the syncing here.
  output->set_cached_result(partition);
  ++outstanding_rewrites_;
  if (ok_to_rewrite) {
    // To avoid rewrites from delaying fetches, we try to fallback
    // to the original version if rewriting takes too long.
    if (CanFetchFallbackToOriginal(kFallbackDiscretional)) {
      fetch_->SetupDeadlineAlarm();
    }

    // Generally, we want to do all rewriting in the low-priority thread,
    // to ensure the main rewrite thread is always responsive. However, the
    // low-priority thread's tasks may get cancelled due to load-shedding,
    // so we have to be careful not to do it for filters where falling back
    // to an input isn't an option (such as combining filters or filters that
    // set OptimizationOnly() to false).
    InvokeRewriteFunction* call_rewrite =
        new InvokeRewriteFunction(this, 0, output);
    if (CanFetchFallbackToOriginal(kFallbackDiscretional)) {
      Driver()->AddLowPriorityRewriteTask(call_rewrite);
    } else {
      Driver()->AddRewriteTask(call_rewrite);
    }
  } else {
    partition->clear_input();
    AddRecheckDependency();
    RewriteDone(kRewriteFailed, 0);
  }
}

void RewriteContext::MarkSlow() {
  if (has_parent()) {
    return;
  }

  ContextSet to_detach;
  CollectDependentTopLevel(&to_detach);

  int num_new_slow = 0;
  for (ContextSet::iterator i = to_detach.begin();
        i != to_detach.end(); ++i) {
    RewriteContext* c = *i;
    if (!c->slow_) {
      c->slow_ = true;
      ++num_new_slow;
    }
  }

  if (num_new_slow != 0) {
    Driver()->ReportSlowRewrites(num_new_slow);
  }
}

void RewriteContext::MarkTooBusy() {
  ok_to_write_output_partitions_ = false;
  was_too_busy_ = true;
}

void RewriteContext::CollectDependentTopLevel(ContextSet* contexts) {
  std::pair<ContextSet::iterator, bool> insert_result = contexts->insert(this);
  if (!insert_result.second) {
    // We were already there.
    return;
  }

  for (int c = 0, n = successors_.size(); c < n; ++c) {
    if (!successors_[c]->has_parent()) {
      successors_[c]->CollectDependentTopLevel(contexts);
    }
  }

  for (int c = 0, n = repeated_.size(); c < n; ++c) {
    if (!repeated_[c]->has_parent()) {
      repeated_[c]->CollectDependentTopLevel(contexts);
    }
  }
}

bool RewriteContext::CreateOutputResourceForCachedOutput(
    const CachedResult* cached_result,
    OutputResourcePtr* output_resource) {
  bool ret = false;
  GoogleUrl gurl(cached_result->url());
  const ContentType* content_type =
      NameExtensionToContentType(StrCat(".", cached_result->extension()));

  ResourceNamer namer;
  if (gurl.is_valid() && namer.Decode(gurl.LeafWithQuery())) {
    output_resource->reset(
        new OutputResource(FindServerContext(),
                           gurl.AllExceptLeaf() /* resolved_base */,
                           gurl.AllExceptLeaf() /* unmapped_base */,
                           Driver()->base_url().Origin() /* original_base */,
                           namer, Options(), kind()));
    // We trust the type here since we should have gotten it right when
    // writing it into the cache.
    (*output_resource)->SetType(content_type);
    ret = true;
  }
  return ret;
}

bool RewriteContext::Partition(OutputPartitions* partitions,
                               OutputResourceVector* outputs) {
  LOG(FATAL) << "RewriteContext subclasses must reimplement one of "
                "PartitionAsync or Partition";
  return false;
}

void RewriteContext::PartitionAsync(OutputPartitions* partitions,
                                    OutputResourceVector* outputs) {
  PartitionDone(Partition(partitions, outputs));
}

void RewriteContext::CrossThreadPartitionDone(bool result) {
  Driver()->AddRewriteTask(
      MakeFunction(this, &RewriteContext::PartitionDone, result));
}

void RewriteContext::Freshen() {
  // Note: only CACHED inputs are freshened (not FILE_BASED or ALWAYS_VALID).
  FreshenMetadataUpdateManager* freshen_manager =
      new FreshenMetadataUpdateManager(
          partition_key_, FindServerContext()->metadata_cache(),
          FindServerContext()->thread_system()->NewMutex());
  for (int j = 0, n = partitions_->partition_size(); j < n; ++j) {
    const CachedResult& partition = partitions_->partition(j);
    for (int i = 0, m = partition.input_size(); i < m; ++i) {
      const InputInfo& input_info = partition.input(i);
      if (stale_rewrite_ ||
          ((input_info.type() == InputInfo::CACHED) &&
           input_info.has_expiration_time_ms() &&
           input_info.has_date_ms() &&
           input_info.has_index())) {
        ResourcePtr resource(slots_[input_info.index()]->resource());
        if (stale_rewrite_|| FindServerContext()->IsImminentlyExpiring(
            input_info.date_ms(), input_info.expiration_time_ms())) {
          RewriteFreshenCallback* callback = NULL;
          if (input_info.has_input_content_hash()) {
            callback = new RewriteFreshenCallback(
                resource, j, i, freshen_manager);
            freshen_manager->IncrementFreshens(*partitions_.get());
          }
          // TODO(nikhilmadan): We don't actually update the metadata when the
          // InputInfo does not contain an input_content_hash. However, we still
          // re-fetch the original resource and update the HTTPCache.
          resource->Freshen(callback, FindServerContext()->message_handler());
        }
      }
    }
  }
  freshen_manager->MarkAllFreshensTriggered();
}

const UrlSegmentEncoder* RewriteContext::encoder() const {
  return &default_encoder_;
}

GoogleString RewriteContext::CacheKeySuffix() const {
  return "";
}

bool RewriteContext::DecodeFetchUrls(
    const OutputResourcePtr& output_resource,
    MessageHandler* message_handler,
    GoogleUrlStarVector* url_vector) {
  GoogleUrl original_base(output_resource->url());
  GoogleUrl decoded_base(output_resource->decoded_base());
  StringPiece original_base_sans_leaf(original_base.AllExceptLeaf());
  bool check_for_multiple_rewrites =
      (original_base_sans_leaf != decoded_base.AllExceptLeaf());
  StringVector urls;
  if (encoder()->Decode(output_resource->name(), &urls, resource_context_.get(),
                        message_handler)) {
    if (check_for_multiple_rewrites) {
      // We want to drop the leaf from the base URL before combining it
      // with the decoded name, in case the decoded name turns into a
      // query. (Since otherwise we would end up with http://base/,qfoo?foo
      // rather than http://base?foo).
      original_base.Reset(original_base_sans_leaf);
    }

    for (int i = 0, n = urls.size(); i < n; ++i) {
      // If the decoded name is still encoded (because originally it was
      // rewritten by multiple filters, such as CSS minified then combined),
      // keep the un-decoded base, otherwise use the decoded base.
      // For example, this encoded URL:
      //   http://cdn.com/my.com/I.a.css.pagespeed.cf.0.css
      // needs will be decoded to http://my.com/a.css so we need to use the
      // decoded domain here. But this encoded URL:
      //   http://cdn.com/my.com/I.a.css+b.css,Mcc.0.css.pagespeed.cf.0.css
      // needs will be decoded first to:
      //   http://cdn.com/my.com/I.a.css+b.css,pagespeed.cc.0.css
      // which will then be decoded to http://my.com/a.css and b.css so for the
      // first decoding here we need to retain the encoded domain name.
      GoogleUrl* url = NULL;
      ResourceNamer namer;

      if (check_for_multiple_rewrites) {
        scoped_ptr<GoogleUrl> orig_based_url(
            new GoogleUrl(original_base, urls[i]));
        if (FindServerContext()->IsPagespeedResource(*orig_based_url.get())) {
          url = orig_based_url.release();
        }
      }

      if (url == NULL) {  // Didn't set one based on original_base
        url = new GoogleUrl(decoded_base, urls[i]);
      }
      url_vector->push_back(url);
    }
    return true;
  }
  return false;
}

bool RewriteContext::Fetch(
    const OutputResourcePtr& output_resource,
    AsyncFetch* fetch,
    MessageHandler* message_handler) {
  // Decode the URLs required to execute the rewrite.
  bool ret = false;
  RewriteDriver* driver = Driver();
  driver->InitiateFetch(this);
  GoogleUrlStarVector url_vector;
  if (DecodeFetchUrls(output_resource, message_handler, &url_vector)) {
    bool is_valid = true;
    for (int i = 0, n = url_vector.size(); i < n; ++i) {
      GoogleUrl* url = url_vector[i];
      if (!url->is_valid()) {
        is_valid = false;
        break;
      }

      if (!FindServerContext()->url_namer()->ProxyMode() &&
          !driver->MatchesBaseUrl(*url)) {
        // Reject absolute url references unless we're proxying.
        is_valid = false;
        message_handler->Message(kError, "Rejected absolute url reference %s",
                                 url->spec_c_str());
        break;
      }

      ResourcePtr resource(driver->CreateInputResource(*url));
      if (resource.get() == NULL) {
        // TODO(jmarantz): bump invalid-input-resource count
         is_valid = false;
         break;
      }
      resource->set_is_background_fetch(false);
      ResourceSlotPtr slot(new FetchResourceSlot(resource));
      AddSlot(slot);
    }
    STLDeleteContainerPointers(url_vector.begin(), url_vector.end());
    if (is_valid) {
      SetPartitionKey();
      fetch_.reset(
          new FetchContext(this, fetch, output_resource, message_handler));
      if (output_resource->has_hash()) {
        fetch_->set_requested_hash(output_resource->hash());
      }
      Driver()->AddRewriteTask(MakeFunction(this,
                                            &RewriteContext::StartFetch,
                                            &RewriteContext::CancelFetch));
      ret = true;
    }
  }
  if (!ret) {
    fetch->response_headers()->SetStatusAndReason(HttpStatus::kNotFound);
  }

  return ret;
}

void RewriteContext::CancelFetch() {
  AsyncFetch* fetch = fetch_->async_fetch();
    fetch->response_headers()->SetStatusAndReason(
        HttpStatus::kInternalServerError  /* 500 */);
  FetchCallbackDone(false);
}

void RewriteContext::FetchCacheDone(CacheLookupResult* cache_result) {
  // If we have metadata during a resource fetch, we see if we can use it
  // to find a pre-existing result in HTTP cache we can serve. This is done
  // by sanity-checking the metadata here, then doing an async cache lookup via
  // FetchTryFallback, which in turn calls FetchFallbackCacheDone.
  // If we're successful at that point FetchContext::FetchFallbackDone
  // serves out the bits with a shortened TTL; if we fail at any point
  // we call StartFetchReconstruction which will invoke the normal process of
  // locking things, fetching inputs, rewriting, and so on.

  scoped_ptr<CacheLookupResult> owned_cache_result(cache_result);
  partitions_.reset(owned_cache_result->partitions.release());
  LogMetadataCacheInfo(owned_cache_result->cache_ok,
                       owned_cache_result->can_revalidate);

  if (owned_cache_result->cache_ok && (num_output_partitions() == 1)) {
    CachedResult* result = output_partition(0);
    OutputResourcePtr output_resource;
    if (result->optimizable() &&
        CreateOutputResourceForCachedOutput(result, &output_resource)) {
      if (fetch_->requested_hash() != output_resource->hash()) {
        // Try to do a cache look up on the proper hash; if it's available,
        // we can serve it.
        FetchTryFallback(output_resource->HttpCacheKey(),
                         output_resource->hash());
        return;
      }
    } else if (CanFetchFallbackToOriginal(kFallbackDiscretional)) {
      // The result is not optimizable, and it makes sense to use
      // the original instead, so try to do that.
      // (For simplicity, we will do an another rewrite attempt if it's not in
      // the cache).
      FetchTryFallback(slot(0)->resource()->url(), "");
      return;
    }
  }

  // Didn't figure out anything clever; so just rewrite on demand.
  StartFetchReconstruction();
}

void RewriteContext::FetchTryFallback(const GoogleString& url,
                                      const StringPiece& hash) {
  FindServerContext()->http_cache()->Find(
      url,
      FindServerContext()->message_handler(),
      new HTTPCacheCallback(
          this, &RewriteContext::FetchFallbackCacheDone));
}

void RewriteContext::FetchFallbackCacheDone(HTTPCache::FindResult result,
                                            HTTPCache::Callback* data) {
  scoped_ptr<HTTPCache::Callback> cleanup_callback(data);

  StringPiece contents;
  if ((result == HTTPCache::kFound) &&
      data->http_value()->ExtractContents(&contents) &&
      (data->response_headers()->status_code() == HttpStatus::kOK)) {
    // We want to serve the found result, with short cache lifetime.
    fetch_->FetchFallbackDone(contents, data->response_headers());
  } else {
    StartFetchReconstruction();
  }
}

void RewriteContext::FetchCallbackDone(bool success) {
  RewriteDriver* notify_driver =
      notify_driver_on_fetch_done_ ? Driver() : NULL;
  async_fetch()->Done(success);  // deletes this.
  if (notify_driver != NULL) {
    notify_driver->FetchComplete();
  }
}

bool RewriteContext::CanFetchFallbackToOriginal(
    FallbackCondition condition) const {
  if (!OptimizationOnly() && (condition != kFallbackEmergency)) {
    // If the filter is non-discretionary we will run it unless it already
    // failed and we would rather serve -something-.
    return false;
  }
  // We can serve the original (well, perhaps with some absolutification) in
  // cases where there is a single input.
  return (num_slots() == 1);
}

void RewriteContext::StartFetch() {
  DCHECK_EQ(kind(), fetch_->output_resource()->kind());
  // If we have an on-the-fly resource, we almost always want to reconstruct it
  // --- there will be no shortcuts in the metadata cache unless the rewrite
  // fails, and it's ultra-cheap to reconstruct anyway.
  if (kind() == kOnTheFlyResource) {
    StartFetchReconstruction();
  } else {
    // Try to lookup metadata, as it may mark the result as non-optimizable
    // or point us to the right hash.
    FindServerContext()->metadata_cache()->Get(
        partition_key_,
        new OutputCacheCallback(this, &RewriteContext::FetchCacheDone));
  }
}

void RewriteContext::StartFetchReconstruction() {
  // Note that in case of fetches we continue even if we didn't manage to
  // take the lock.
  partitions_->Clear();
  FindServerContext()->LockForCreation(
      Lock(), Driver()->rewrite_worker(),
      MakeFunction(this, &RewriteContext::FetchInputs,
                   &RewriteContext::FetchInputs));
}

void RewriteContext::DetachFetch() {
  CHECK(fetch_.get() != NULL);
  fetch_->set_detached(true);
  Driver()->DetachFetch();
}

RewriteDriver* RewriteContext::Driver() const {
  const RewriteContext* rc;
  for (rc = this; rc->driver_ == NULL; rc = rc->parent_) {
    CHECK(rc != NULL);
  }
  return rc->driver_;
}

ServerContext* RewriteContext::FindServerContext() const {
  return Driver()->server_context();
}

const RewriteOptions* RewriteContext::Options() const {
  return Driver()->options();
}

void RewriteContext::FixFetchFallbackHeaders(ResponseHeaders* headers) {
  if (headers->Sanitize()) {
    headers->ComputeCaching();
  }

  // Shorten cache length, and prevent proxies caching this, as it's under
  // the "wrong" URL.
  headers->SetDateAndCaching(
      headers->date_ms(),
      std::min(headers->cache_ttl_ms(), ResponseHeaders::kImplicitCacheTtlMs),
      ",private");
  headers->ComputeCaching();
}

bool RewriteContext::FetchContextDetached() {
  DCHECK(fetch_.get() != NULL);
  return fetch_->detached();
}

bool RewriteContext::AbsolutifyIfNeeded(const StringPiece& input_contents,
                                        Writer* writer,
                                        MessageHandler* handler) {
  return writer->Write(input_contents, handler);
}

AsyncFetch* RewriteContext::async_fetch() {
  DCHECK(fetch_.get() != NULL);
  return fetch_->async_fetch();
}

MessageHandler* RewriteContext::fetch_message_handler() {
  DCHECK(fetch_.get() != NULL);
  return fetch_->handler();
}

}  // namespace net_instaweb
