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
// Author: morlovich@google.com (Maksim Orlovich)

#include "net/instaweb/util/public/shared_mem_lock_manager_test_base.h"

#include "base/scoped_ptr.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/md5_hasher.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/named_lock_manager.h"
#include "net/instaweb/util/public/shared_mem_lock_manager.h"
#include "net/instaweb/util/public/shared_mem_test_base.h"
#include "net/instaweb/util/public/thread_system.h"

namespace net_instaweb {

namespace {

const char kPath[] = "shm_locks";
const char kLockA[] = "lock_a";
const char kLockB[] = "lock_b";

}  // namespace

SharedMemLockManagerTestBase::SharedMemLockManagerTestBase(
    SharedMemTestEnv* test_env)
    : test_env_(test_env),
      shmem_runtime_(test_env->CreateSharedMemRuntime()),
      timer_(0),
      thread_system_(ThreadSystem::CreateThreadSystem()),
      scheduler_(thread_system_.get(), &timer_) {
}

void SharedMemLockManagerTestBase::SetUp() {
  root_lock_manager_.reset(CreateLockManager());
  EXPECT_TRUE(root_lock_manager_->Initialize());
}

void SharedMemLockManagerTestBase::TearDown() {
  SharedMemLockManager::GlobalCleanup(shmem_runtime_.get(), kPath, &handler_);
}

bool SharedMemLockManagerTestBase::CreateChild(TestMethod method) {
  Function* callback =
      new MemberFunction0<SharedMemLockManagerTestBase>(method, this);
  return test_env_->CreateChild(callback);
}

SharedMemLockManager* SharedMemLockManagerTestBase::CreateLockManager() {
  return new SharedMemLockManager(shmem_runtime_.get(), kPath, &scheduler_,
                                  &hasher_, &handler_);
}

SharedMemLockManager* SharedMemLockManagerTestBase::AttachDefault() {
  SharedMemLockManager* lock_man = CreateLockManager();
  if (!lock_man->Attach()) {
    delete lock_man;
    lock_man = NULL;
  }
  return lock_man;
}

void SharedMemLockManagerTestBase::TestBasic() {
  scoped_ptr<SharedMemLockManager> lock_manager_(AttachDefault());
  ASSERT_TRUE(lock_manager_.get() != NULL);
  scoped_ptr<NamedLock> lock_a(lock_manager_->CreateNamedLock(kLockA));
  scoped_ptr<NamedLock> lock_b(lock_manager_->CreateNamedLock(kLockB));

  ASSERT_TRUE(lock_a.get() != NULL);
  ASSERT_TRUE(lock_b.get() != NULL);

  EXPECT_FALSE(lock_a->Held());
  EXPECT_FALSE(lock_b->Held());

  // Can lock exactly once...
  EXPECT_TRUE(lock_a->TryLock());
  EXPECT_TRUE(lock_b->TryLock());
  EXPECT_TRUE(lock_a->Held());
  EXPECT_TRUE(lock_b->Held());
  EXPECT_FALSE(lock_a->TryLock());
  EXPECT_FALSE(lock_b->TryLock());
  EXPECT_TRUE(lock_a->Held());
  EXPECT_TRUE(lock_b->Held());

  // Unlocking lets one lock again
  lock_b->Unlock();
  EXPECT_FALSE(lock_b->Held());
  EXPECT_FALSE(lock_a->TryLock());
  EXPECT_TRUE(lock_b->TryLock());

  // Now unlock A, and let kid confirm the state
  lock_a->Unlock();
  EXPECT_FALSE(lock_a->Held());
  CreateChild(&SharedMemLockManagerTestBase::TestBasicChild);
  test_env_->WaitForChildren();

  // A should still be unlocked since child's locks should get cleaned up
  // by ~NamedLock.. but not lock b, which we were holding
  EXPECT_TRUE(lock_a->TryLock());
  EXPECT_FALSE(lock_b->TryLock());
}

void SharedMemLockManagerTestBase::TestBasicChild() {
  scoped_ptr<SharedMemLockManager> lock_manager_(AttachDefault());
  scoped_ptr<NamedLock> lock_a(lock_manager_->CreateNamedLock(kLockA));
  scoped_ptr<NamedLock> lock_b(lock_manager_->CreateNamedLock(kLockB));

  if (lock_a.get() == NULL || lock_b.get() == NULL) {
    test_env_->ChildFailed();
  }

  // A should lock fine
  if (!lock_a->TryLock() || !lock_a->Held()) {
    test_env_->ChildFailed();
  }

  // B shouldn't lock fine.
  if (lock_b->TryLock() || lock_b->Held()) {
    test_env_->ChildFailed();
  }

  // Note: here we should unlock a due to destruction of A.
}

void SharedMemLockManagerTestBase::TestDestructorUnlock() {
  // Standalone test for destructors cleaning up. It is covered by the
  // above, but this does it single-threaded, without weird things.
  scoped_ptr<SharedMemLockManager> lock_manager_(AttachDefault());
  ASSERT_TRUE(lock_manager_.get() != NULL);

  {
    scoped_ptr<NamedLock> lock_a(lock_manager_->CreateNamedLock(kLockA));
    EXPECT_TRUE(lock_a->TryLock());
  }

  {
    scoped_ptr<NamedLock> lock_a(lock_manager_->CreateNamedLock(kLockA));
    EXPECT_TRUE(lock_a->TryLock());
  }
}

void SharedMemLockManagerTestBase::TestSteal() {
  scoped_ptr<SharedMemLockManager> lock_manager_(AttachDefault());
  ASSERT_TRUE(lock_manager_.get() != NULL);
  scoped_ptr<NamedLock> lock_a(lock_manager_->CreateNamedLock(kLockA));
  EXPECT_TRUE(lock_a->TryLock());
  EXPECT_TRUE(lock_a->Held());
  CreateChild(&SharedMemLockManagerTestBase::TestStealChild);
  test_env_->WaitForChildren();
}

void SharedMemLockManagerTestBase::TestStealChild() {
  const int kStealTimeMs = 1000;

  scoped_ptr<SharedMemLockManager> lock_manager_(AttachDefault());
  ASSERT_TRUE(lock_manager_.get() != NULL);
  scoped_ptr<NamedLock> lock_a(lock_manager_->CreateNamedLock(kLockA));

  // First, attempting to steal should fail, as 'time' hasn't moved yet.
  if (lock_a->TryLockStealOld(kStealTimeMs) || lock_a->Held()) {
    test_env_->ChildFailed();
  }

  timer_.AdvanceMs(kStealTimeMs + 1);

  // Now it should succeed.
  if (!lock_a->TryLockStealOld(kStealTimeMs) || !lock_a->Held()) {
    test_env_->ChildFailed();
  }
}

}  // namespace net_instaweb
