// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This permits the use of any UrlPollableAsyncFetcher as a synchronous fetcher

#ifndef NET_INSTAWEB_UTIL_PUBLIC_SYNC_FETCHER_ADAPTER_H_
#define NET_INSTAWEB_UTIL_PUBLIC_SYNC_FETCHER_ADAPTER_H_

#include <string>
#include "base/basictypes.h"
#include "net/instaweb/util/public/url_fetcher.h"
#include "net/instaweb/util/public/url_pollable_async_fetcher.h"

namespace net_instaweb {

class Timer;

class SyncFetcherAdapter : public UrlFetcher {
 public:
  // Note: the passed in async fetcher should use a timeout similar to
  // fetcher_timeout_ms (or none at all)
  SyncFetcherAdapter(Timer* timer,
                     int64 fetcher_timeout_ms,
                     UrlPollableAsyncFetcher* async_fetcher);
  virtual ~SyncFetcherAdapter();
  virtual bool StreamingFetchUrl(const std::string& url,
                                 const RequestHeaders& request_headers,
                                 ResponseHeaders* response_headers,
                                 Writer* fetched_content_writer,
                                 MessageHandler* message_handler);

 private:
  Timer* timer_;
  int64 fetcher_timeout_ms_;
  UrlPollableAsyncFetcher* async_fetcher_;

  DISALLOW_COPY_AND_ASSIGN(SyncFetcherAdapter);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_SYNC_FETCHER_ADAPTER_H_

