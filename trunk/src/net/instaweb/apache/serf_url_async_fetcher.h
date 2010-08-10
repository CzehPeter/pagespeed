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

#ifndef HTML_REWRITER_SERF_URL_ASYNC_FETCHER_H_
#define HTML_REWRITER_SERF_URL_ASYNC_FETCHER_H_

#include <set>
#include <string>
#include <vector>
#include "base/basictypes.h"
#include "net/instaweb/util/public/url_async_fetcher.h"
#include "net/instaweb/util/public/message_handler.h"

struct apr_pool_t;
struct serf_context_t;
struct apr_thread_mutex_t;

using net_instaweb::MessageHandler;
using net_instaweb::Writer;
using net_instaweb::MetaData;
using net_instaweb::UrlAsyncFetcher;

namespace html_rewriter {

class SerfFetch;
class AprMutex;

class SerfUrlAsyncFetcher : public UrlAsyncFetcher {
 public:
  explicit SerfUrlAsyncFetcher(const char* proxy, apr_pool_t* pool);
  virtual ~SerfUrlAsyncFetcher();
  virtual void StreamingFetch(const std::string& url,
                              const MetaData& request_headers,
                              MetaData* response_headers,
                              Writer* fetched_content_writer,
                              MessageHandler* message_handler,
                              UrlAsyncFetcher::Callback* callback);

  bool Poll(int microseconds, MessageHandler* message_handler);
  bool WaitForInProgressFetches(int64 max_milliseconds,
                                MessageHandler* message_handler);

  // Remove the completed fetch from the active fetch set, and put it into a
  // completed fetch list to be cleaned up.
  void FetchComplete(SerfFetch* fetch);
  apr_pool_t* pool() const { return pool_; }
  serf_context_t* serf_context() const { return serf_context_; }

 private:
  bool SetupProxy(const char* proxy);

  apr_pool_t* pool_;
  AprMutex* mutex_;
  serf_context_t* serf_context_;
  std::set<SerfFetch*> active_fetches_;
  std::vector<SerfFetch*> completed_fetches_;
};

}  // namespace html_rewriter

#endif  // HTML_REWRITER_SERF_URL_ASYNC_FETCHER_H_
