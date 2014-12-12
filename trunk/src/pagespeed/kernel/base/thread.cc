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

// Author: morlovich@google.com (Maksim Orlovich)
//
// Implementation of the Thread class, which routes things to an underlying
// ThreadImpl.

#include "base/logging.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

ThreadSystem::Thread::Thread(
    ThreadSystem* runtime, StringPiece name, ThreadFlags flags)
    : impl_(runtime->NewThreadImpl(this, flags)),
      flags_(flags),
      started_(false) {
  name.CopyToString(&name_);
}

ThreadSystem::Thread::~Thread() {
}

bool ThreadSystem::Thread::Start() {
  started_ = impl_->StartImpl();
  return started_;
}

void ThreadSystem::Thread::Join() {
  if (!started_) {
    LOG(DFATAL) << "Trying to join thread that wasn't Start()ed";
    return;
  }

  if ((flags_ & ThreadSystem::kJoinable) == 0) {
    LOG(DFATAL) << "Trying to join a detached thread";
    return;
  }

  impl_->JoinImpl();
}

}  // namespace net_instaweb
