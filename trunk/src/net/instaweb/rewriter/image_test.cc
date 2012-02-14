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
// Author: jmaessen@google.com (Jan Maessen)

// Unit tests for Image class used in rewriting.

#include "net/instaweb/rewriter/public/image.h"

#include <algorithm>

#include "base/scoped_ptr.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/image_data_lookup.h"
#include "net/instaweb/rewriter/public/image_rewrite_filter.h"
#include "net/instaweb/rewriter/public/image_test_base.h"
#include "net/instaweb/rewriter/public/image_url_encoder.h"
#include "net/instaweb/util/public/base64_util.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/dynamic_annotations.h"  // RunningOnValgrind
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/stdio_file_system.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace {

const char kProgressiveHeader[] = "\xFF\xC2";
const int kProgressiveHeaderStartIndex = 158;

enum ImageContext {
  kNoWebpNoMobile,
  kNoWebpMobile,
  kWebpNoMobile,
  kWebpMobile,
};
}  // namespace

namespace net_instaweb {

class ImageTest : public ImageTestBase {
 public:
  ImageTest() : options_(new Image::CompressionOptions()) {}

 protected:
  void ExpectEmptyOuput(Image* image) {
    EXPECT_FALSE(image->output_valid_);
    EXPECT_TRUE(image->output_contents_.empty());
  }

  void ExpectContentType(Image::Type image_type, Image* image) {
    EXPECT_EQ(image_type, image->image_type_);
  }

  void ExpectDimensions(Image::Type image_type, int size,
                        int expected_width, int expected_height,
                        Image *image) {
    EXPECT_EQ(size, image->input_size());
    EXPECT_EQ(image_type, image->image_type());
    ImageDim image_dim;
    image_dim.Clear();
    image->Dimensions(&image_dim);
    EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(image_dim));
    EXPECT_EQ(expected_width, image_dim.width());
    EXPECT_EQ(expected_height, image_dim.height());
    EXPECT_EQ(StringPrintf("%dx%dxZZ", image_dim.width(), image_dim.height()),
              EncodeUrlAndDimensions(kNoWebpNoMobile, "ZZ", image_dim));
  }

  void CheckInvalid(const GoogleString& name, const GoogleString& contents,
                    Image::Type input_type, Image::Type output_type,
                    bool progressive) {
    ImagePtr image(ImageFromString(output_type, name, contents, progressive));
    EXPECT_EQ(contents.size(), image->input_size());
    EXPECT_EQ(input_type, image->image_type());
    ImageDim  image_dim;
    image_dim.Clear();
    image->Dimensions(&image_dim);
    EXPECT_FALSE(ImageUrlEncoder::HasValidDimension(image_dim));
    EXPECT_FALSE(image_dim.has_width());
    EXPECT_FALSE(image_dim.has_height());
    EXPECT_EQ(contents.size(), image->output_size());
    EXPECT_EQ("xZZ", EncodeUrlAndDimensions(kNoWebpNoMobile, "ZZ", image_dim));
  }

  void CheckImageFromFile(const char* filename,
                          Image::Type input_type,
                          Image::Type output_type,
                          int min_bytes_to_type,
                          int min_bytes_to_dimensions,
                          int width, int height,
                          int size, bool optimizable) {
    options_->webp_preferred = output_type == Image::IMAGE_WEBP;
    options_->convert_png_to_jpeg = output_type == Image::IMAGE_JPEG;
    bool progressive = options_->progressive_jpeg;
    GoogleString contents;
    ImagePtr image(ReadFromFileWithOptions(
        filename, &contents, options_.release()));
    ExpectDimensions(input_type, size, width, height, image.get());
    if (optimizable) {
      EXPECT_GT(size, image->output_size());
      ExpectDimensions(output_type, size, width, height, image.get());
    } else {
      EXPECT_EQ(size, image->output_size());
      ExpectDimensions(input_type, size, width, height, image.get());
    }

    // Construct data url, then decode it and check for match.
    CachedResult cached;
    GoogleString data_url;
    EXPECT_NE(Image::IMAGE_UNKNOWN, image->image_type());
    StringPiece image_contents = image->Contents();

    if (progressive) {
      EXPECT_STREQ(kProgressiveHeader, image_contents.substr(
          kProgressiveHeaderStartIndex, strlen(kProgressiveHeader)));
    }

    cached.set_inlined_data(image_contents.data(), image_contents.size());
    cached.set_inlined_image_type(static_cast<int>(image->image_type()));
    EXPECT_TRUE(ImageRewriteFilter::TryInline(
        image->output_size() + 1, &cached, &data_url));
    GoogleString data_header("data:");
    data_header.append(image->content_type()->mime_type());
    data_header.append(";base64,");
    EXPECT_EQ(data_header, data_url.substr(0, data_header.size()));
    StringPiece encoded_contents(
        data_url.data() + data_header.size(),
        data_url.size() - data_header.size());
    GoogleString decoded_contents;
    EXPECT_TRUE(Mime64Decode(encoded_contents, &decoded_contents));
    EXPECT_EQ(image->Contents(), decoded_contents);

    // Now truncate the file in various ways and make sure we still
    // get partial data.
    GoogleString dim_data(contents, 0, min_bytes_to_dimensions);
    ImagePtr dim_image(
        ImageFromString(output_type, filename, dim_data, progressive));
    ExpectDimensions(input_type, min_bytes_to_dimensions, width, height,
                     dim_image.get());
    EXPECT_EQ(min_bytes_to_dimensions, dim_image->output_size());

    GoogleString no_dim_data(contents, 0, min_bytes_to_dimensions - 1);
    CheckInvalid(filename, no_dim_data, input_type, output_type, progressive);
    GoogleString type_data(contents, 0, min_bytes_to_type);
    CheckInvalid(filename, type_data, input_type, output_type, progressive);
    GoogleString junk(contents, 0, min_bytes_to_type - 1);
    CheckInvalid(filename, junk, Image::IMAGE_UNKNOWN, Image::IMAGE_UNKNOWN,
                 progressive);
  }

  GoogleString EncodeUrlAndDimensions(
      ImageContext image_context, const StringPiece& origin_url,
      const ImageDim& dim) {
    StringVector v;
    v.push_back(origin_url.as_string());
    GoogleString out;
    ResourceContext data;
    *data.mutable_desired_image_dims() = dim;
    data.set_attempt_webp(IsWebp(image_context));
    data.set_mobile_user_agent(IsMobile(image_context));
    encoder_.Encode(v, &data, &out);
    return out;
  }

  bool DecodeUrlAndDimensions(ImageContext expected_image_context,
                              const StringPiece& encoded,
                              ImageDim* dim,
                              GoogleString* url) {
    ResourceContext context;
    StringVector urls;
    bool result = encoder_.Decode(encoded, &urls, &context, &handler_);
    if (result) {
      EXPECT_EQ(IsWebp(expected_image_context), context.attempt_webp());
      EXPECT_EQ(IsMobile(expected_image_context), context.mobile_user_agent());
      EXPECT_EQ(1, urls.size());
      url->assign(urls.back());
      *dim = context.desired_image_dims();
    }
    return result;
  }

  void ExpectBadDim(const StringPiece& url) {
    GoogleString origin_url;
    ImageDim dim;
    EXPECT_FALSE(DecodeUrlAndDimensions(kNoWebpNoMobile,
                                        url, &dim, &origin_url));
    EXPECT_FALSE(ImageUrlEncoder::HasValidDimension(dim));
  }

  StdioFileSystem file_system_;
  GoogleMessageHandler handler_;
  ImageUrlEncoder encoder_;
  scoped_ptr<Image::CompressionOptions> options_;

 private:
  bool IsWebp(ImageContext context) {
    return context == kWebpMobile || context == kWebpNoMobile;
  }

  bool IsMobile(ImageContext context) {
    return context == kWebpMobile || context == kNoWebpMobile;
  }

  DISALLOW_COPY_AND_ASSIGN(ImageTest);
};

TEST_F(ImageTest, EmptyImageUnidentified) {
  CheckInvalid("Empty string", "", Image::IMAGE_UNKNOWN, Image::IMAGE_UNKNOWN,
               false);
}

TEST_F(ImageTest, InputWebpTest) {
  CheckImageFromFile(
      kScenery, Image::IMAGE_WEBP, Image::IMAGE_WEBP,
      20,  // Min bytes to bother checking file type at all.
      30,
      550, 368,
      30320, false);
}

// FYI: Takes ~20000 ms to run under Valgrind.
TEST_F(ImageTest, WebpLowResTest) {
  GoogleString contents;
  ImagePtr image(ReadImageFromFile(Image::IMAGE_WEBP, kScenery, &contents,
                                   false));
  int filesize = 30320;
  image->SetTransformToLowRes();
  EXPECT_GT(filesize, image->output_size());
}

TEST_F(ImageTest, PngTest) {
  CheckImageFromFile(
      kBikeCrash, Image::IMAGE_PNG, Image::IMAGE_PNG,
      ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2,
      100, 100,
      26548, true);
}

TEST_F(ImageTest, PngToJpegTest) {
  options_->jpeg_quality = 85;
  CheckImageFromFile(
      kBikeCrash, Image::IMAGE_PNG, Image::IMAGE_JPEG,
      ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2,
      100, 100,
      26548, true);
}

TEST_F(ImageTest, PngToProgressiveJpegTest) {
  options_->progressive_jpeg = true;
  options_->jpeg_quality = 85;
  CheckImageFromFile(
      kBikeCrash, Image::IMAGE_PNG, Image::IMAGE_JPEG,
      ImageHeaders::kPngHeaderLength,
      ImageHeaders::kIHDRDataStart + ImageHeaders::kPngIntSize * 2,
      100, 100,
      26548, true);
}

TEST_F(ImageTest, GifTest) {
  CheckImageFromFile(
      kIronChef, Image::IMAGE_GIF, Image::IMAGE_PNG,
      8,  // Min bytes to bother checking file type at all.
      ImageHeaders::kGifDimStart + ImageHeaders::kGifIntSize * 2,
      192, 256,
      24941, true);
}

TEST_F(ImageTest, AnimationTest) {
  CheckImageFromFile(
      kCradle, Image::IMAGE_GIF, Image::IMAGE_PNG,
      8,  // Min bytes to bother checking file type at all.
      ImageHeaders::kGifDimStart + ImageHeaders::kGifIntSize * 2,
      200, 150,
      583374, false);
}

TEST_F(ImageTest, JpegTest) {
  CheckImageFromFile(
      kPuzzle, Image::IMAGE_JPEG, Image::IMAGE_JPEG,
      8,  // Min bytes to bother checking file type at all.
      6468,  // Specific to this test
      1023, 766,
      241260, true);
}

TEST_F(ImageTest, ProgressiveJpegTest) {
  CheckImageFromFile(
      kPuzzle, Image::IMAGE_JPEG, Image::IMAGE_JPEG,
      8,  // Min bytes to bother checking file type at all.
      6468,  // Specific to this test
      1023, 766,
      241260, true);
}

// FYI: Takes ~70000 ms to run under Valgrind.
TEST_F(ImageTest, WebpTest) {
  CheckImageFromFile(
      kPuzzle, Image::IMAGE_JPEG, Image::IMAGE_WEBP,
      8,  // Min bytes to bother checking file type at all.
      6468,  // Specific to this test
      1023, 766,
      241260, true);
}

TEST_F(ImageTest, DrawImage) {
  GoogleString buf1;
  ImagePtr image1(ReadImageFromFile(Image::IMAGE_PNG, kBikeCrash, &buf1,
                                    false));
  ImageDim image_dim1;
  image1->Dimensions(&image_dim1);

  GoogleString buf2;
  ImagePtr image2(ReadImageFromFile(Image::IMAGE_PNG, kCuppa, &buf2,
                                    false));
  ImageDim image_dim2;
  image2->Dimensions(&image_dim2);

  int width = std::max(image_dim1.width(), image_dim2.width());
  int height = image_dim1.height() + image_dim2.height();
  ASSERT_GT(width, 0);
  ASSERT_GT(height, 0);
  ImagePtr canvas(BlankImage(width, height, Image::IMAGE_PNG,
                             GTestTempDir(), &handler_));
  EXPECT_TRUE(canvas->DrawImage(image1.get(), 0, 0));
  EXPECT_TRUE(canvas->DrawImage(image2.get(), 0, image_dim1.height()));
  // The combined image should be bigger than either of the components, but
  // smaller than their unoptimized sum.
  EXPECT_GT(canvas->output_size(), image1->output_size());
  EXPECT_GT(canvas->output_size(), image2->output_size());
  EXPECT_GT(image1->input_size() + image2->input_size(),
            canvas->output_size());
}


const char kActualUrl[] = "http://encoded.url/with/various.stuff";

TEST_F(ImageTest, NoDims) {
  const char kNoDimsUrl[] = "x,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(
      kNoWebpNoMobile, kNoDimsUrl, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kNoDimsUrl,
            EncodeUrlAndDimensions(kNoWebpNoMobile, origin_url, dim));
}

TEST_F(ImageTest, NoDimsWebp) {
  const char kNoDimsUrl[] = "w,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kWebpNoMobile,
                                     kNoDimsUrl, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kNoDimsUrl, EncodeUrlAndDimensions(kWebpNoMobile, origin_url, dim));
}

TEST_F(ImageTest, NoDimsMobile) {
  const char kNoDimsUrl[] = "mx,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(
      kNoWebpMobile, kNoDimsUrl, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kNoDimsUrl, EncodeUrlAndDimensions(kNoWebpMobile, origin_url, dim));
}

TEST_F(ImageTest, NoDimsWebpMobile) {
  const char kNoDimsUrl[] = "mw,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(
      kWebpMobile, kNoDimsUrl, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kNoDimsUrl, EncodeUrlAndDimensions(kWebpMobile, origin_url, dim));
}

TEST_F(ImageTest, HasDims) {
  const char kDimsUrl[] = "17x33x,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(
      kNoWebpNoMobile, kDimsUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(kNoWebpNoMobile, origin_url, dim));
}

TEST_F(ImageTest, HasDimsWebp) {
  const char kDimsUrl[] = "17x33w,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(
      kWebpNoMobile, kDimsUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(kWebpNoMobile, origin_url, dim));
}

TEST_F(ImageTest, HasDimsMobile) {
  const char kDimsUrl[] = "17x33mx,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(
      kNoWebpMobile, kDimsUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(kNoWebpMobile, origin_url, dim));
}

TEST_F(ImageTest, HasDimsWebpMobile) {
  const char kDimsUrl[] = "17x33mw,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(
      kWebpMobile, kDimsUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(kWebpMobile, origin_url, dim));
}
TEST_F(ImageTest, HasWidth) {
  const char kWidthUrl[] = "17xNx,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kNoWebpNoMobile,
                                     kWidthUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(-1, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kWidthUrl, EncodeUrlAndDimensions(kNoWebpNoMobile,
                                              origin_url, dim));
}

TEST_F(ImageTest, HasWidthWebp) {
  const char kWidthUrl[] = "17xNw,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kWebpNoMobile,
                                     kWidthUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(-1, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kWidthUrl, EncodeUrlAndDimensions(kWebpNoMobile, origin_url, dim));
}

TEST_F(ImageTest, HasHeight) {
  const char kHeightUrl[] = "Nx33x,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kNoWebpNoMobile,
                                     kHeightUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(-1, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kHeightUrl, EncodeUrlAndDimensions(kNoWebpNoMobile,
                                               origin_url, dim));
}

TEST_F(ImageTest, HasHeightWebp) {
  const char kHeightUrl[] = "Nx33w,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kWebpNoMobile,
                                     kHeightUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimension(dim));
  EXPECT_EQ(-1, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kHeightUrl, EncodeUrlAndDimensions(kWebpNoMobile, origin_url, dim));
}

TEST_F(ImageTest, BadFirst) {
  const char kBadFirst[] = "badx33x,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageTest, BadFirstWebp) {
  const char kBadFirst[] = "badx33w,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageTest, BadFirstMobile) {
  const char kBadFirst[] = "badx33mx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageTest, BadFirstWebpMobile) {
  const char kBadFirst[] = "badx33mw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadFirst);
}

TEST_F(ImageTest, BadSecond) {
  const char kBadSecond[] = "17xbadx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageTest, BadSecondWebp) {
  const char kBadSecond[] = "17xbadw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageTest, BadSecondMobile) {
  const char kBadSecond[] = "17xbadmx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageTest, BadSecondWebpMobile) {
  const char kBadSecond[] = "17xbadmw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadSecond);
}

TEST_F(ImageTest, BadLeadingN) {
  const char kBadLeadingN[] = "Nxw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadLeadingN);
}

TEST_F(ImageTest, BadMiddleN) {
  const char kBadMiddleN[] = "17xN,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBadMiddleN);
}

TEST_F(ImageTest, NoXs) {
  const char kNoXs[] = ",hencoded.url,_with,_various.stuff";
  ExpectBadDim(kNoXs);
}

TEST_F(ImageTest, NoXsMoble) {
  const char kNoXs[] = "m,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kNoXs);
}

TEST_F(ImageTest, BlankSecond) {
  const char kBlankSecond[] = "17xx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBlankSecond);
}

TEST_F(ImageTest, BadSizeCheck) {
  // Catch case where url size check was inverted.
  const char kBadSize[] = "17xx";
  ExpectBadDim(kBadSize);
}

TEST_F(ImageTest, BlankSecondWebp) {
  const char kBlankSecond[] = "17xw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBlankSecond);
}

TEST_F(ImageTest, BlankSecondMobile) {
  const char kBlankSecond[] = "17xmx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBlankSecond);
}

TEST_F(ImageTest, BlankSecondWebpMobile) {
  const char kBlankSecond[] = "17xmw,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kBlankSecond);
}

TEST_F(ImageTest, BadTrailChar) {
  const char kDimsUrl[] = "17x33u,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kDimsUrl);
}

TEST_F(ImageTest, BadInitChar) {
  const char kNoDimsUrl[] = "u,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kNoDimsUrl);
}

TEST_F(ImageTest, BadWidthChar) {
  const char kWidthUrl[] = "17u,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kWidthUrl);
}

TEST_F(ImageTest, BadHeightChar) {
  const char kHeightUrl[] = "Nx33u,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kHeightUrl);
}

TEST_F(ImageTest, ShortBothDims) {
  const char kShortUrl[] = "17x33";
  ExpectBadDim(kShortUrl);
}

TEST_F(ImageTest, ShortWidth) {
  const char kShortWidth[] = "Nx33";
  ExpectBadDim(kShortWidth);
}

TEST_F(ImageTest, ShortHeight) {
  const char kShortHeight[] = "17xN";
  ExpectBadDim(kShortHeight);
}

TEST_F(ImageTest, BothDimsMissing) {
  const char kNeitherUrl[] = "NxNx,hencoded.url,_with,_various.stuff";
  ExpectBadDim(kNeitherUrl);
}

TEST_F(ImageTest, VeryShortUrl) {
  const char kVeryShortUrl[] = "7x3";
  ExpectBadDim(kVeryShortUrl);
}

TEST_F(ImageTest, TruncatedAfterFirstDim) {
  const char kTruncatedUrl[] = "175x";
  ExpectBadDim(kTruncatedUrl);
}

TEST_F(ImageTest, TruncatedBeforeSep) {
  const char kTruncatedUrl[] = "12500";
  ExpectBadDim(kTruncatedUrl);
}

// Test OpenCV bug where width * height of image could be allocated on the
// stack. kLarge is a 10000x10000 image, so it will try to allocate > 100MB
// on the stack, which should overflow the stack and SEGV.
TEST_F(ImageTest, OpencvStackOverflow) {
  // This test takes ~90000 ms on Valgrind and need not be run there.
  if (RunningOnValgrind()) {
    return;
  }

  GoogleString buf;
  ImagePtr image(ReadImageFromFile(Image::IMAGE_JPEG, kLarge, &buf, false));

  ImageDim new_dim;
  new_dim.set_width(1);
  new_dim.set_height(1);
  image->ResizeTo(new_dim);
}

TEST_F(ImageTest, ResizeTo) {
  GoogleString buf;
  ImagePtr image(ReadImageFromFile(Image::IMAGE_JPEG, kPuzzle, &buf, false));

  ImageDim new_dim;
  new_dim.set_width(10);
  new_dim.set_height(10);
  image->ResizeTo(new_dim);

  ExpectEmptyOuput(image.get());
  ExpectContentType(Image::IMAGE_JPEG, image.get());
}

}  // namespace net_instaweb
