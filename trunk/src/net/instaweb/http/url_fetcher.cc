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

// Author: sligocki@google.com (Shawn Ligocki)

#include "net/instaweb/http/public/url_fetcher.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/util/public/string_writer.h"

namespace net_instaweb {

UrlFetcher::~UrlFetcher() {
}

bool UrlFetcher::FetchUrl(const GoogleString& url, GoogleString* content,
                          MessageHandler* message_handler) {
  StringWriter writer(content);
  RequestHeaders request_headers;
  ResponseHeaders response_headers;
  return StreamingFetchUrl(url, request_headers, &response_headers, &writer,
                           message_handler);
}

}  // namespace net_instaweb
