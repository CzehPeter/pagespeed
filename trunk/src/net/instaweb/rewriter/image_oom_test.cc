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

// Unit tests for out-of-memory handling in Image class

#include "net/instaweb/rewriter/public/image.h"

#include <sys/time.h>
#include <sys/resource.h>

#include "net/instaweb/rewriter/public/image_test_base.h"
#include "pagespeed/kernel/base/dynamic_annotations.h"  // RunningOnValgrind
#include "pagespeed/kernel/base/gtest.h"
#include "pagespeed/kernel/base/mock_message_handler.h"
#include "pagespeed/kernel/base/mock_timer.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/image_types.pb.h"

namespace net_instaweb {
namespace {

const char kLargeJpeg[] = "Large.jpg";

class ImageOomTest : public ImageTestBase {
 public:
  virtual void SetUp() {
    ImageTestBase::SetUp();

    // All of these tests need to be disabled under valgrind since
    // valgrind and setrlimit don't get along
    if (!RunningOnValgrind()) {
      getrlimit(RLIMIT_AS, &old_mem_limit_);

      // Limit ourselves to about 100 million bytes of memory --- not enough
      // to fit in the 10000x10000 image (which has 100 million pixels).
      rlimit new_mem_limit = old_mem_limit_;
      new_mem_limit.rlim_cur = 100000000;
      setrlimit(RLIMIT_AS, &new_mem_limit);
    }
  }

  virtual void TearDown() {
    if (!RunningOnValgrind()) {
      // Restore previous rlimit
      setrlimit(RLIMIT_AS, &old_mem_limit_);
    }
    ImageTestBase::TearDown();
  }

 private:
  rlimit old_mem_limit_;
};

TEST_F(ImageOomTest, BlankImageTooLarge) {
  if (RunningOnValgrind()) {
    return;
  }

#ifndef NDEBUG
  return;
#endif

  Image::CompressionOptions* options = new Image::CompressionOptions();
  // Make sure creating gigantic image fails cleanly.
  ImagePtr giant(BlankImageWithOptions(10000000, 10000, IMAGE_PNG,
                                       GTestTempDir(), &timer_,
                                       &message_handler_, options));
  EXPECT_EQ(NULL, giant.get());
}

TEST_F(ImageOomTest, BlankImageNotTooLarge) {
  if (RunningOnValgrind()) {
    return;
  }

#ifndef NDEBUG
  return;
#endif

  Image::CompressionOptions* options = new Image::CompressionOptions();
  ImagePtr not_too_large(BlankImageWithOptions(4000, 4000, IMAGE_PNG,
                                               GTestTempDir(), &timer_,
                                               &message_handler_, options));
  // Image of this size can be created.
  EXPECT_NE(static_cast<net_instaweb::Image*>(NULL), not_too_large.get());
}

TEST_F(ImageOomTest, LoadLargeJpeg) {
  if (RunningOnValgrind()) {
    return;
  }

  GoogleString buf;
  bool not_progressive = false;
  ImagePtr giant(ReadImageFromFile(IMAGE_JPEG, kLargeJpeg, &buf,
                                   not_progressive));
  // We do not rewrite JPEG images of such large size, so the input and output
  // images have the same length.
  EXPECT_EQ(buf.length(), giant->output_size());
}

TEST_F(ImageOomTest, LoadLargePng) {
  if (RunningOnValgrind()) {
    return;
  }

  GoogleString buf;
  bool not_progressive = true;
  ImagePtr image(ReadImageFromFile(IMAGE_PNG, kLarge, &buf,
                                   not_progressive));
  // PNG images needs less memory to rewrite than JPEG. After rewriting
  // this image shrinks.
  EXPECT_GT(buf.length(), image->output_size());
}

}  // namespace
}  // namespace net_instaweb
