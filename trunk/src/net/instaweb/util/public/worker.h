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
// This contains Worker, which is a base class for classes managing running
// of work in background, as well as Worker::Closure, the base class for the
// work to run.

#ifndef NET_INSTAWEB_UTIL_PUBLIC_WORKER_H_
#define NET_INSTAWEB_UTIL_PUBLIC_WORKER_H_

#include "base/scoped_ptr.h"
#include "net/instaweb/util/public/atomicops.h"
#include "net/instaweb/util/public/basictypes.h"

namespace net_instaweb {

class ThreadSystem;

// This class is a base for various mechanisms of running things in background.
//
// If you just want to run something in background, you want to use a subclass
// of this, such as a SlowWorker or QueuedWorker instance.
//
// Subclasses should implement bool PermitQueue() and provide an appropriate
// wrapper around QueueIfPermitted().
class Worker {
 public:
  // Tasks you wish the worker to perform must subclass this and implement
  // Run(). Long running tasks should check quit_requested() inside it
  // periodically.
  //
  // Worker classes take ownership of any closures passed to them, and
  // may also delete them without running on shutdown or if dictated by
  // policy.
  class Closure {
   public:
    Closure();
    virtual ~Closure();

    virtual void Run() = 0;
    bool quit_requested() const {
      return base::subtle::Acquire_Load(&quit_requested_);
    }

    void set_quit_requested(bool q) {
      base::subtle::Release_Store(&quit_requested_, q);
    }

   private:
    base::subtle::AtomicWord quit_requested_;
    DISALLOW_COPY_AND_ASSIGN(Closure);
  };

  // Tries to start the thread. It will be cleaned up by ~Worker().
  // Returns whether successful.
  bool Start();

  // An Idle callback is called when a worker that is running
  // a task completes all its tasks, and goes into a wait-state
  // for more tasks to be queued.
  //
  // The idle callback will not be called immediately when a Worker
  // is started, even if it starts in the idle state.  It is only called
  // on the completion of all queued tasks.
  //
  // The idle-callback is intended only for testing purposes.  If
  // this is ever used for anything else we should consider making
  // a vector of callbacks and changing the method to add_idle_callback.
  void set_idle_callback(Closure* cb) { idle_callback_.reset(cb); }

  // Finishes the currently running jobs, and deletes any queued jobs.
  // No further jobs will be accepted after this call either; they will
  // just be deleted. It is safe to call this method multiple times.
  void ShutDown();

 protected:
  explicit Worker(ThreadSystem* runtime);
  virtual ~Worker();

  // If IsPermitted() returns true, queues up the given closure to be run,
  // takes ownership of closure, and returns true. (Also wakes up the work
  // thread to actually run it if it's idle)
  //
  // Otherwise it merely returns false, and doesn't do anything else.
  bool QueueIfPermitted(Closure* closure);

  // Subclasses should implement this method to implement the policy
  // on whether to run given tasks or not.
  virtual bool IsPermitted(Closure* closure) = 0;

  // Returns the number of jobs, including any running and queued jobs.
  // The lock semantics here are as follows:
  // - QueueIfPermitted calls IsPermitted with lock held.
  // - NumJobs assumes lock to be held.
  // => It's safe to call NumJobs from within IsPermitted if desired.
  int NumJobs();

 private:
  class WorkThread;
  friend class WorkThread;

  void RunIdleCallback();

  scoped_ptr<WorkThread> thread_;
  scoped_ptr<Closure> idle_callback_;

  DISALLOW_COPY_AND_ASSIGN(Worker);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_WORKER_H_
