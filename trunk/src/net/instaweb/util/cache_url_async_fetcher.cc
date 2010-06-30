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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/util/public/cache_url_async_fetcher.h"
#include "net/instaweb/util/public/cache_url_fetcher.h"
#include "net/instaweb/util/public/http_cache.h"
#include "net/instaweb/util/public/simple_meta_data.h"
#include "net/instaweb/util/public/string_writer.h"

namespace net_instaweb {

namespace {

// This version of the caching async fetcher callback uses the
// caller-supplied response-header buffer, and also forwards the
// content to the client before caching it.
class ForwardingAsyncFetch : public CacheUrlFetcher::AsyncFetch {
 public:
  ForwardingAsyncFetch(const StringPiece& url, HTTPCache* cache,
                       MessageHandler* handler, Callback* callback,
                       Writer* writer, MetaData* response_headers,
                       bool force_caching)
      : CacheUrlFetcher::AsyncFetch(url, cache, handler, force_caching),
        callback_(callback),
        client_writer_(writer),
        response_headers_(response_headers) {
  }

  virtual void Done(bool success) {
    // Copy the data to the client even with a failure; there may be useful
    // error messages in the content.
    client_writer_->Write(content_, message_handler_);

    // Update the cache before calling the client Done callback, which might
    // delete the headers.
    if (success) {
      UpdateCache();
    }

    callback_->Done(success);
    delete this;
  }

  virtual MetaData* ResponseHeaders() { return response_headers_; }

 private:
  Callback* callback_;
  Writer* client_writer_;
  MetaData* response_headers_;
};

}  // namespace

CacheUrlAsyncFetcher::~CacheUrlAsyncFetcher() {
}

void CacheUrlAsyncFetcher::StreamingFetch(
    const std::string& url, const MetaData& request_headers,
    MetaData* response_headers, Writer* writer, MessageHandler* handler,
    Callback* callback) {
  if (http_cache_->Get(url.c_str(), response_headers, writer, handler)) {
    callback->Done(true);
  } else {
    ForwardingAsyncFetch* fetch = new ForwardingAsyncFetch(
        url, http_cache_, handler, callback, writer, response_headers,
        force_caching_);
    fetch->Start(fetcher_, request_headers);
  }
}

}  // namespace net_instaweb
