// Copyright 2010 Google Inc. All Rights Reserved.
// Author: jmaessen@google.com (Jan Maessen)

// Unit tests for Image class used in rewriting.

#include "net/instaweb/rewriter/public/image.h"

#include <algorithm>

#include "base/scoped_ptr.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/image_data_lookup.h"
#include "net/instaweb/rewriter/public/image_rewrite_filter.h"
#include "net/instaweb/rewriter/public/image_url_encoder.h"
#include "net/instaweb/util/public/base64_util.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/stdio_file_system.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace {

const char kTestData[] = "/net/instaweb/rewriter/testdata/";
const char kCuppa[] = "Cuppa.png";
const char kBikeCrash[] = "BikeCrashIcn.png";
const char kIronChef[] = "IronChef2.gif";
const char kCradle[] = "CradleAnimation.gif";
const char kPuzzle[] = "Puzzle.jpg";
const char kLarge[] = "Large.png";

}  // namespace

namespace net_instaweb {

const bool kNoWebp = false;
const bool kWebp   = true;

class ImageTest : public testing::Test {
 protected:
  typedef scoped_ptr<Image> ImagePtr;

  ImageTest() { }

  // We use the output_type (ultimate expected output type after image
  // processing) to set up rewrite permissions for the resulting Image object.
  Image* ImageFromString(Image::Type output_type,
                         const GoogleString& name,
                         const GoogleString& contents) {
    return NewImage(contents, name, GTestTempDir(),
                    output_type == Image::IMAGE_WEBP, -1, &handler_);
  }

  void ExpectDimensions(Image::Type image_type, int size,
                        int expected_width, int expected_height,
                        Image *image) {
    EXPECT_EQ(size, image->input_size());
    EXPECT_EQ(image_type, image->image_type());
    // Arbitrary but bogus values to make sure we get dimensions.
    ImageDim image_dim;
    image_dim.set_width(-7);
    image_dim.set_height(-9);
    image_dim.Clear();
    image->Dimensions(&image_dim);
    EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(image_dim));
    EXPECT_EQ(expected_width, image_dim.width());
    EXPECT_EQ(expected_height, image_dim.height());
    EXPECT_EQ(StringPrintf("%dx%dxZZ", image_dim.width(), image_dim.height()),
              EncodeUrlAndDimensions(kNoWebp, "ZZ", image_dim));
  }

  void CheckInvalid(const GoogleString& name, const GoogleString& contents,
                    Image::Type input_type, Image::Type output_type) {
    ImagePtr image(ImageFromString(output_type, name, contents));
    EXPECT_EQ(contents.size(), image->input_size());
    EXPECT_EQ(input_type, image->image_type());
    // Arbitrary but bogus values to check for accidental modification.
    ImageDim  image_dim;
    image_dim.set_width(-7);
    image_dim.set_height(-9);
    image_dim.Clear();
    image->Dimensions(&image_dim);
    EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(image_dim));
    EXPECT_FALSE(image_dim.has_width());
    EXPECT_FALSE(image_dim.has_height());
    EXPECT_EQ(contents.size(), image->output_size());
    EXPECT_EQ("xZZ", EncodeUrlAndDimensions(kNoWebp, "ZZ", image_dim));
  }

  // We use the output_type (ultimate expected output type after image
  // processing) to set up rewrite permissions for the resulting Image object.
  Image* ReadImageFromFile(Image::Type output_type,
                           const char* filename, GoogleString* buffer) {
    EXPECT_TRUE(file_system_.ReadFile(
        StrCat(GTestSrcDir(), kTestData, filename).c_str(),
        buffer, &handler_));
    return ImageFromString(output_type, filename, *buffer);
  }

  void CheckImageFromFile(const char* filename,
                          Image::Type input_type,
                          Image::Type output_type,
                          int min_bytes_to_type,
                          int min_bytes_to_dimensions,
                          int width, int height,
                          int size, bool optimizable) {
    GoogleString contents;
    ImagePtr image(ReadImageFromFile(output_type, filename, &contents));
    ExpectDimensions(input_type, size, width, height, image.get());
    if (optimizable) {
      EXPECT_GT(size, image->output_size());
      ExpectDimensions(output_type, size, width, height, image.get());
    } else {
      EXPECT_EQ(size, image->output_size());
      ExpectDimensions(input_type, size, width, height, image.get());
    }

    // Construct data url, then decode it and check for match.
    GoogleString data_url;
    EXPECT_FALSE(NULL == image->content_type());
    EXPECT_TRUE(ImageRewriteFilter::CanInline(
        image->output_size(), image->Contents(), image->content_type(),
        &data_url));
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
    ImagePtr dim_image(ImageFromString(output_type, filename, dim_data));
    ExpectDimensions(input_type, min_bytes_to_dimensions, width, height,
                     dim_image.get());
    EXPECT_EQ(min_bytes_to_dimensions, dim_image->output_size());

    GoogleString no_dim_data(contents, 0, min_bytes_to_dimensions - 1);
    CheckInvalid(filename, no_dim_data, input_type, output_type);
    GoogleString type_data(contents, 0, min_bytes_to_type);
    CheckInvalid(filename, type_data, input_type, output_type);
    GoogleString junk(contents, 0, min_bytes_to_type - 1);
    CheckInvalid(filename, junk, Image::IMAGE_UNKNOWN, Image::IMAGE_UNKNOWN);
  }

  GoogleString EncodeUrlAndDimensions(
      bool use_webp, const StringPiece& origin_url, const ImageDim& dim) {
    StringVector v;
    v.push_back(origin_url.as_string());
    GoogleString out;
    ResourceContext data;
    *data.mutable_image_tag_dims() = dim;
    data.set_attempt_webp(use_webp);
    encoder_.Encode(v, &data, &out);
    return out;
  }

  bool DecodeUrlAndDimensions(bool expect_webp,
                              const StringPiece& encoded,
                              ImageDim* dim,
                              GoogleString* url) {
    ResourceContext context;
    StringVector urls;
    bool result = encoder_.Decode(encoded, &urls, &context, &handler_);
    if (result) {
      EXPECT_EQ(expect_webp, context.attempt_webp());
      EXPECT_EQ(1, urls.size());
      url->assign(urls.back());
      *dim = context.image_tag_dims();
    }
    return result;
  }

  StdioFileSystem file_system_;
  GoogleMessageHandler handler_;
  ImageUrlEncoder encoder_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ImageTest);
};

TEST_F(ImageTest, EmptyImageUnidentified) {
  CheckInvalid("Empty string", "", Image::IMAGE_UNKNOWN, Image::IMAGE_UNKNOWN);
}

TEST_F(ImageTest, PngTest) {
  CheckImageFromFile(
      kBikeCrash, Image::IMAGE_PNG, Image::IMAGE_PNG,
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
  ImagePtr image1(ReadImageFromFile(Image::IMAGE_PNG, kBikeCrash, &buf1));
  ImageDim image_dim1;
  image1->Dimensions(&image_dim1);

  GoogleString buf2;
  ImagePtr image2(ReadImageFromFile(Image::IMAGE_PNG, kCuppa, &buf2));
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
  EXPECT_TRUE(DecodeUrlAndDimensions(kNoWebp, kNoDimsUrl, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kNoDimsUrl, EncodeUrlAndDimensions(kNoWebp, origin_url, dim));
}

TEST_F(ImageTest, NoDimsWebp) {
  const char kNoDimsUrl[] = "w,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kWebp, kNoDimsUrl, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kNoDimsUrl, EncodeUrlAndDimensions(kWebp, origin_url, dim));
}

TEST_F(ImageTest, HasDims) {
  const char kDimsUrl[] = "17x33x,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kNoWebp, kDimsUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(kNoWebp, origin_url, dim));
}

TEST_F(ImageTest, HasDimsWebp) {
  const char kDimsUrl[] = "17x33w,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_TRUE(DecodeUrlAndDimensions(kWebp, kDimsUrl, &dim, &origin_url));
  EXPECT_TRUE(ImageUrlEncoder::HasValidDimensions(dim));
  EXPECT_EQ(17, dim.width());
  EXPECT_EQ(33, dim.height());
  EXPECT_EQ(kActualUrl, origin_url);
  EXPECT_EQ(kDimsUrl, EncodeUrlAndDimensions(kWebp, origin_url, dim));
}

TEST_F(ImageTest, BadFirst) {
  const char kBadFirst[] = "badx33x,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_FALSE(DecodeUrlAndDimensions(kNoWebp, kBadFirst, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
}

TEST_F(ImageTest, BadFirstWebp) {
  const char kBadFirst[] = "badx33w,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_FALSE(DecodeUrlAndDimensions(kWebp, kBadFirst, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
}

TEST_F(ImageTest, BadSecond) {
  const char kBadSecond[] = "17xbadx,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_FALSE(DecodeUrlAndDimensions(kNoWebp, kBadSecond, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
}

TEST_F(ImageTest, BadSecondWebp) {
  const char kBadSecond[] = "17xbadw,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_FALSE(DecodeUrlAndDimensions(kWebp, kBadSecond, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
}

TEST_F(ImageTest, NoXs) {
  const char kNoXs[] = ",hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_FALSE(DecodeUrlAndDimensions(kNoWebp, kNoXs, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
}

TEST_F(ImageTest, BlankSecond) {
  const char kBlankSecond[] = "17xx,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_FALSE(
      DecodeUrlAndDimensions(kNoWebp, kBlankSecond, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
}

TEST_F(ImageTest, BlankSecondWebp) {
  const char kBlankSecond[] = "17xw,hencoded.url,_with,_various.stuff";
  GoogleString origin_url;
  ImageDim dim;
  EXPECT_FALSE(DecodeUrlAndDimensions(kWebp, kBlankSecond, &dim, &origin_url));
  EXPECT_FALSE(ImageUrlEncoder::HasValidDimensions(dim));
}

// Test OpenCV bug where width * height of image could be allocated on the
// stack. kLarge is a 10000x10000 image, so it will try to allocate > 100MB
// on the stack, which should overflow the stack and SEGV.
TEST_F(ImageTest, OpencvStackOverflow) {
  GoogleString buf;
  ImagePtr image(ReadImageFromFile(Image::IMAGE_JPEG, kLarge, &buf));

  ImageDim new_dim;
  new_dim.set_width(1);
  new_dim.set_height(1);
  image->ResizeTo(new_dim);
}

}  // namespace net_instaweb
