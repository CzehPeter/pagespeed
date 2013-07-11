// Copyright 2010 and onwards Google Inc.
// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/http/public/http_response_parser.h"

#include "net/instaweb/http/public/fetcher_test.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/stdio_file_system.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_writer.h"
namespace net_instaweb {

class HttpResponseParserTest : public FetcherTest {
};

TEST_F(HttpResponseParserTest, TestFetcher) {
  GoogleString http_filename = TestFilename();
  GoogleString http, content;
  ResponseHeaders response_headers;
  MockTimer timer(0);
  StdioFileSystem file_system;
  ASSERT_TRUE(file_system.ReadFile(http_filename.c_str(), &http,
                                   &message_handler_));
  StringWriter writer(&content);
  HttpResponseParser fetch(&response_headers, &writer, &message_handler_);
  ASSERT_TRUE(fetch.ParseChunk(http));
  ValidateOutput(content, response_headers);
}

}  // namespace net_instaweb
