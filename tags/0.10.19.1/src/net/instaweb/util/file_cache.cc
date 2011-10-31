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

// Author: lsong@google.com (Libo Song)

#include "net/instaweb/util/public/file_cache.h"

#include <cstddef>
#include <cstdlib>
#include <vector>
#include <queue>
#include "base/scoped_ptr.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/filename_encoder.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/null_message_handler.h"
#include "net/instaweb/util/public/shared_string.h"
#include "net/instaweb/util/public/slow_worker.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/timer.h"

namespace net_instaweb {

namespace {  // For structs used only in Clean().

class CacheFileInfo {
 public:
  CacheFileInfo(int64 size, int64 atime, const GoogleString& name)
      : size_(size), atime_(atime), name_(name) {}
  int64 size_;
  int64 atime_;
  GoogleString name_;
 private:
  DISALLOW_COPY_AND_ASSIGN(CacheFileInfo);
};

struct CompareByAtime {
 public:
  bool operator()(const CacheFileInfo* one,
                  const CacheFileInfo* two) const {
    return one->atime_ < two->atime_;
  }
};

}  // namespace for structs used only in Clean().

class FileCache::CacheCleanFunction : public Function {
 public:
  CacheCleanFunction(FileCache* cache, int64 next_clean_time_ms)
      : cache_(cache),
        next_clean_time_ms_(next_clean_time_ms) {}
  virtual ~CacheCleanFunction() {}
  virtual void Run() {
    cache_->last_conditional_clean_result_ =
        cache_->CleanWithLocking(next_clean_time_ms_);
  }

 private:
  FileCache* cache_;
  int64 next_clean_time_ms_;
  DISALLOW_COPY_AND_ASSIGN(CacheCleanFunction);
};

// Filenames for the next scheduled clean time and the lockfile.  In
// order to prevent these from colliding with actual cachefiles, they
// contain characters that our filename encoder would escape.
const char FileCache::kCleanTimeName[] = "!clean!time!";
const char FileCache::kCleanLockName[] = "!clean!lock!";

// TODO(abliss): remove policy from constructor; provide defaults here
// and setters below.
FileCache::FileCache(const GoogleString& path, FileSystem* file_system,
                     SlowWorker* worker,
                     FilenameEncoder* filename_encoder,
                     CachePolicy* policy,
                     MessageHandler* handler)
    : path_(path),
      file_system_(file_system),
      worker_(worker),
      filename_encoder_(filename_encoder),
      message_handler_(handler),
      cache_policy_(policy),
      path_length_limit_(file_system_->MaxPathLength(path)),
      clean_time_path_(path) {
  // NOTE(abliss): We don't want all the caches racing for the
  // lock at startup, so each one gets a random offset.
  next_clean_ms_ = policy->timer->NowMs()
      + (random() % policy->clean_interval_ms);
  EnsureEndsInSlash(&clean_time_path_);
  clean_time_path_ += kCleanTimeName;
}

FileCache::~FileCache() {
}

void FileCache::Get(const GoogleString& key, Callback* callback) {
  GoogleString filename;
  bool ret = EncodeFilename(key, &filename);
  if (ret) {
    GoogleString* buffer = callback->value()->get();

    // Suppress read errors.  Note that we want to show Write errors,
    // as they likely indicate a permissions or disk-space problem
    // which is best not eaten.  It's cheap enough to construct
    // a NullMessageHandler on the stack when we want one.
    NullMessageHandler null_handler;
    ret = file_system_->ReadFile(filename.c_str(), buffer, &null_handler);
  }
  callback->Done(ret ? kAvailable : kNotFound);
}

void FileCache::Put(const GoogleString& key, SharedString* value) {
  GoogleString filename;
  if (EncodeFilename(key, &filename)) {
    const GoogleString& buffer = **value;
    GoogleString temp_filename;
    if (file_system_->WriteTempFile(filename, buffer,
                                    &temp_filename, message_handler_)) {
      file_system_->RenameFile(temp_filename.c_str(), filename.c_str(),
                               message_handler_);
    }
  }
  CleanIfNeeded();
}

void FileCache::Delete(const GoogleString& key) {
  GoogleString filename;
  if (!EncodeFilename(key, &filename)) {
    return;
  }
  file_system_->RemoveFile(filename.c_str(), message_handler_);
  return;
}

bool FileCache::EncodeFilename(const GoogleString& key,
                               GoogleString* filename) {
  GoogleString prefix = path_;
  // TODO(abliss): unify and make explicit everyone's assumptions
  // about trailing slashes.
  EnsureEndsInSlash(&prefix);
  filename_encoder_->Encode(prefix, key, filename);

  // Make sure the length isn't too big for filesystem to handle; if it is
  // just name the object using a hash.
  if (static_cast<int>(filename->length()) > path_length_limit_) {
    filename_encoder_->Encode(prefix, cache_policy_->hasher->Hash(key),
                              filename);
  }

  return true;
}

bool FileCache::Clean(int64 target_size) {
  StringVector files;
  int64 file_size;
  int64 file_atime;
  int64 total_size = 0;

  message_handler_->Message(kInfo,
                            "Checking cache size against target %ld",
                            static_cast<long>(target_size));

  if (!file_system_->RecursiveDirSize(path_, &total_size, message_handler_)) {
    return false;
  }

  // TODO(jmarantz): gcc 4.1 warns about double/int64 comparisons here,
  // but this really should be factored into a settable member var.
  if (total_size < ((target_size * 5) / 4)) {
    message_handler_->Message(kInfo,
                              "File cache size is %ld; no cleanup needed.",
                              static_cast<long>(total_size));
    return true;
  }
  message_handler_->Message(kInfo,
                            "File cache size is %ld; beginning cleanup.",
                            static_cast<long>(total_size));
  bool everything_ok = true;
  everything_ok &= file_system_->ListContents(path_, &files, message_handler_);

  // We will now iterate over the entire directory and its children,
  // keeping a heap of files to be deleted.  Our goal is to delete the
  // oldest set of files that sum to enough space to bring us below
  // our target.
  std::priority_queue<CacheFileInfo*, std::vector<CacheFileInfo*>,
      CompareByAtime> heap;
  int64 total_heap_size = 0;
  // TODO(jmarantz): gcc 4.1 warns about double/int64 comparisons here,
  // but this really should be factored into a settable member var.
  int64 target_heap_size = total_size - ((target_size * 3 / 4));

  GoogleString prefix = path_;
  EnsureEndsInSlash(&prefix);
  for (size_t i = 0; i < files.size(); i++) {
    GoogleString file_name = files[i];
    BoolOrError isDir = file_system_->IsDir(file_name.c_str(),
                                            message_handler_);
    if (isDir.is_error()) {
      return false;
    } else if (clean_time_path_.compare(file_name) == 0) {
      // Don't clean the clean_time file!  It ought to be the newest file (and
      // very small) so the following algorithm would normally not delete it
      // anyway.  But on some systems (e.g. mounted noatime?) it was getting
      // deleted.
      continue;
    } else if (isDir.is_true()) {
      // add files in this directory to the end of the vector, to be
      // examined later.
      everything_ok &= file_system_->ListContents(file_name, &files,
                                                  message_handler_);
    } else {
      everything_ok &= file_system_->Size(file_name, &file_size,
                                          message_handler_);
      everything_ok &= file_system_->Atime(file_name, &file_atime,
                                           message_handler_);
      // If our heap is still too small; add everything in.
      // Otherwise, add the file in only if it's older than the newest
      // thing in the heap.
      if ((total_heap_size < target_heap_size) ||
          (file_atime < heap.top()->atime_)) {
        CacheFileInfo* info =
            new CacheFileInfo(file_size, file_atime, file_name);
        heap.push(info);
        total_heap_size += file_size;
        // Now remove new things from the heap which are not needed
        // to keep the heap size over its target size.
        while (total_heap_size - heap.top()->size_ > target_heap_size) {
          total_heap_size -= heap.top()->size_;
          delete heap.top();
          heap.pop();
        }
      }
    }
  }
  for (size_t i = heap.size(); i > 0; i--) {
    everything_ok &= file_system_->RemoveFile(heap.top()->name_.c_str(),
                                              message_handler_);
    delete heap.top();
    heap.pop();
  }
  message_handler_->Message(kInfo,
                            "File cache cleanup complete; freed %ld bytes\n",
                            static_cast<long>(total_heap_size));
  return everything_ok;
}

bool FileCache::CleanWithLocking(int64 next_clean_time_ms) {
  bool to_return = false;

  GoogleString lock_name(path_);
  EnsureEndsInSlash(&lock_name);
  lock_name += kCleanLockName;
  if (file_system_->TryLockWithTimeout(
      lock_name, Timer::kHourMs, message_handler_).is_true()) {
    // Update the timestamp file..
    next_clean_ms_ = next_clean_time_ms;
    file_system_->WriteFile(clean_time_path_.c_str(),
                            Integer64ToString(next_clean_time_ms),
                            message_handler_);

    // Now actually clean.
    to_return = Clean(cache_policy_->target_size);
    file_system_->Unlock(lock_name, message_handler_);
  }
  return to_return;
}

bool FileCache::ShouldClean(int64* suggested_next_clean_time_ms) {
  bool to_return = false;
  const int64 now_ms = cache_policy_->timer->NowMs();
  if (now_ms < next_clean_ms_) {
    *suggested_next_clean_time_ms = next_clean_ms_;  // No change yet.
    return false;
  }

  GoogleString clean_time_str;
  int64 clean_time_ms = 0;
  int64 new_clean_time_ms = now_ms + cache_policy_->clean_interval_ms;
  NullMessageHandler null_handler;
  if (file_system_->ReadFile(clean_time_path_.c_str(), &clean_time_str,
                              &null_handler)) {
    StringToInt64(clean_time_str, &clean_time_ms);
  } else {
    message_handler_->Message(
        kWarning, "Failed to read cache clean timestamp %s. "
        " Doing an extra cache clean to be safe.", clean_time_path_.c_str());
  }

  // If the "clean time" written in the file is older than now, we
  // clean.
  if (clean_time_ms < now_ms) {
    message_handler_->Message(kInfo,
                              "Need to check cache size against target %ld",
                              static_cast<long>(cache_policy_->target_size));
    to_return = true;
  }
  // If the "clean time" is later than now plus one interval, something
  // went wrong (like the system clock moving backwards or the file
  // getting corrupt) so we clean and reset it.
  if (clean_time_ms > new_clean_time_ms) {
    message_handler_->Message(kError,
                              "Next scheduled file cache clean time %s"
                              " is implausibly remote.  Cleaning now.",
                              Integer64ToString(clean_time_ms).c_str());
    to_return = true;
  }

  *suggested_next_clean_time_ms = new_clean_time_ms;
  return to_return;
}

void FileCache::CleanIfNeeded() {
  if (worker_ != NULL) {
    int64 suggested_next_clean_time_ms;
    last_conditional_clean_result_ = false;
    if (ShouldClean(&suggested_next_clean_time_ms)) {
      worker_->Start();
      worker_->RunIfNotBusy(
          new CacheCleanFunction(this, suggested_next_clean_time_ms));
    } else {
      next_clean_ms_ = suggested_next_clean_time_ms;
    }
  }
}

}  // namespace net_instaweb
