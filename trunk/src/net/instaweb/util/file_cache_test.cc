// Copyright 2010 and onwards Google Inc.
// Author: lsong@google.com (Libo Song)

// Unit-test the file cache

#include "net/instaweb/util/public/file_cache.h"
#include "base/basictypes.h"
#include "base/logging.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/filename_encoder.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/mem_file_system.h"
#include "net/instaweb/util/public/shared_string.h"
#include <string>

namespace net_instaweb {

class FileCacheTest : public testing::Test {
 protected:
  FileCacheTest()
      : cache_(GTestTempDir(), &file_system_, &filename_encoder_,
               &message_handler_) {
  }

  void CheckGet(const char* key, const std::string& expected_value) {
    SharedString value_buffer;
    EXPECT_TRUE(cache_.Get(key, &value_buffer));
    EXPECT_EQ(expected_value, *value_buffer);
    EXPECT_EQ(CacheInterface::kAvailable, cache_.Query(key));
  }

  void Put(const char* key, const char* value) {
    SharedString put_buffer(value);
    cache_.Put(key, &put_buffer);
  }

  void CheckNotFound(const char* key) {
    SharedString value_buffer;
    EXPECT_FALSE(cache_.Get(key, &value_buffer));
    EXPECT_EQ(CacheInterface::kNotFound, cache_.Query(key));
  }

 protected:
  MemFileSystem file_system_;
  FilenameEncoder filename_encoder_;
  FileCache cache_;
  GoogleMessageHandler message_handler_;

 private:
  DISALLOW_COPY_AND_ASSIGN(FileCacheTest);
};

// Simple flow of putting in an item, getting it, deleting it.
TEST_F(FileCacheTest, PutGetDelete) {
  Put("Name", "Value");
  CheckGet("Name", "Value");
  CheckNotFound("Another Name");

  Put("Name", "NewValue");
  CheckGet("Name", "NewValue");

  cache_.Delete("Name");
  SharedString value_buffer;
  EXPECT_FALSE(cache_.Get("Name", &value_buffer));
}

// Throw a bunch of files into the cache and verify that they are
// evicted sensibly.
TEST_F(FileCacheTest, Clean) {
  file_system_.Clear();
  // Make some "directory" entries so that the mem_file_system recurses
  // correctly.
  std::string dir1 = GTestTempDir() + "/a/";
  std::string dir2 = GTestTempDir() + "/b/";
  EXPECT_TRUE(file_system_.MakeDir(dir1.c_str(), &message_handler_));
  EXPECT_TRUE(file_system_.MakeDir(dir2.c_str(), &message_handler_));
  // Commonly-used keys
  const char* names1[] = {"a1", "a2", "a/3"};
  const char* values1[] = {"a2", "a234", "a2345678"};
  // Less common keys
  const char* names2[] =
      {"b/1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "b9"};
  const char* values2[] = {"b2", "b234", "b2345678",
                            "b2", "b234", "b2345678",
                            "b2", "b234", "b2345678"};
  for (int i = 0; i < 3; i++) {
    Put(names1[i], values1[i]);
  }
  for (int i = 0; i < 9; i++) {
    Put(names2[i], values2[i]);
  }
  int64 total_size = 0;
  EXPECT_TRUE(
      file_system_.RecursiveDirSize(GTestTempDir(), &total_size,
                                    &message_handler_));
  EXPECT_EQ((2 + 4 + 8) * 4, total_size);

  // Clean should not remove anything if target is bigger than total size.
  EXPECT_TRUE(cache_.Clean(total_size + 1));
  for (int i = 0; i < 27; i++) {
    // This pattern represents more common usage of the names1 files.
    CheckGet(names1[i % 3], values1[i % 3]);
    CheckGet(names2[i % 9], values2[i % 9]);
  }

  int64 target_size = total_size  / 1.25 - 1;
  EXPECT_TRUE(cache_.Clean(target_size));
  // Common files should stay
  for (int i = 0; i < 3; i++) {
    CheckGet(names1[i], values1[i]);
  }
  // Some of the less common files should be gone
  for (int i = 0; i < 3; i++) {
    CheckNotFound(names2[i]);
  }
}

}  // namespace net_instaweb
