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

// Helper class for sending concurrent traffic to a cache during unit tests.

#ifndef PAGESPEED_KERNEL_CACHE_CACHE_SPAMMER_H_
#define PAGESPEED_KERNEL_CACHE_CACHE_SPAMMER_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

class CacheInterface;

// Helper class for blasting a cache with concurrent requests.  Refactored
// from threadsafe_cache_test.cc.
class CacheSpammer : public ThreadSystem::Thread {
 public:
  virtual ~CacheSpammer();

  // value_pattern will be used as a format must have a single %d.
  static void RunTests(int num_threads, int num_iters, int num_inserts,
                       bool expecting_evictions, bool do_deletes,
                       const char* value_pattern,
                       CacheInterface* cache,
                       ThreadSystem* thread_runtime);

 protected:
  virtual void Run();

 private:
  CacheSpammer(ThreadSystem* runtime,
               ThreadSystem::ThreadFlags flags,
               CacheInterface* cache,
               bool expecting_evictions,
               bool do_deletes,
               const char* value_pattern,
               int index,
               int num_iters,
               int num_inserts);

  CacheInterface* cache_;
  bool expecting_evictions_;
  bool do_deletes_;
  const char* value_pattern_;
  int index_;
  int num_iters_;
  int num_inserts_;

  DISALLOW_COPY_AND_ASSIGN(CacheSpammer);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_CACHE_CACHE_SPAMMER_H_
