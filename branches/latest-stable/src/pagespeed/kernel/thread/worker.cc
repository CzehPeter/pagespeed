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
// Implements Worker, base class for various run-in-a-thread classes,
// via Worker::WorkThread.

#include "pagespeed/kernel/thread/worker.h"

#include <deque>

#include "base/logging.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/atomic_bool.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/condvar.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/waveform.h"

namespace net_instaweb {

// The actual thread that does the work.
class Worker::WorkThread : public ThreadSystem::Thread {
 public:
  WorkThread(Worker* owner, StringPiece thread_name, ThreadSystem* runtime)
      : Thread(runtime, thread_name, ThreadSystem::kJoinable),
        owner_(owner),
        mutex_(runtime->NewMutex()),
        state_change_(mutex_->NewCondvar()),
        current_task_(NULL),
        exit_(false),
        started_(false) {
    quit_requested_.set_value(false);
  }

  // If worker thread exit is requested, returns NULL.  Returns next
  // pending task, also setting it in current_task_ otherwise.  Takes
  // care of synchronization, including waiting for next state change.
  Function* GetNextTask() {
    ScopedMutex lock(mutex_.get());

    // Clean any task we were running last iteration
    current_task_ = NULL;

    while (!exit_ && tasks_.empty()) {
      state_change_->Wait();
    }

    // Handle exit.
    if (exit_) {
      return NULL;
    }

    // Get task.
    current_task_ = tasks_.front();
    tasks_.pop_front();
    owner_->UpdateQueueSizeStat(-1);

    return current_task_;
  }

  virtual void Run() {
    Function* task;
    while ((task = GetNextTask()) != NULL) {
      // Run tasks (not holding the lock, so new tasks can be added).
      task->set_quit_requested_pointer(&quit_requested_);
      task->CallRun();
    }
  }

  void ShutDown() {
    {
      ScopedMutex lock(mutex_.get());

      if (exit_ || !started_) {
        // Already shutdown, or was never started in the first place.
        return;
      }

      exit_ = true;
      if (current_task_ != NULL) {
        quit_requested_.set_value(true);
      }
      state_change_->Signal();
    }

    Join();
    int delta = tasks_.size();
    owner_->UpdateQueueSizeStat(-delta);
    while (!tasks_.empty()) {
      Function* closure = tasks_.front();
      tasks_.pop_front();
      closure->CallCancel();
    }
    started_ = false;  // Reject further jobs on explicit shutdown.
  }

  void Start() {
    ScopedMutex lock(mutex_.get());
    if (!started_ && !exit_) {
      started_ = Thread::Start();
      if (!started_) {
        LOG(ERROR) << "Unable to start worker thread";
      }
    }
  }

  bool QueueIfPermitted(Function* closure) {
    if (!started_) {
      closure->CallCancel();
      return true;
    }

    ScopedMutex lock(mutex_.get());
    if (owner_->IsPermitted(closure)) {
      tasks_.push_back(closure);
      owner_->UpdateQueueSizeStat(1);
      if (current_task_ == NULL) {  // wake the thread up if it's idle.
        state_change_->Signal();
      }
      return true;
    } else {
      return false;
    }
  }

  // Must be called with mutex held.
  int NumJobs() {
    int num = static_cast<int>(tasks_.size());
    if (current_task_ != NULL) {
      ++num;
    }
    return num;
  }

  bool IsBusy() const {
    ScopedMutex lock(mutex_.get());
    return (current_task_ != NULL) || !tasks_.empty();
  }

 private:
  Worker* owner_;

  // guards state_change_, current_task_, tasks_, exit_;
  scoped_ptr<ThreadSystem::CondvarCapableMutex> mutex_;
  scoped_ptr<ThreadSystem::Condvar> state_change_;
  Function* current_task_;  // non-NULL if we are actually running something.
  std::deque<Function*> tasks_;  // things waiting to be run.
  bool exit_;
  bool started_;
  AtomicBool quit_requested_;

  DISALLOW_COPY_AND_ASSIGN(WorkThread);
};

Worker::Worker(StringPiece thread_name, ThreadSystem* runtime)
    : thread_(new WorkThread(this, thread_name, runtime)),
      queue_size_(NULL) {
}

Worker::~Worker() {
  thread_->ShutDown();
}

void Worker::Start() {
  thread_->Start();
}

bool Worker::IsBusy() {
  return thread_->IsBusy();
}

bool Worker::QueueIfPermitted(Function* closure) {
  return thread_->QueueIfPermitted(closure);
}

int Worker::NumJobs() {
  return thread_->NumJobs();
}

void Worker::ShutDown() {
  thread_->ShutDown();
}

void Worker::UpdateQueueSizeStat(int value) {
  if (queue_size_ != NULL) {
    queue_size_->AddDelta(value);
  }
}


}  // namespace net_instaweb
