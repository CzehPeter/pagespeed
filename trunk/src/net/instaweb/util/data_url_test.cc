// Copyright 2010 Google Inc. All Rights Reserved.
// Author: jmaessen@google.com (Jan Maessen)

#include "net/instaweb/util/public/data_url.h"

#include "net/instaweb/util/public/content_type.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/gtest.h"

namespace net_instaweb {

const std::string kAsciiData =
    "A_Rather=Long,But-conventional?looking_string#with;some:odd,characters.";
const std::string kAsciiDataBase64 =
    "QV9SYXRoZXI9TG9uZyxCdXQtY29udmVudGlvbmFsP2xvb2tpbmdfc3RyaW5nI3dpdGg7c29"
    "tZTpvZGQsY2hhcmFjdGVycy4=";

// A string with embedded NULs; we must construct it carefully to avoid
// truncation.
const char kMixedDataChars[] =
    "This string\ncontains\0lots of\tunusual\xe3~characters\xd7\xa5";
const std::string kMixedData(kMixedDataChars, sizeof(kMixedDataChars)-1);
const std::string kMixedDataBase64 =
    "VGhpcyBzdHJpbmcKY29udGFpbnMAbG90cyBvZgl1bnVzdWFs435jaGFyYWN0ZXJz16U=";

const std::string kPlainPrefix = "data:text/plain,";
const std::string kBase64Prefix = "data:text/plain;base64,";

const std::string kGifPlainPrefix = "data:image/gif,";
const std::string kGifBase64Prefix = "data:image/gif;base64,";

class DataUrlTest : public testing::Test {
 protected:
  // Make a ContentType yield readable failure output.  Needed this to fix bugs
  // in the tests!  (No actual program bugs found here...)
  const char* Mime(const ContentType* type) {
    if (type == NULL || type->mime_type() == NULL) {
      return "NULL";
    } else {
      return type->mime_type();
    }
  }

  void TestDecoding(const bool can_parse,
                    const bool can_decode,
                    const std::string& prefix,
                    const std::string& encoded,
                    const ContentType* type,
                    Encoding encoding,
                    const std::string& decoded) {
    std::string url = prefix + encoded;
    const ContentType* parsed_type;
    Encoding parsed_encoding = UNKNOWN;
    StringPiece parsed_encoded;
    EXPECT_EQ(can_parse,
              ParseDataUrl(url, &parsed_type,
                           &parsed_encoding, &parsed_encoded));
    EXPECT_EQ(encoding, parsed_encoding);
    EXPECT_EQ(type, parsed_type) << "type '" << Mime(type) <<
        "' didn't match '" << Mime(parsed_type) << "'\n";
    EXPECT_EQ(encoded, parsed_encoded);
    std::string parsed_decoded;
    EXPECT_EQ(can_decode,
              DecodeDataUrlContent(encoding, parsed_encoded, &parsed_decoded));
    EXPECT_EQ(decoded, parsed_decoded);
  }
};

TEST_F(DataUrlTest, TestDataPlain) {
  std::string url;
  DataUrl(kContentTypeText, PLAIN, kAsciiData, &url);
  EXPECT_EQ(kPlainPrefix + kAsciiData, url);
}

TEST_F(DataUrlTest, TestDataBase64) {
  std::string url;
  DataUrl(kContentTypeText, BASE64, kAsciiData, &url);
  EXPECT_EQ(kBase64Prefix + kAsciiDataBase64, url);
}

TEST_F(DataUrlTest, TestData1Plain) {
  std::string url;
  DataUrl(kContentTypeGif, PLAIN, kMixedData, &url);
  EXPECT_EQ(kGifPlainPrefix + kMixedData, url);
}

TEST_F(DataUrlTest, TestData1Base64) {
  std::string url;
  DataUrl(kContentTypeGif, BASE64, kMixedData, &url);
  EXPECT_EQ(kGifBase64Prefix + kMixedDataBase64, url);
}

TEST_F(DataUrlTest, ParseDataPlain) {
  TestDecoding(true, true, kPlainPrefix, kAsciiData,
               &kContentTypeText, PLAIN, kAsciiData);
}

TEST_F(DataUrlTest, ParseDataBase64) {
  TestDecoding(true, true, kBase64Prefix, kAsciiDataBase64,
               &kContentTypeText, BASE64, kAsciiData);
}

TEST_F(DataUrlTest, ParseData1Plain) {
  TestDecoding(true, true, kPlainPrefix, kMixedData,
               &kContentTypeText, PLAIN, kMixedData);
}

TEST_F(DataUrlTest, ParseData1Base64) {
  TestDecoding(true, true, kBase64Prefix, kMixedDataBase64,
               &kContentTypeText, BASE64, kMixedData);
}

TEST_F(DataUrlTest, ParseBadProtocol) {
  TestDecoding(false, false, "http://www.google.com/", "",
               NULL, UNKNOWN, "");
}

TEST_F(DataUrlTest, ParseNoComma) {
  TestDecoding(false, false, "data:text/plain;base64;"+kMixedDataBase64, "",
               NULL, UNKNOWN, "");
}

TEST_F(DataUrlTest, ParseNoMime) {
  TestDecoding(true, true, "data:;base64,",kMixedDataBase64,
               NULL, BASE64, kMixedData);
}

TEST_F(DataUrlTest, ParseCorruptMime) {
  TestDecoding(true, true, "data:#$!;base64,", kMixedDataBase64,
               NULL, BASE64, kMixedData);
}

TEST_F(DataUrlTest, ParseBadEncodingIsPlain) {
  TestDecoding(true, true, "data:text/plain;mumbledypeg,", kMixedData,
               &kContentTypeText, PLAIN, kMixedData);
}

TEST_F(DataUrlTest, ParseBadBase64) {
  TestDecoding(true, false, kBase64Prefix, "@%#$%@#$%^@%%^%*%^&*",
               &kContentTypeText, BASE64, "");
}

}  // namespace net_instaweb
