/*
 * Copyright 2014 Google Inc.
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

// Author: Huibao Lin

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/gtest.h"
#include "pagespeed/kernel/base/mock_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/image/image_analysis.h"
#include "pagespeed/kernel/image/scanline_utils.h"
#include "pagespeed/kernel/image/test_utils.h"

namespace {

using net_instaweb::MessageHandler;
using net_instaweb::MockMessageHandler;
using net_instaweb::NullMutex;
using pagespeed::image_compression::GRAY_8;
using pagespeed::image_compression::Histogram;
using pagespeed::image_compression::kNumColorHistogramBins;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::RGBA_8888;
using pagespeed::image_compression::SynthesizeImage;

class ImageAnalysisTest : public testing::Test {
 public:
  ImageAnalysisTest() :
      message_handler_(new NullMutex) {
    // Initialize the expected histogram.
    for (int i = 0; i < kNumColorHistogramBins; ++i) {
      expected_hist_[i] = 0.0f;
    }
  }

 protected:
  // Synthesize an image; compute its gradient; and verify that the
  // gradient has the expected value.
  void TestGradient(int width, int height, PixelFormat pixel_format,
                    int bytes_per_line, const uint8_t* seed_value,
                    const int* delta_x, const int* delta_y,
                    const uint8_t* expected_gradient) {
    const int num_channels =
      GetNumChannelsFromPixelFormat(pixel_format, &message_handler_);

    // Synthesize the image.
    net_instaweb::scoped_array<uint8_t> image(
        new uint8_t[bytes_per_line * height]);
    SynthesizeImage(width, height, bytes_per_line, num_channels,
                    seed_value, delta_x, delta_y, image.get());

    // Compute gradient.
    net_instaweb::scoped_array<uint8_t> gradient(new uint8_t[width * height]);
    ASSERT_TRUE(SimpleGradient(image.get(), width, height, bytes_per_line,
                               pixel_format, &message_handler_,
                               gradient.get()));

    // Verify the gradient.
    EXPECT_EQ(0, memcmp(gradient.get(), expected_gradient,
                        width * height * sizeof(gradient[0])));
  }

 protected:
  MockMessageHandler message_handler_;
  float expected_hist_[kNumColorHistogramBins];

 private:
  DISALLOW_COPY_AND_ASSIGN(ImageAnalysisTest);
};

TEST_F(ImageAnalysisTest, GradientOfWhiteImage) {
  int width = 9;
  int height = 5;
  int bytes_per_line = 12;  // End of scanline will have garbage data
  const uint8_t seed_value[] = {255};
  const int delta_x[] = {0};
  const int delta_y[] = {0};
  const PixelFormat pixel_format = GRAY_8;
  const int num_channels =
    GetNumChannelsFromPixelFormat(pixel_format, &message_handler_);

  uint8_t seed[] = {0};
  net_instaweb::scoped_array<uint8_t> expected_gradient(
      new uint8_t[width * height]);
  SynthesizeImage(width, height, width, num_channels, seed,
                  delta_x, delta_y, expected_gradient.get());

  TestGradient(width, height, GRAY_8, bytes_per_line, seed_value,
               delta_x, delta_y, expected_gradient.get());
}

TEST_F(ImageAnalysisTest, GradientOfIncreasingPixelValues) {
  int width = 11;
  int height = 6;
  const uint8_t seed_value[] = {0, 20, 40, 100};
  const int delta_x[] = {1, 2, 3, 24};
  const int delta_y[] = {10, 20, 30, 123};
  const PixelFormat pixel_format[] = {RGB_888, RGBA_8888};
  // "bytes_per_line" may be larger than the size of the memory for holding
  // the pixels, so the end of scanline may have garbage data.
  const int bytes_per_line[] = {36, 44};

  // Generate ground truth. The synthesized image has RGB channels,
  // so the gradient is equal to
  //   max( 2 * (delta_x[0] + delta_x[1] + delta_x[2]) / 3,
  //        2 * (delta_y[0] + delta_y[1] + delta_y[2]) / 3 )
  // which is 40.
  net_instaweb::scoped_array<uint8_t> expected_gradient(
      new uint8_t[width * height]);
  memset(expected_gradient.get(), 0, width * height);
  for (int y = 1; y < height - 1; ++y) {
    memset(expected_gradient.get() + y * width + 1, 40, width - 2);
  }

  // Test the gradient computed for two pixel formats: RGB_888 and RGBA_8888.
  // Since the alpha channel is ignored, both formats have the same gradient.
  for (int format = 0; format < 2; ++format) {
    TestGradient(width, height, pixel_format[format], bytes_per_line[format],
                 seed_value, delta_x, delta_y, expected_gradient.get());
  }
}

TEST_F(ImageAnalysisTest, GradientOfFluctuatingPixelValues) {
  int width = 6;
  int height = 5;
  const uint8_t seed_value[] = {128, 128, 128, 100};
  const int delta_x[] = {-30, 45, -51, 24};
  const int delta_y[] = {-42, -20, 50, 123};
  const int bytes_per_line[] = {18, 24};
  const PixelFormat pixel_format[] = {RGB_888, RGBA_8888};

  const uint8_t expected_gradient[] = {
      0,   0,   0,   0,   0, 0,
      0,  24,  78,  78,  24, 0,
      0,  78,  78, 110,  24, 0,
      0, 147,  24,  24, 110, 0,
      0,   0,   0,   0,   0, 0};

  // Test the gradient computed for two pixel formats: RGB_888 and RGBA_8888.
  // Since the alpha channel is ignored, both formats have the same gradient.
  for (int format = 0; format < 2; ++format) {
    TestGradient(width, height, pixel_format[format], bytes_per_line[format],
                 seed_value, delta_x, delta_y, expected_gradient);
  }
}

TEST_F(ImageAnalysisTest, HistogramOfBlankImage) {
  int width = 9;
  int height = 5;
  int bytes_per_line = 12;  // End of scanline will have garbage data
  int num_channels = 1;
  const uint8_t seed_value[] = {123};
  const int delta_x[] = {0};
  const int delta_y[] = {0};

  net_instaweb::scoped_array<uint8_t> image(
      new uint8_t[bytes_per_line * height]);
  SynthesizeImage(width, height, bytes_per_line, num_channels, seed_value,
                  delta_x, delta_y, image.get());

  // Ground truth. All of the pixels have value of 123, so only one bin
  // has non-zero value (which is 1).
  expected_hist_[seed_value[0]] = 1.0f;

  float hist[kNumColorHistogramBins];
  const int x0 = 1;
  const int y0 = 2;
  Histogram(image.get(), width-x0, height-y0, bytes_per_line, x0, y0, hist);
  EXPECT_EQ(0, memcmp(expected_hist_, hist,
                      kNumColorHistogramBins * sizeof(hist[0])));
}

TEST_F(ImageAnalysisTest, HistogramOfIncreasingPixelValues) {
  int width = 9;
  int height = 5;
  int bytes_per_line = 12;  // End of scanline will have garbage data
  int num_channels = 1;
  const uint8_t seed_value[] = {123};
  const int delta_x[] = {1};
  const int delta_y[] = {9};

  net_instaweb::scoped_array<uint8_t> image(
      new uint8_t[bytes_per_line * height]);
  SynthesizeImage(width, height, bytes_per_line, num_channels, seed_value,
                  delta_x, delta_y, image.get());

  // Generate ground truth. The pixels have contiguous values, so
  // the histogram has a flat region.
  for (int i = seed_value[0]; i < seed_value[0] + width * height; ++i) {
    expected_hist_[i] = 1.0f / (width * height);
  }

  float hist[kNumColorHistogramBins];
  Histogram(image.get(), width, height, bytes_per_line, 0, 0, hist);
  EXPECT_EQ(0, memcmp(expected_hist_, hist,
                      kNumColorHistogramBins * sizeof(hist[0])));
}

}  // namespace
