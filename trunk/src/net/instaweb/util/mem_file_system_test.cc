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

// Author: abliss@google.com (Adam Bliss)

// Unit-test the in-memory filesystem

#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/file_system_test.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/mem_file_system.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class MemFileSystemTest : public FileSystemTest {
 protected:
  MemFileSystemTest()
      : timer_(0),
        mem_file_system_(&timer_) {
    mem_file_system_.set_advance_time_on_update(true);
  }
  virtual void DeleteRecursively(const StringPiece& filename) {
    mem_file_system_.Clear();
  }
  virtual FileSystem* file_system() {
    return &mem_file_system_;
  }
  virtual GoogleString test_tmpdir() {
    return GTestTempDir();
  }
 private:
  MockTimer timer_;
  MemFileSystem mem_file_system_;

  DISALLOW_COPY_AND_ASSIGN(MemFileSystemTest);
};

// Write a named file, then read it.
TEST_F(MemFileSystemTest, TestWriteRead) {
  TestWriteRead();
}

// Write a temp file, then read it.
TEST_F(MemFileSystemTest, TestTemp) {
  TestTemp();
}

// Write a temp file, rename it, then read it.
TEST_F(MemFileSystemTest, TestRename) {
  TestRename();
}

// Write a file and successfully delete it.
TEST_F(MemFileSystemTest, TestRemove) {
  TestRemove();
}

// Write a file and check that it exists.
TEST_F(MemFileSystemTest, TestExists) {
  TestExists();
}

// Create a file along with its directory which does not exist.
TEST_F(MemFileSystemTest, TestCreateFileInDir) {
  TestCreateFileInDir();
}


// Make a directory and check that files may be placed in it.
TEST_F(MemFileSystemTest, TestMakeDir) {
  TestMakeDir();
}

TEST_F(MemFileSystemTest, TestSize) {
  // Since we don't have directories, we need to do a slightly
  // different size test.
  GoogleString filename1 = "file-in-dir.txt";
  GoogleString filename2 = "another-file-in-dir.txt";
  GoogleString content1 = "12345";
  GoogleString content2 = "1234567890";
  ASSERT_TRUE(file_system()->WriteFile(filename1.c_str(),
                                       content1, &handler_));
  ASSERT_TRUE(file_system()->WriteFile(filename2.c_str(),
                                       content2, &handler_));
  int64 size;

  EXPECT_TRUE(file_system()->Size(filename1, &size, &handler_));
  EXPECT_EQ(5, size);
  EXPECT_TRUE(file_system()->Size(filename2, &size, &handler_));
  EXPECT_EQ(10, size);
}

TEST_F(MemFileSystemTest, TestListContents) {
  TestListContents();
}

TEST_F(MemFileSystemTest, TestAtime) {
  // Slightly modified version of TestAtime, without the sleeps
  GoogleString dir_name = test_tmpdir() + "/make_dir";
  DeleteRecursively(dir_name);
  GoogleString filename1 = "file-in-dir.txt";
  GoogleString filename2 = "another-file-in-dir.txt";
  GoogleString full_path1 = dir_name + "/" + filename1;
  GoogleString full_path2 = dir_name + "/" + filename2;
  GoogleString content = "Lorem ipsum dolor sit amet";

  ASSERT_TRUE(file_system()->MakeDir(dir_name.c_str(), &handler_));
  ASSERT_TRUE(file_system()->WriteFile(full_path1.c_str(),
                                       content, &handler_));
  ASSERT_TRUE(file_system()->WriteFile(full_path2.c_str(),
                                       content, &handler_));

  int64 atime1, atime2;
  CheckRead(full_path1, content);
  CheckRead(full_path2, content);
  ASSERT_TRUE(file_system()->Atime(full_path1, &atime1, &handler_));
  ASSERT_TRUE(file_system()->Atime(full_path2, &atime2, &handler_));
  EXPECT_LT(atime1, atime2);

  CheckRead(full_path2, content);
  CheckRead(full_path1, content);
  ASSERT_TRUE(file_system()->Atime(full_path1, &atime1, &handler_));
  ASSERT_TRUE(file_system()->Atime(full_path2, &atime2, &handler_));
  EXPECT_LT(atime2, atime1);
}

TEST_F(MemFileSystemTest, TestLock) {
  TestLock();
}

// Since this filesystem doesn't support directories, we skip these tests:
// TestIsDir
// TestRecursivelyMakeDir
// TestRecursivelyMakeDir_NoPermission
// TestRecursivelyMakeDir_FileInPath

}  // namespace net_instaweb
