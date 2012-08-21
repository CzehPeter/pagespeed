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

#ifndef NET_INSTAWEB_APACHE_APACHE_CACHE_H_
#define NET_INSTAWEB_APACHE_APACHE_CACHE_H_

#include "base/scoped_ptr.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class ApacheConfig;
class ApacheRewriteDriverFactory;
class AprMemCache;
class AprMemCacheServers;
class AsyncCache;
class CacheInterface;
class FileCache;
class FileSystemLockManager;
class Hasher;
class HTTPCache;
class MessageHandler;
class NamedLockManager;
class PropertyCache;
class QueuedWorkerPool;
class SharedMemRuntime;
class SharedMemLockManager;
class Timer;

// The ApacheCache encapsulates a cache-sharing model where a user specifies
// a file-cache path per virtual-host.  With each file-cache object we keep
// a locking mechanism and an optional per-process LRUCache.
class ApacheCache {
 public:
  static const char kFileCache[];
  static const char kLruCache[];
  static const char kMemcached[];

  ApacheCache(const StringPiece& path,
              const ApacheConfig& config,
              ApacheRewriteDriverFactory* factory);
  ~ApacheCache();
  CacheInterface* cache() { return cache_.get(); }
  NamedLockManager* lock_manager() { return lock_manager_; }
  HTTPCache* http_cache() { return http_cache_.get(); }
  PropertyCache* page_property_cache() { return page_property_cache_.get(); }
  PropertyCache* client_property_cache() {
    return client_property_cache_.get();
  }

  void RootInit();
  void ChildInit();
  void GlobalCleanup(MessageHandler* handler);  // only called in root process
  AprMemCache* mem_cache() { return mem_cache_; }
  AprMemCacheServers* mem_cache_servers() { return mem_cache_servers_.get(); }

  // Stops any further Gets from occuring in the Async cache.  This is used to
  // help wind down activity during a shutdown.
  void StopAsyncGets();

 private:
  void FallBackToFileBasedLocking();

  GoogleString path_;

  ApacheRewriteDriverFactory* factory_;
  scoped_ptr<CacheInterface> cache_;
  scoped_ptr<SharedMemLockManager> shared_mem_lock_manager_;
  scoped_ptr<FileSystemLockManager> file_system_lock_manager_;
  NamedLockManager* lock_manager_;
  FileCache* file_cache_;   // may be NULL if there we are using memcache.
  AprMemCache* mem_cache_;  // may be NULL if there we are using filecache
  scoped_ptr<AprMemCacheServers> mem_cache_servers_;
  AsyncCache* async_cache_;  // may be NULL if not threading memcache lookups.
  CacheInterface* l2_cache_;
  scoped_ptr<QueuedWorkerPool> pool_;
  scoped_ptr<HTTPCache> http_cache_;
  scoped_ptr<PropertyCache> page_property_cache_;
  scoped_ptr<PropertyCache> client_property_cache_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_APACHE_APACHE_CACHE_H_
