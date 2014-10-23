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

#include "pagespeed/kernel/thread/pthread_mutex.h"

#include <pthread.h>
#include "pagespeed/kernel/thread/pthread_condvar.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

PthreadMutex::PthreadMutex() {
  pthread_mutex_init(&mutex_, NULL);
}

PthreadMutex::~PthreadMutex() {
  pthread_mutex_destroy(&mutex_);
}

bool PthreadMutex::TryLock() {
  return (pthread_mutex_trylock(&mutex_) == 0);
}

void PthreadMutex::Lock() {
  pthread_mutex_lock(&mutex_);
}

void PthreadMutex::Unlock() {
  pthread_mutex_unlock(&mutex_);
}

ThreadSystem::Condvar* PthreadMutex::NewCondvar() {
  return new PthreadCondvar(this);
}


}  // namespace net_instaweb
