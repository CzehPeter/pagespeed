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

#include "net/instaweb/util/public/queued_worker_pool.h"

#include <deque>
#include <set>
#include <vector>

#include "base/logging.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/condvar.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/queued_worker.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/public/waveform.h"

namespace net_instaweb {

QueuedWorkerPool::QueuedWorkerPool(int max_workers, ThreadSystem* thread_system)
    : thread_system_(thread_system),
      mutex_(thread_system_->NewMutex()),
      max_workers_(max_workers),
      shutdown_(false),
      queue_size_(NULL) {
}

QueuedWorkerPool::~QueuedWorkerPool() {
  ShutDown();

  // Final shutdown (in case ShutDown was not called) and deletion of
  // sequences.
  for (int i = 0, n = all_sequences_.size(); i < n; ++i) {
    Sequence* sequence = all_sequences_[i];
    sequence->WaitForShutDown();
    delete sequence;
  }
}

void QueuedWorkerPool::ShutDown() {
  // Set the shutdown flag so that no one adds any more groups.
  {
    ScopedMutex lock(mutex_.get());
    if (shutdown_) {
      // ShutDown might be called explicitly and also from the destructor.
      // No workers should have magically re-appeared while in shutdown mode,
      // although the all_sequences_ vector may be non-empty since we don't
      // delete those till the pool itself is deleted.
      DCHECK(active_workers_.empty());
      DCHECK(available_workers_.empty());
      return;
    }
    shutdown_ = true;
  }

  // Clear out all the sequences, so that no one adds any more runnable
  // functions. We don't need to lock our access to all_sequences_ as
  // that can only be mutated when shutdown_ == false.
  for (int i = 0, n = all_sequences_.size(); i < n; ++i) {
    Sequence* sequence = all_sequences_[i];
    sequence->WaitForShutDown();
    // Do not delete the sequence; just leave it in shutdown-mode so no
    // further tasks will be started in the thread.
  }

  // Wait for all workers to complete whatever they were doing.
  //
  // TODO(jmarantz): attempt to cancel in-progress functions via
  // Function::set_quit_requested.  For now, we just complete the
  // currently running functions and then shut down.
  while (true) {
    QueuedWorker* worker = NULL;
    {
      ScopedMutex lock(mutex_.get());
      if (active_workers_.empty()) {
        break;
      }
      std::set<QueuedWorker*>::iterator p = active_workers_.begin();
      worker = *p;
      active_workers_.erase(p);
    }
    worker->ShutDown();
    delete worker;
  }

  // At this point there are no active tasks or workers, so we can stop
  // mutexing.
  for (int i = 0, n = available_workers_.size(); i < n; ++i) {
    QueuedWorker* worker = available_workers_[i];
    worker->ShutDown();
    delete worker;
  }
  available_workers_.clear();
}

// Runs computable tasks through a worker.  Note that a first
// candidate sequence is passed into this method, but we can start
// looking at a new sequence when the passed-in one is exhausted
void QueuedWorkerPool::Run(Sequence* sequence, QueuedWorker* worker) {
  while (sequence != NULL) {
    // This is a little unfair but we will continue to pull tasks from
    // the same sequence and run them until the sequence is exhausted.  This
    // avoids locking the pool's central mutex every time we want to
    // run a new task; we need only mutex at the sequence level.
    while (Function* function = sequence->NextFunction()) {
      function->CallRun();
    }

    // Once a sequence is exhausted see if there's another queued sequence,
    // If there are no available sequences, the worker gets put back into
    // the 'available' list to wait for another Sequence::Add.
    sequence = AssignWorkerToNextSequence(worker);
  }
}

QueuedWorkerPool::Sequence* QueuedWorkerPool::AssignWorkerToNextSequence(
    QueuedWorker* worker) {
  Sequence* sequence = NULL;
  ScopedMutex lock(mutex_.get());
  if (!shutdown_) {
    if (queued_sequences_.empty()) {
      int erased = active_workers_.erase(worker);
      DCHECK_EQ(1, erased);
      available_workers_.push_back(worker);
    } else {
      sequence = queued_sequences_.front();
      queued_sequences_.pop_front();
    }
  }
  return sequence;
}

void QueuedWorkerPool::QueueSequence(Sequence* sequence) {
  QueuedWorker* worker = NULL;
  {
    ScopedMutex lock(mutex_.get());
    if (available_workers_.empty()) {
      // If we have haven't yet initiated our full allotment of threads, add
      // on demand until we hit that limit.
      if (active_workers_.size() < max_workers_) {
        worker = new QueuedWorker(thread_system_);
        worker->Start();
        active_workers_.insert(worker);
      } else {
        // No workers available: must queue the sequence.
        queued_sequences_.push_back(sequence);
      }
    } else {
      // We pulled a worker off the free-stack.
      worker = available_workers_.back();
      available_workers_.pop_back();
      active_workers_.insert(worker);
    }
  }

  // Run the worker without holding the Pool lock.
  if (worker != NULL) {
    worker->RunInWorkThread(
        new MemberFunction2<QueuedWorkerPool, QueuedWorkerPool::Sequence*,
                            QueuedWorker*>(
            &QueuedWorkerPool::Run, this, sequence, worker));
  }
}

bool QueuedWorkerPool::AreBusy(const SequenceSet& sequences) {
  // This is the only operation that accesses multiple workers at once.
  // We order our lock acquisitions by address comparisons to get
  // 2-phase locking, and thus avoid deadlock... With the ordering
  // done for us by SequenceSet already.

  for (SequenceSet::iterator i = sequences.begin(); i != sequences.end(); ++i) {
    (*i)->sequence_mutex_->Lock();
  }

  bool busy = false;
  for (SequenceSet::iterator i = sequences.begin(); i != sequences.end(); ++i) {
    if ((*i)->IsBusy()) {
      busy = true;
      break;
    }
  }

  for (SequenceSet::iterator i = sequences.begin(); i != sequences.end(); ++i) {
    (*i)->sequence_mutex_->Unlock();
  }

  return busy;
}

QueuedWorkerPool::Sequence* QueuedWorkerPool::NewSequence() {
  ScopedMutex lock(mutex_.get());
  Sequence* sequence = NULL;
  if (!shutdown_) {
    if (free_sequences_.empty()) {
      sequence = new Sequence(thread_system_, this);
      sequence->set_queue_size_stat(queue_size_);
      all_sequences_.push_back(sequence);
    } else {
      sequence = free_sequences_.back();
      free_sequences_.pop_back();
      sequence->Reset();
    }
  }
  return sequence;
}

void QueuedWorkerPool::FreeSequence(Sequence* sequence) {
  // If the sequence is inactive, then we can immediately
  // recycle it.  But if the sequence was busy, then we must
  // wait until it completes its last function to recycle it.
  // This will happen in QueuedWorkerPool::Sequence::NextFunction,
  // which will then call SequenceNoLongerActive.
  if (sequence->InitiateShutDown()) {
    ScopedMutex lock(mutex_.get());
    free_sequences_.push_back(sequence);
  }
}

void QueuedWorkerPool::SequenceNoLongerActive(Sequence* sequence) {
  ScopedMutex lock(mutex_.get());
  if (!shutdown_) {
    free_sequences_.push_back(sequence);
  }
}

QueuedWorkerPool::Sequence::Sequence(ThreadSystem* thread_system,
                                     QueuedWorkerPool* pool)
    : sequence_mutex_(thread_system->NewMutex()),
      pool_(pool),
      termination_condvar_(sequence_mutex_->NewCondvar()),
      queue_size_(NULL) {
  Reset();
}

void QueuedWorkerPool::Sequence::Reset() {
  shutdown_ = false;
  active_ = false;
  DCHECK(work_queue_.empty());
}

QueuedWorkerPool::Sequence::~Sequence() {
  DCHECK(shutdown_);
  DCHECK(work_queue_.empty());
}

QueuedWorkerPool::Sequence::AddFunction::~AddFunction() {
}

bool QueuedWorkerPool::Sequence::InitiateShutDown() {
  ScopedMutex lock(sequence_mutex_.get());
  shutdown_ = true;
  return !active_;
}

void QueuedWorkerPool::Sequence::WaitForShutDown() {
  int num_canceled = 0;
  {
    ScopedMutex lock(sequence_mutex_.get());
    shutdown_ = true;
    pool_ = NULL;

    while (active_) {
      // We use a TimedWait rather than a Wait so that we don't deadlock if
      // active_ turns false after the above check and before the call to
      // TimedWait.
      termination_condvar_->TimedWait(Timer::kSecondMs);
    }
    num_canceled = CancelTasksOnWorkQueue();
    DCHECK(work_queue_.empty());
  }
  if ((queue_size_ != NULL) && (num_canceled != 0)) {
    queue_size_->AddDelta(-num_canceled);
  }
}

int QueuedWorkerPool::Sequence::CancelTasksOnWorkQueue() {
  int num_canceled = 0;
  while (!work_queue_.empty()) {
    Function* function = work_queue_.front();
    work_queue_.pop_front();
    sequence_mutex_->Unlock();
    function->CallCancel();
    ++num_canceled;
    sequence_mutex_->Lock();
  }
  return num_canceled;
}

void QueuedWorkerPool::Sequence::Add(Function* function) {
  bool queue_sequence = false;
  {
    ScopedMutex lock(sequence_mutex_.get());
    if (shutdown_) {
      LOG(WARNING) << "Adding function to sequence " << this
                   << " after shutdown";
      function->CallCancel();
      return;
    }
    work_queue_.push_back(function);
    queue_sequence = (!active_ && (work_queue_.size() == 1));
  }
  if (queue_sequence) {
    pool_->QueueSequence(this);
  }
  if (queue_size_ != NULL) {
    queue_size_->AddDelta(1);
  }
}

Function* QueuedWorkerPool::Sequence::NextFunction() {
  Function* function = NULL;
  QueuedWorkerPool* release_to_pool = NULL;
  int queue_size_delta = 0;
  {
    ScopedMutex lock(sequence_mutex_.get());
    if (shutdown_) {
      if (active_) {
        if (!work_queue_.empty()) {
          LOG(WARNING) << "Canceling " << work_queue_.size()
                       << " functions on sequence Shutdown";
          queue_size_delta -= CancelTasksOnWorkQueue();
        }
        active_ = false;

        // Note after the Signal(), the current sequence may be
        // deleted if we are in the process of shutting down the
        // entire pool, so no further access to member variables is
        // allowed.  Hence we copied the pool_ variable to a local
        // temp so we can return it.  Note also that if the pool is in
        // the process of shutting down, then pool_ will be NULL so we
        // won't bother to add the free_sequences_ list.  In any case
        // this will be cleaned on shutdown via all_sequences_.
        release_to_pool = pool_;
        termination_condvar_->Signal();
      }
    } else if (work_queue_.empty()) {
      active_ = false;
    } else {
      function = work_queue_.front();
      work_queue_.pop_front();
      active_ = true;
      --queue_size_delta;
    }
  }
  if (release_to_pool != NULL) {
    // If the entire pool is in the process of shutting down when
    // NextFunction is called, we don't need to add this to the
    // free list; the pool will directly delete all sequences from
    // QueuedWorkerPool::ShutDown().
    release_to_pool->SequenceNoLongerActive(this);
  }
  if ((queue_size_ != NULL) && (queue_size_delta != 0)) {
    queue_size_->AddDelta(queue_size_delta);
  }

  return function;
}

bool QueuedWorkerPool::Sequence::IsBusy() {
  return active_ || !work_queue_.empty();
}

}  // namespace net_instaweb
