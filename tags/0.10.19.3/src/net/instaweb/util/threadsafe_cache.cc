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

#include "net/instaweb/util/public/threadsafe_cache.h"

#include "base/scoped_ptr.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/shared_string.h"
#include "net/instaweb/util/public/string.h"

namespace net_instaweb {

ThreadsafeCache::~ThreadsafeCache() {
}

namespace {

class ThreadsafeCallback : public CacheInterface::Callback {
 public:
  ThreadsafeCallback(AbstractMutex* mutex,
                     const GoogleString& key,
                     CacheInterface::Callback* callback)
      : mutex_(mutex),
        key_(key),
        callback_(callback) {
    mutex_->Lock();
  }

  virtual ~ThreadsafeCallback() {
  }

  virtual void Done(CacheInterface::KeyState state) {
    mutex_->Unlock();
    *callback_->value() = *value();
    callback_->Done(state);
    delete this;
  }

  AbstractMutex* mutex_;
  const GoogleString& key_;
  CacheInterface::Callback* callback_;
};

}  // namespace

void ThreadsafeCache::Get(const GoogleString& key, Callback* callback) {
  ThreadsafeCallback* cb = new ThreadsafeCallback(mutex_.get(), key, callback);
  cache_->Get(key, cb);
}

void ThreadsafeCache::Put(const GoogleString& key, SharedString* value) {
  ScopedMutex mutex(mutex_.get());
  cache_->Put(key, value);
}

void ThreadsafeCache::Delete(const GoogleString& key) {
  ScopedMutex mutex(mutex_.get());
  cache_->Delete(key);
}

}  // namespace net_instaweb
