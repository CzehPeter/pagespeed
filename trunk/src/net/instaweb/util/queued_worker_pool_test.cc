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

// Unit-test for QueuedWorkerPool

#include "net/instaweb/util/public/queued_worker_pool.h"

#include "base/scoped_ptr.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/worker_test_base.h"

namespace net_instaweb {
namespace {

class QueuedWorkerPoolTest: public WorkerTestBase {
 public:
  QueuedWorkerPoolTest()
      : worker_(new QueuedWorkerPool(2, thread_runtime_.get())) {}

 protected:
  scoped_ptr<QueuedWorkerPool> worker_;

 private:
  DISALLOW_COPY_AND_ASSIGN(QueuedWorkerPoolTest);
};

// A function that, without protection of a mutex, increments a shared
// integer.  The intent is that the QueuedWorkerPool::Sequence is
// enforcing the sequentiality on our behalf so we don't have to worry
// about mutexing in here.
class Increment : public Function {
 public:
  Increment(int expected_value, int* count)
      : expected_value_(expected_value),
        count_(count) {
  }

 protected:
  virtual void Run() {
    ++*count_;
    EXPECT_EQ(expected_value_, *count_);
  }
  virtual void Cancel() {
    *count_ -= 100;
    EXPECT_EQ(expected_value_, *count_);
  }

 private:
  int expected_value_;
  int* count_;

  DISALLOW_COPY_AND_ASSIGN(Increment);
};

// Tests that all the jobs queued in one sequence should run sequentially.
TEST_F(QueuedWorkerPoolTest, BasicOperation) {
  const int kBound = 42;
  int count = 0;
  SyncPoint sync(thread_runtime_.get());

  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  for (int i = 0; i < kBound; ++i) {
    sequence->Add(new Increment(i + 1, &count));
  }

  sequence->Add(new NotifyRunFunction(&sync));
  sync.Wait();
  EXPECT_EQ(kBound, count);
  worker_->FreeSequence(sequence);
}

// Test ordinary and cancelled AddFunction callback.
TEST_F(QueuedWorkerPoolTest, AddFunctionTest) {
  const int kBound = 5;
  int count1 = 0;
  int count2 = 0;
  SyncPoint sync(thread_runtime_.get());

  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  for (int i = 0; i < kBound; ++i) {
    QueuedWorkerPool::Sequence::AddFunction
        add(sequence, new Increment(i + 1, &count1));
    add.set_delete_after_callback(false);
    add.CallRun();
    QueuedWorkerPool::Sequence::AddFunction
        cancel(sequence, new Increment(-100 * (i + 1), &count2));
    cancel.set_delete_after_callback(false);
    cancel.CallCancel();
  }

  sequence->Add(new NotifyRunFunction(&sync));
  sync.Wait();
  EXPECT_EQ(kBound, count1);
  EXPECT_EQ(-100 * kBound, count2);
  worker_->FreeSequence(sequence);
}

// Makes sure that even if one sequence is blocked, another can
// complete, because we have more than one thread at our disposal in
// this worker.
TEST_F(QueuedWorkerPoolTest, SlowAndFastSequences) {
  const int kBound = 42;
  int count = 0;
  SyncPoint sync(thread_runtime_.get());
  SyncPoint wait(thread_runtime_.get());

  QueuedWorkerPool::Sequence* slow_sequence = worker_->NewSequence();
  slow_sequence->Add(new WaitRunFunction(&wait));
  slow_sequence->Add(new NotifyRunFunction(&sync));

  QueuedWorkerPool::Sequence* fast_sequence = worker_->NewSequence();
  for (int i = 0; i < kBound; ++i) {
    fast_sequence->Add(new Increment(i + 1, &count));
  }

  // At this point the fast sequence is churning through its work, while the
  // slow sequence is blocked waiting for SyncPoint 'wait'.  Let the fast
  // sequence unblock it.
  fast_sequence->Add(new NotifyRunFunction(&wait));

  sync.Wait();
  EXPECT_EQ(kBound, count);
  worker_->FreeSequence(fast_sequence);
  worker_->FreeSequence(slow_sequence);
}

class MakeNewSequence : public Function {
 public:
  MakeNewSequence(WorkerTestBase::SyncPoint* sync,
                  QueuedWorkerPool* pool,
                  QueuedWorkerPool::Sequence* sequence)
      : sync_(sync),
        pool_(pool),
        sequence_(sequence) {
  }

  virtual void Run() {
    pool_->FreeSequence(sequence_);
    pool_->NewSequence()->Add(new WorkerTestBase::NotifyRunFunction(sync_));
  }

 private:
  WorkerTestBase::SyncPoint* sync_;
  QueuedWorkerPool* pool_;
  QueuedWorkerPool::Sequence* sequence_;

  DISALLOW_COPY_AND_ASSIGN(MakeNewSequence);
};

TEST_F(QueuedWorkerPoolTest, RestartSequenceFromFunction) {
  SyncPoint sync(thread_runtime_.get());
  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  sequence->Add(new MakeNewSequence(&sync, worker_.get(), sequence));
  sync.Wait();
}

// Keeps track of whether run or cancel were called.
class LogOpsFunction : public Function {
 public:
  LogOpsFunction() : run_called_(false), cancel_called_(false) {}
  virtual ~LogOpsFunction() {}

  bool run_called() const { return run_called_; }
  bool cancel_called() const { return cancel_called_; }

 protected:
  virtual void Run() { run_called_ = true; }
  virtual void Cancel() { cancel_called_ = true; }

 private:
  bool run_called_;
  bool cancel_called_;
};

// Make sure calling add after worker was shut down Cancel()s the function
// properly.
TEST_F(QueuedWorkerPoolTest, AddAfterShutDown) {
  QueuedWorkerPool::Sequence* sequence = worker_->NewSequence();
  worker_->ShutDown();
  LogOpsFunction f;
  f.set_delete_after_callback(false);
  sequence->Add(&f);
  worker_.reset(NULL);
  EXPECT_TRUE(f.cancel_called());
  EXPECT_FALSE(f.run_called());
}

}  // namespace

}  // namespace net_instaweb
