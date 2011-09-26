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
#include "base/scoped_ptr.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/blocking_behavior.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_namer.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_single_resource_filter.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/named_lock_manager.h"
#include "net/instaweb/util/public/null_writer.h"
#include "net/instaweb/util/public/proto_util.h"
#include "net/instaweb/util/public/shared_string.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/stl_util.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/public/url_segment_encoder.h"
#include "net/instaweb/util/public/writer.h"

namespace net_instaweb {

class RewriteFilter;

const char kRewriteContextLockPrefix[] = "rc:";

// Two callback classes for completed caches & fetches.  These gaskets
// help RewriteContext, which knows about all the pending inputs,
// trigger the rewrite once the data is available.  There are two
// versions of the callback.

// Callback to wake up the RewriteContext when the partitioning is looked up
// in the cache.  The RewriteContext can then decide whether to queue the
// output-resource for a DOM update, or re-initiate the Rewrite, depending
// on the metadata returned.
class RewriteContext::OutputCacheCallback : public CacheInterface::Callback {
 public:
  explicit OutputCacheCallback(RewriteContext* rc) : rewrite_context_(rc) {}
  virtual ~OutputCacheCallback() {}
  virtual void Done(CacheInterface::KeyState state) {
    RewriteDriver* rewrite_driver = rewrite_context_->Driver();
    rewrite_driver->AddRewriteTask(MakeFunction(
        rewrite_context_, &RewriteContext::OutputCacheDone, state, *value()));
    delete this;
  }

 private:
  RewriteContext* rewrite_context_;
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
  virtual void Done(bool success) {
    delegate_.Done(success);
    delete this;
  }

  virtual bool EnableThreaded() const { return true; }

 private:
  ResourceCallbackUtils delegate_;
};

// Callback used when we need to reconstruct a resource we made to satisfy
// a fetch (due to rewrites being nested inside each other).
class RewriteContext::ResourceReconstructCallback
    : public UrlAsyncFetcher::Callback {
 public:
  // Takes ownership of the driver (e.g. will call Cleanup)
  ResourceReconstructCallback(RewriteDriver* driver, RewriteContext* rc,
                              const OutputResourcePtr& resource, int slot_index)
      : driver_(driver),
        delegate_(rc, ResourcePtr(resource), slot_index),
        resource_(resource) {
  }

  virtual ~ResourceReconstructCallback() {
  }

  virtual void Done(bool success) {
    // Make sure to release the lock here, as in case of nested reconstructions
    // that fail it would otherwise only get released on ~OutputResource, which
    // in turn will only happen once the top-level is done, which may take a
    // while.
    resource_->DropCreationLock();

    delegate_.Done(success);
    driver_->Cleanup();
    delete this;
  }

  const RequestHeaders& request_headers() const { return request_headers_; }
  ResponseHeaders* response_headers() { return &response_headers_; }
  Writer* writer() { return &writer_; }

 private:
  RewriteDriver* driver_;
  ResourceCallbackUtils delegate_;
  OutputResourcePtr resource_;

  // We ignore the output here as it's also put into the resource itself.
  NullWriter writer_;
  ResponseHeaders response_headers_;
  RequestHeaders request_headers_;
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

  virtual void Done(bool success) {
    RewriteDriver* rewrite_driver = rewrite_context_->Driver();
    rewrite_driver->AddRewriteTask(
        MakeFunction(rewrite_context_, &RewriteContext::ResourceRevalidateDone,
                     input_info_, success));
    delete this;
  }

  virtual bool EnableThreaded() const { return true; }

 private:
  RewriteContext* rewrite_context_;
  InputInfo* input_info_;
};

// This class encodes a few data members used for responding to
// resource-requests when the output_resource is not in cache.
class RewriteContext::FetchContext {
 public:
  FetchContext(RewriteContext* rewrite_context,
               Writer* writer,
               ResponseHeaders* response_headers,
               UrlAsyncFetcher::Callback* callback,
               const OutputResourcePtr& output_resource,
               MessageHandler* handler)
      : rewrite_context_(rewrite_context),
        writer_(writer),
        response_headers_(response_headers),
        callback_(callback),
        output_resource_(output_resource),
        handler_(handler),
        success_(false) {
  }

  // Note that the callback is called from the RewriteThread.
  void FetchDone() {
    GoogleString output;
    bool ok = false;
    if (success_) {
      // TODO(sligocki): It might be worth streaming this.
      response_headers_->CopyFrom(*(output_resource_->response_headers()));
      ok = writer_->Write(output_resource_->contents(), handler_);
    } else {
      // TODO(jmarantz): implement this:
      // CacheRewriteFailure();

      // Rewrite failed. If we have a single original, write it out instead.
      if (rewrite_context_->num_slots() == 1) {
        ResourcePtr input_resource(rewrite_context_->slot(0)->resource());
        if (input_resource.get() != NULL && input_resource->ContentsValid()) {
          response_headers_->CopyFrom(*input_resource->response_headers());
          ok = writer_->Write(input_resource->contents(), handler_);
        } else {
          GoogleString url = input_resource.get()->url();
          handler_->Error(
              output_resource_->name().as_string().c_str(), 0,
              "Resource based on %s but cannot access the original",
              url.c_str());
        }
      }
    }

    callback_->Done(ok);
  }

  void set_success(bool success) { success_ = success; }
  OutputResourcePtr output_resource() { return output_resource_; }

  RewriteContext* rewrite_context_;
  Writer* writer_;
  ResponseHeaders* response_headers_;
  UrlAsyncFetcher::Callback* callback_;
  OutputResourcePtr output_resource_;
  MessageHandler* handler_;
  bool success_;
};

// Helper for running filter's Rewrite method in low-priority rewrite thread,
// which deals with cancellation of rewrites due to load shedding or shutdown by
// introducing a kTooBusy response if the job gets dumped.
class RewriteContext::InvokeRewriteFunction : public Function {
 public:
  InvokeRewriteFunction(RewriteContext* context, int partition)
      : context_(context), partition_(partition) {}

  virtual ~InvokeRewriteFunction() {}

  virtual void Run() {
    context_->Rewrite(partition_,
                      context_->partitions_->mutable_partition(partition_),
                      context_->outputs_[partition_]);
  }

  virtual void Cancel() {
    context_->RewriteDone(RewriteSingleResourceFilter::kTooBusy, partition_);
  }

 private:
  RewriteContext* context_;
  int partition_;
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
    revalidate_ok_(true) {
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
  CacheInterface* metadata_cache = Manager()->metadata_cache();
  SetPartitionKey();

  // See if some other handler already had to do an identical rewrite.
  RewriteContext* previous_handler =
      Driver()->RegisterForPartitionKey(partition_key_, this);
  if (previous_handler == NULL) {
    // When the cache lookup is finished, OutputCacheDone will be called.
    metadata_cache->Get(partition_key_, new OutputCacheCallback(this));
  } else {
    if (previous_handler->slow()) {
      MarkSlow();
    }
    previous_handler->repeated_.push_back(this);
  }
}

void RewriteContext::SetPartitionKey() {
  partition_key_ = CacheKey();
  StrAppend(&partition_key_, ":", id());
}

// Check if this mapping from input to output URLs is still valid; and if not
// if we can re-check based on content.
bool RewriteContext::IsCachedResultValid(CachedResult* partition,
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

bool RewriteContext::IsOtherDependencyValid(
    const OutputPartitions* partitions) {
  for (int j = 0, m = partitions->other_dependency_size(); j < m; ++j) {
    if (!IsInputValid(partitions->other_dependency(j))) {
      return false;
    }
  }
  return true;
}

void RewriteContext::AddRecheckDependency() {
  int64 now_ms = Manager()->timer()->NowMs();
  InputInfo* force_recheck = partitions_->add_other_dependency();
  force_recheck->set_type(InputInfo::CACHED);
  force_recheck->set_expiration_time_ms(
      now_ms + ResponseHeaders::kImplicitCacheTtlMs);
}

bool RewriteContext::IsInputValid(const InputInfo& input_info) {
  switch (input_info.type()) {
    case InputInfo::CACHED: {
      // It is invalid if cacheable inputs have expired or ...
      DCHECK(input_info.has_expiration_time_ms());
      if (!input_info.has_expiration_time_ms()) {
        return false;
      }
      int64 now_ms = Manager()->timer()->NowMs();
      return (now_ms <= input_info.expiration_time_ms());
      break;
    }
    case InputInfo::FILE_BASED: {
      // ... if file-based inputs have changed.
      DCHECK(input_info.has_last_modified_time_ms() &&
             input_info.has_filename());
      if (!input_info.has_last_modified_time_ms() ||
          !input_info.has_filename()) {
        return false;
      }
      int64 mtime_sec;
      Manager()->file_system()->Mtime(input_info.filename(), &mtime_sec,
                                      Manager()->message_handler());
      return (mtime_sec * Timer::kSecondMs ==
                input_info.last_modified_time_ms());
      break;
    }
    case InputInfo::ALWAYS_VALID:
      return true;
  }

  DLOG(FATAL) << "Corrupt InputInfo object !?";
  return false;
}

void RewriteContext::OutputCacheDone(CacheInterface::KeyState state,
                                     SharedString value) {
  DCHECK_LE(0, outstanding_fetches_);
  DCHECK_EQ(static_cast<size_t>(0), outputs_.size());

  bool can_revalidate = true;
  InputInfoStarVector revalidate;

  if (state == CacheInterface::kAvailable) {
    // We've got a hit on the output metadata; the contents should
    // be a protobuf.  Try to parse it.
    const GoogleString* val_str = value.get();
    ArrayInputStream input(val_str->data(), val_str->size());
    if (partitions_->ParseFromZeroCopyStream(&input) &&
        IsOtherDependencyValid(partitions_.get())) {
      // Go through and figure out if the cached results for each partition are
      // valid, and if not it's worth trying to salvage them by re-checking if
      // the resources have -really- changed.
      for (int i = 0, n = partitions_->partition_size(); i < n; ++i) {
        CachedResult* partition = partitions_->mutable_partition(i);
        bool can_revalidate_resource;
        if (!IsCachedResultValid(partition, &can_revalidate_resource,
                                 &revalidate)) {
          state = CacheInterface::kNotFound;
          can_revalidate = can_revalidate && can_revalidate_resource;
        }
      }

      // If OK or worth rechecking, set things up for the cache hit case.
      if ((state == CacheInterface::kAvailable) || can_revalidate) {
        for (int i = 0, n = partitions_->partition_size(); i < n; ++i) {
          const CachedResult& partition = partitions_->partition(i);
          OutputResourcePtr output_resource;
          const ContentType* content_type = NameExtensionToContentType(
              StrCat(".", partition.extension()));

          if (partition.optimizable() &&
              CreateOutputResourceForCachedOutput(
                  partition.url(), content_type, &output_resource)) {
            outputs_.push_back(output_resource);
          } else {
            outputs_.push_back(OutputResourcePtr(NULL));
          }
        }
      }
    } else {
      // This case includes both corrupt protobufs and the case where
      // external dependencies are invalid. We do not attempt to reuse
      // rewrite results by input content hashes even in the second
      // case as that would require us to try to re-fetch those URLs as well.
      can_revalidate = false;
      state = CacheInterface::kNotFound;
      // TODO(jmarantz): count cache corruptions in a stat?
    }
  } else {
    can_revalidate = false;
    Manager()->rewrite_stats()->cached_output_misses()->Add(1);
  }

  // If the cache gave a miss, or yielded unparsable data, then acquire a lock
  // and start fetching the input resources.
  if (state == CacheInterface::kAvailable) {
    OutputCacheHit(false /* no need to write back to cache*/);
  } else {
    MarkSlow();
    if (can_revalidate) {
      OutputCacheRevalidate(revalidate);
    } else {
      OutputCacheMiss();
    }
  }
}

void RewriteContext::OutputCacheHit(bool write_partitions) {
  for (int i = 0, n = partitions_->partition_size(); i < n; ++i) {
    if (outputs_[i].get() != NULL) {
      Freshen(partitions_->partition(i));
      RenderPartitionOnDetach(i);
    }
  }

  ok_to_write_output_partitions_ = write_partitions;
  Finalize();
}

void RewriteContext::OutputCacheMiss() {
  outputs_.clear();
  partitions_->Clear();
  FetchInputs(kNeverBlock);
}

void RewriteContext::OutputCacheRevalidate(
    const InputInfoStarVector& to_revalidate) {
  DCHECK(!to_revalidate.empty());
  outstanding_fetches_ = to_revalidate.size();

  for (int i = 0, n = to_revalidate.size(); i < n; ++i) {
    InputInfo* input_info = to_revalidate[i];
    ResourcePtr resource = slots_[input_info->index()]->resource();
    Manager()->ReadAsync(
        new ResourceRevalidateCallback(this, resource, input_info));
  }
}

void RewriteContext::RepeatedSuccess(const RewriteContext* primary) {
  CHECK(outputs_.empty());
  CHECK_EQ(num_slots(), primary->num_slots());
  // Copy over partition tables, outputs, and render_slot_ (as well as
  // was_optimized) information --- everything we can set in normal
  // OutputCacheDone.
  partitions_->CopyFrom(*primary->partitions_.get());
  for (int i = 0, n = primary->outputs_.size(); i < n; ++i) {
    outputs_.push_back(primary->outputs_[i]);
  }

  for (int i = 0, n = primary->num_slots(); i < n; ++i) {
    slot(i)->set_was_optimized(primary->slot(i)->was_optimized());
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
  WritePartition();
}

void RewriteContext::FetchInputs(BlockingBehavior block) {
  // NOTE: This lock is based on hashes so if you use a MockHasher, you may
  // only rewrite a single resource at a time (e.g. no rewriting resources
  // inside resources, see css_image_rewriter_test.cc for examples.)
  //
  // TODO(jmarantz): In the multi-resource rewriters that can
  // generate more than one partition, we create a lock based on the
  // entire set of input URLs, plus a lock for each individual
  // output.  However, in single-resource rewriters, we really only
  // need one of these locks.  So figure out which one we'll go with
  // and use that.
  if (lock_.get() == NULL) {
    GoogleString lock_name = StrCat(kRewriteContextLockPrefix, partition_key_);
    lock_.reset(Manager()->MakeCreationLock(lock_name));
  }

  Manager()->LockForCreation(block, lock_.get());
  // Note that in case of fetches we continue even if we didn't manage to
  // steal the lock.
  if (lock_->Held() || (block == kMayBlock)) {
    ++num_predecessors_;

    for (int i = 0, n = slots_.size(); i < n; ++i) {
      const ResourceSlotPtr& slot = slots_[i];
      ResourcePtr resource(slot->resource());
      if (!(resource->loaded() && resource->ContentsValid())) {
        ++outstanding_fetches_;

        // In case of fetches, we may need to handle rewrites nested inside
        // each other; so we want to pass them on to other rewrite tasks
        // rather than try to fetch them over HTTP.
        bool handled_internally = false;
        if (fetch_.get() != NULL) {
          GoogleUrl resource_gurl(resource->url());
          if (Manager()->IsPagespeedResource(resource_gurl)) {
            RewriteDriver* nested_driver = Driver()->Clone();
            RewriteFilter* filter = NULL;
            // We grab the filter now (and not just call DecodeOutputResource
            // instead of IsPagespeedResource) so we get a filter that's bound
            // to the new RewriteDriver.
            OutputResourcePtr output_resource =
                nested_driver->DecodeOutputResource(resource_gurl, &filter);
            if (output_resource.get() != NULL) {
              handled_internally = true;
              slot->SetResource(ResourcePtr(output_resource));
              ResourceReconstructCallback* callback =
                  new ResourceReconstructCallback(
                      nested_driver, this, output_resource, i);
              nested_driver->FetchOutputResource(
                  output_resource, filter,
                  callback->request_headers(),
                  callback->response_headers(),
                  callback->writer(),
                  callback);
            } else {
              Manager()->ReleaseRewriteDriver(nested_driver);
            }
          }
        }

        if (!handled_internally) {
          Manager()->ReadAsync(new ResourceFetchCallback(this, resource, i));
        }
      }
    }

    --num_predecessors_;
  } else {
    // TODO(jmarantz): bump stat for abandoned rewrites due to lock
    // contention.
    ok_to_write_output_partitions_ = false;
  }

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
      resource->FillInPartitionInputInfo(input_info);
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
      StartRewrite();
    } else {
      FinishFetch();
    }
  }
}

void RewriteContext::StartRewrite() {
  CHECK(has_parent() || slow_) << "slow_ not set on a rewriting job?";
  if (!Partition(partitions_.get(), &outputs_)) {
    partitions_->clear_partition();
    outputs_.clear();
  }

  outstanding_rewrites_ = partitions_->partition_size();
  if (outstanding_rewrites_ == 0) {
    // The partitioning succeeded, but yielded zero rewrites.  Write out the
    // empty partition table and let any successor Rewrites run.
    rewrite_done_ = true;

    // TODO(morlovich): The filters really should be doing this themselves,
    // since there may be partial failures in cases of multiple inputs which
    // we do not see here.
    AddRecheckDependency();
    WritePartition();
  } else {
    // We will let the Rewrites complete prior to writing the
    // OutputPartitions, which contain not just the partition table
    // but the content-hashes for the rewritten content.  So we must
    // rewrite before calling WritePartition.

    // Note that we run the actual rewrites in the "low priority" thread except
    // if we're serving a fetch, since we do not want to fail it due to
    // load shedding.
    bool is_fetch = (fetch_.get() != NULL) ||
                    ((parent_ != NULL) && (parent_->fetch_.get() != NULL));

    CHECK_EQ(outstanding_rewrites_, static_cast<int>(outputs_.size()));
    for (int i = 0, n = outstanding_rewrites_; i < n; ++i) {
      InvokeRewriteFunction* invoke_rewrite =
          new InvokeRewriteFunction(this, i);
      if (is_fetch) {
        Driver()->AddRewriteTask(invoke_rewrite);
      } else {
        Driver()->AddLowPriorityRewriteTask(invoke_rewrite);
      }
    }
  }
}

void RewriteContext::WritePartition() {
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

  ResourceManager* manager = Manager();
  if (ok_to_write_output_partitions_ &&
      !manager->metadata_cache_readonly()) {
    CacheInterface* metadata_cache = manager->metadata_cache();
    SharedString buf;
    {
      StringOutputStream sstream(buf.get());
      partitions_->SerializeToZeroCopyStream(&sstream);
      // destructor of sstream prepares *buf.get()
    }
    metadata_cache->Put(partition_key_, &buf);
  } else {
    // TODO(jmarantz): if our rewrite failed due to lock contention or
    // being too busy, then cancel all successors.
  }
  lock_.reset();
  if (parent_ != NULL) {
    DCHECK(driver_ == NULL);
    Propagate(true);
    parent_->NestedRewriteDone(this);
  } else {
    // The RewriteDriver is waiting for this to complete.  Defer to the
    // RewriteDriver to schedule the Rendering of this context on the main
    // thread.
    CHECK(driver_ != NULL);
    driver_->RewriteComplete(this);
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

void RewriteContext::RewriteDone(
    RewriteSingleResourceFilter::RewriteResult result,
    int partition_index) {
  // RewriteDone may be called from a low-priority rewrites thread.
  // Make sure the rest of the work happens in the high priority rewrite thread.
  Driver()->AddRewriteTask(
      MakeFunction(this, &RewriteContext::RewriteDoneImpl,
                   result, partition_index));
}

void RewriteContext::RewriteDoneImpl(
    RewriteSingleResourceFilter::RewriteResult result,
    int partition_index) {
  if (result == RewriteSingleResourceFilter::kTooBusy) {
    MarkTooBusy();
  } else {
    CachedResult* partition =
        partitions_->mutable_partition(partition_index);
    bool optimizable = (result == RewriteSingleResourceFilter::kRewriteOk);
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
      fetch_->set_success((result == RewriteSingleResourceFilter::kRewriteOk));
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
          if (render_slots) {
            slots_[slot_index]->Render();
          }
        }
      }
    }
  }

  RunSuccessors();
}

void RewriteContext::Finalize() {
  rewrite_done_ = true;
  if (num_pending_nested_ == 0) {
    if (fetch_.get() != NULL) {
      fetch_->FetchDone();
    } else {
      WritePartition();
    }
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

void RewriteContext::RunSuccessors() {
  for (int i = 0, n = slots_.size(); i < n; ++i) {
    slot(i)->DetachContext(this);
  }

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

void RewriteContext::FinishFetch() {
  // Make a fake partition that has all the inputs, since we are
  // performing the rewrite for only one output resource.
  CachedResult* partition = partitions_->add_partition();
  bool ok_to_rewrite = true;
  for (int i = 0, n = slots_.size(); i < n; ++i) {
    ResourcePtr resource(slot(i)->resource());
    if (resource->loaded() && resource->ContentsValid()) {
      resource->AddInputInfoToPartition(i, partition);
    } else {
      ok_to_rewrite = false;
      break;
    }
  }
  OutputResourcePtr output(fetch_->output_resource());
  ++outstanding_rewrites_;
  if (ok_to_rewrite) {
    Rewrite(0, partition, output);
  } else {
    RewriteDone(RewriteSingleResourceFilter::kRewriteFailed, 0);
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
    const StringPiece& url, const ContentType* content_type,
    OutputResourcePtr* output_resource) {
  bool ret = false;
  GoogleUrl gurl(url);
  ResourceNamer namer;
  if (gurl.is_valid() && namer.Decode(gurl.LeafWithQuery())) {
    output_resource->reset(
        new OutputResource(Manager(),
                           gurl.AllExceptLeaf() /* resolved_base */,
                           gurl.AllExceptLeaf() /* unmapped_base */,
                           Driver()->base_url().Origin() /* original_base */,
                           namer, content_type, Options(), kind()));
    (*output_resource)->set_written_using_rewrite_context_flow(true);
    ret = true;
  }
  return ret;
}

void RewriteContext::Freshen(const CachedResult& partition) {
  // TODO(morlovich): This isn't quite enough as this doesn't cause us to
  // update the expiration in the partition tables; it merely makes it
  // essentially prefetch things in the cache for the future, which might
  // help the rewrite get in by the deadline.
  for (int i = 0, m = partition.input_size(); i < m; ++i) {
    const InputInfo& input_info = partition.input(i);
    if ((input_info.type() == InputInfo::CACHED) &&
        input_info.has_expiration_time_ms() &&
        input_info.has_fetch_time_ms() &&
        input_info.has_index()) {
      if (Manager()->IsImminentlyExpiring(input_info.fetch_time_ms(),
                                          input_info.expiration_time_ms())) {
        ResourcePtr resource(slots_[input_info.index()]->resource());
        resource->Freshen(Manager()->message_handler());
      }
    }
  }
}

const UrlSegmentEncoder* RewriteContext::encoder() const {
  return &default_encoder_;
}

GoogleString RewriteContext::CacheKey() const {
  GoogleString key;
  StringVector urls;
  for (int i = 0, n = num_slots(); i < n; ++i) {
    ResourcePtr resource(slot(i)->resource());
    urls.push_back(resource->url());
  }
  encoder()->Encode(urls, resource_context_.get(), &key);
  return key;
}

bool RewriteContext::Fetch(
    const OutputResourcePtr& output_resource,
    Writer* response_writer,
    ResponseHeaders* response_headers,
    MessageHandler* message_handler,
    UrlAsyncFetcher::Callback* callback) {
  // Decode the URLs required to execute the rewrite.
  bool ret = false;
  StringVector urls;
  GoogleUrl base(output_resource->decoded_base());
  RewriteDriver* driver = Driver();
  driver->InitiateFetch(this);
  if (encoder()->Decode(output_resource->name(), &urls, resource_context_.get(),
                        message_handler)) {
    for (int i = 0, n = urls.size(); i < n; ++i) {
      GoogleUrl url(base, urls[i]);
      if (!url.is_valid()) {
        return false;
      }
      ResourcePtr resource(driver->CreateInputResource(url));
      if (resource.get() == NULL) {
        // TODO(jmarantz): bump invalid-input-resource count
        return false;
      }
      ResourceSlotPtr slot(new FetchResourceSlot(resource));
      AddSlot(slot);
    }
    SetPartitionKey();
    fetch_.reset(
        new FetchContext(this, response_writer, response_headers, callback,
                         output_resource, message_handler));
    Driver()->AddRewriteTask(new MemberFunction0<RewriteContext>(
        &RewriteContext::StartFetch, this));
    ret = true;
  }
  return ret;
}

void RewriteContext::StartFetch() {
  FetchInputs(kMayBlock);
}

RewriteDriver* RewriteContext::Driver() const {
  const RewriteContext* rc;
  for (rc = this; rc->driver_ == NULL; rc = rc->parent_) {
    CHECK(rc != NULL);
  }
  return rc->driver_;
}

ResourceManager* RewriteContext::Manager() const {
  return Driver()->resource_manager();
}

const RewriteOptions* RewriteContext::Options() {
  return Driver()->options();
}

}  // namespace net_instaweb
