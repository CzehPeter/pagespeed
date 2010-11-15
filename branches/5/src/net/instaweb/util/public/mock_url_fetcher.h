/**
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

// Author: sligocki@google.com (Shawn Ligocki)

#ifndef NET_INSTAWEB_UTIL_PUBLIC_MOCK_URL_FETCHER_H_
#define NET_INSTAWEB_UTIL_PUBLIC_MOCK_URL_FETCHER_H_

#include <map>
#include "net/instaweb/util/public/simple_meta_data.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/url_fetcher.h"

namespace net_instaweb {

// Simple UrlFetcher meant for tests, you can set responses for individual URLs.
class MockUrlFetcher : public UrlFetcher {
 public:
  MockUrlFetcher() : enabled_(true), fail_on_unexpected_(true) {}
  virtual ~MockUrlFetcher();

  void SetResponse(const StringPiece& url, const MetaData& response_header,
                   const StringPiece& response_body);

  // Fetching unset URLs will cause EXPECT failures as well as return false.
  virtual bool StreamingFetchUrl(const std::string& url,
                                 const MetaData& request_headers,
                                 MetaData* response_headers,
                                 Writer* response_writer,
                                 MessageHandler* message_handler);

  // When disabled, fetcher will fail (but not crash) for all requests.
  // Use to simulate temporarily not having access to resources, for example.
  void Disable() { enabled_ = false; }
  void Enable() { enabled_ = true; }

  // Set to false if you don't want the fetcher to EXPECT fail on unfound URL.
  // Useful in MockUrlFetcher unittest :)
  void set_fail_on_unexpected(bool x) { fail_on_unexpected_ = x; }

 private:
  class HttpResponse {
   public:
    HttpResponse(const MetaData& in_header, const StringPiece& in_body)
        : body_(in_body.data(), in_body.size()) {
      header_.CopyFrom(in_header);
    }

    const SimpleMetaData& header() const { return header_; }
    const std::string& body() const { return body_; }

   private:
    SimpleMetaData header_;
    std::string body_;

    DISALLOW_COPY_AND_ASSIGN(HttpResponse);
  };
  typedef std::map<const std::string, const HttpResponse*> ResponseMap;

  ResponseMap response_map_;
  bool enabled_;
  bool fail_on_unexpected_;  // Should we EXPECT if unexpected url called?

  DISALLOW_COPY_AND_ASSIGN(MockUrlFetcher);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_MOCK_URL_FETCHER_H_
