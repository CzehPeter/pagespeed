/*
 * Copyright 2011 Google Inc.
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

// Author: morlovich@google.com (Maksim Orlovich)

// Unit-tests for ProxyInterface

#include "net/instaweb/automatic/public/proxy_interface.h"

#include <cstddef>

#include "net/instaweb/automatic/public/proxy_fetch.h"
#include "net/instaweb/htmlparse/public/empty_html_filter.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/mock_callback.h"
#include "net/instaweb/http/public/mock_url_fetcher.h"
#include "net/instaweb/http/public/reflecting_test_fetcher.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/semantic_type.h"
#include "net/instaweb/http/public/user_agent_matcher.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/public/critical_images_finder.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/furious_util.h"
#include "net/instaweb/rewriter/public/js_defer_disabled_filter.h"
#include "net/instaweb/rewriter/public/js_disable_filter.h"
#include "net/instaweb/rewriter/public/lazyload_images_filter.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/split_html_filter.h"
#include "net/instaweb/rewriter/public/static_javascript_manager.h"
#include "net/instaweb/rewriter/public/test_rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/test_url_namer.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/abstract_client_state.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/client_state.h"
#include "net/instaweb/util/public/delay_cache.h"
#include "net/instaweb/util/public/function.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/lru_cache.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/mock_scheduler.h"
#include "net/instaweb/util/public/mock_timer.h"
#include "net/instaweb/util/public/null_message_handler.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/queued_worker_pool.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_synchronizer.h"
#include "net/instaweb/util/public/thread_system.h"
#include "net/instaweb/util/public/time_util.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/worker_test_base.h"

namespace net_instaweb {

class HtmlFilter;
class MessageHandler;

namespace {

// This jpeg file lacks a .jpg or .jpeg extension.  So we initiate
// a property-cache read prior to getting the response-headers back,
// but will never go into the ProxyFetch flow that blocks waiting
// for the cache lookup to come back.
const char kImageFilenameLackingExt[] = "jpg_file_lacks_ext";
const char kPageUrl[] = "page.html";
const char kHttpsPageUrl[] = "https://www.test.com/page.html";
const char kHttpsCssUrl[] = "https://www.test.com/style.css";

const char kCssContent[] = "* { display: none; }";
const char kMinimizedCssContent[] = "*{display:none}";
const char kBackgroundFetchHeader[] = "X-Background-Fetch";
const char kFlushEarlyHtml[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"1.css\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"2.css\">"
    "<script src=\"1.js\"></script>"
    "<script src=\"2.js\"></script>"
    "<img src=\"1.jpg\"/>"
    "<script src=\"http://test.com/private.js\"></script>"
    "<script src=\"http://www.domain1.com/private.js\"></script>"
    "</head>"
    "<body>"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"3.css\">"
    "<script src=\"http://www.domain2.com/private.js\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>";
const char kFlushEarlyMoreResourcesInputHtml[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"1.css\">"
    "</head>"
    "<body>"
    "<script src=\"1.js\"></script>"
    "Hello, mod_pagespeed!"
    "</body>"
    "</html>";
const char kRewrittenHtml[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<img src=\"%s\"/>"
    "<script src=\"http://test.com/private.js\"></script>"
    "<script src=\"http://www.domain1.com/private.js\"></script>"
    "</head>"
    "<body>"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"http://www.domain2.com/private.js\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>";
const char kFlushEarlyRewrittenHtmlImageTag[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<script type=\"text/javascript\">(function(){"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";})()</script>"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 5</script>"
    "</head><head>%s"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<img src=\"%s\"/>"
    "<script src=\"http://test.com/private.js\"></script>"
    "<script src=\"http://www.domain1.com/private.js\"></script>"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"http://www.domain2.com/private.js\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>";
const char kFlushEarlyRewrittenHtmlImageTagInsertDnsPrefetch[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<script type=\"text/javascript\">(function(){"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";})()</script>"
    "<link rel=\"dns-prefetch\" href=\"//test.com\">"
    "<link rel=\"dns-prefetch\" href=\"//www.domain1.com\">"
    "<link rel=\"dns-prefetch\" href=\"//www.domain2.com\">"
    "<link rel=\"dns-prefetch\" href=\"//www.domain3.com\">"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 5</script>"
    "</head><head>%s"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<img src=\"%s\"/>"
    "<script src=\"http://test.com/private.js\"></script>"
    "<script src=\"http://www.domain1.com/private.js\"></script>"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"http://www.domain2.com/private.js\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>";
const char kFlushEarlyRewrittenHtmlLinkRelSubresource[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 5</script>"
    "</head><head>%s"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<img src=\"%s\"/>"
    "<script src=\"http://test.com/private.js\"></script>"
    "<script src=\"http://www.domain1.com/private.js\"></script>"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"http://www.domain2.com/private.js\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>";
const char kFlushEarlyRewrittenHtmlLinkScript[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<link rel=\"stylesheet\" href=\"%s\" media=\"print\" disabled=\"true\"/>\n"
    "<link rel=\"stylesheet\" href=\"%s\" media=\"print\" disabled=\"true\"/>\n"
    "<script type=\"psa_prefetch\" src=\"%s\"></script>\n"
    "<script type=\"psa_prefetch\" src=\"%s\"></script>\n"
    "<link rel=\"stylesheet\" href=\"%s\" media=\"print\" disabled=\"true\"/>\n"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 5</script>"
    "</head><head>%s"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<img src=\"%s\"/>"
    "<script src=\"http://test.com/private.js\"></script>"
    "<script src=\"http://www.domain1.com/private.js\"></script>"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"http://www.domain2.com/private.js\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>";
const char kRewrittenHtmlLazyloadDeferJsScriptFlushedEarly[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<link rel=\"stylesheet\" href=\"%s\" media=\"print\" disabled=\"true\"/>\n"
    "<link rel=\"stylesheet\" href=\"%s\" media=\"print\" disabled=\"true\"/>\n"
    "<link rel=\"stylesheet\" href=\"%s\" media=\"print\" disabled=\"true\"/>\n"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 3</script>"
    "<script type=\"text/javascript\">%s</script>"
    "%s"
    "%s"
    "</head><head>%s"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script pagespeed_orig_src=\"%s\" type=\"text/psajs\" orig_index=\"0\">"
    "</script>"
    "<script pagespeed_orig_src=\"%s\" type=\"text/psajs\" orig_index=\"1\">"
    "</script>"
    "<img pagespeed_lazy_src=\"%s\""
    " src=\"data:image/gif;"
    "base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==\""
    " onload=\"pagespeed.lazyLoadImages.loadIfVisible(this);\"/>"
    "<script type=\"text/javascript\" pagespeed_no_defer=\"\">"
    "pagespeed.lazyLoadImages.overrideAttributeFunctions();</script>"
    "<script pagespeed_orig_src=\"http://test.com/private.js\""
    " type=\"text/psajs\""
    " orig_index=\"2\"></script>"
    "<script pagespeed_orig_src=\"http://www.domain1.com/private.js\""
    " type=\"text/psajs\" orig_index=\"3\"></script>"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script pagespeed_orig_src=\"http://www.domain2.com/private.js\""
    " type=\"text/psajs\" orig_index=\"4\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>";
const char kRewrittenSplitHtmlWithLazyloadScriptFlushedEarly[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<link rel=\"stylesheet\" href=\"%s\" media=\"print\" disabled=\"true\"/>\n"
    "<link rel=\"stylesheet\" href=\"%s\" media=\"print\" disabled=\"true\"/>\n"
    "<link rel=\"stylesheet\" href=\"%s\" media=\"print\" disabled=\"true\"/>\n"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 3</script>"
    "%s"
    "</head><head>%s"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script pagespeed_orig_src=\"%s\" type=\"text/psajs\" orig_index=\"0\">"
    "</script>"
    "<script pagespeed_orig_src=\"%s\" type=\"text/psajs\" orig_index=\"1\">"
    "</script>"
    "%s"
    "<script pagespeed_orig_src=\"http://test.com/private.js\""
    " type=\"text/psajs\""
    " orig_index=\"2\"></script>"
    "<script pagespeed_orig_src=\"http://www.domain1.com/private.js\""
    " type=\"text/psajs\" orig_index=\"3\"></script>%s"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script pagespeed_orig_src=\"http://www.domain2.com/private.js\""
    " type=\"text/psajs\" orig_index=\"4\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>"
    "<script type=\"text/javascript\">pagespeed.num_low_res_images_inlined=0;"
    "</script><script type=\"text/javascript\" src=\"/psajs/blink.js\">"
    "</script>"
    "<script type=\"text/javascript\">"
      "pagespeed.panelLoaderInit();"
      "pagespeed.panelLoader.invokedFromSplit();"
      "pagespeed.panelLoader.loadCriticalData({});"
      "pagespeed.panelLoader.bufferNonCriticalData({});"
    "</script>\n</body></html>\n";
const char kRewrittenPageSpeedLazyImg[] = "<img pagespeed_lazy_src=\"%s\""
    " src=\"data:image/gif;"
    "base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==\""
    " onload=\"pagespeed.lazyLoadImages.loadIfVisible(this);\"/>"
    "<script type=\"text/javascript\" pagespeed_no_defer=\"\">"
    "pagespeed.lazyLoadImages.overrideAttributeFunctions();</script>";
const char kRewrittenPageSpeedImg[] = "<img src=\"%s\"/>";
const char kFlushEarlyRewrittenHtmlImageTagWithDeferJs[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<script type=\"text/javascript\">(function(){"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";"
    "new Image().src=\"%s\";})()</script>"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 3</script>"
    "</head><head>%s"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<img src=\"%s\"/>"
    "<script src=\"http://test.com/private.js\"></script>"
    "<script src=\"http://www.domain1.com/private.js\"></script>"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"http://www.domain2.com/private.js\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>";
const char kFlushEarlyRewrittenHtmlLinkRelSubresourceWithDeferJs[] =
    "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
    "<html>"
    "<head>"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<link rel=\"subresource\" href=\"%s\"/>\n"
    "<script type='text/javascript'>"
    "window.mod_pagespeed_prefetch_start = Number(new Date());"
    "window.mod_pagespeed_num_resources_prefetched = 3</script>"
    "</head><head>%s"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta charset=\"UTF-8\"/>"
    "<title>Flush Subresources Early example</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"%s\"></script>"
    "<script src=\"%s\"></script>"
    "<img src=\"%s\"/>"
    "<script src=\"http://test.com/private.js\"></script>"
    "<script src=\"http://www.domain1.com/private.js\"></script>"
    "</head>"
    "<body>%s"
    "Hello, mod_pagespeed!"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">"
    "<script src=\"http://www.domain2.com/private.js\"></script>"
    "<link rel=\"stylesheet\" type=\"text/css\""
    " href=\"http://www.domain3.com/3.css\">"
    "</body>"
    "</html>";

class MockPage : public PropertyPage {
 public:
  MockPage(AbstractMutex* mutex, const StringPiece& key)
      : PropertyPage(mutex, key) {}
  virtual ~MockPage() {}
  virtual void Done(bool valid) {}
 private:
  DISALLOW_COPY_AND_ASSIGN(MockPage);
};

// Like ExpectStringAsyncFetch but for asynchronous invocation -- it lets
// one specify a WorkerTestBase::SyncPoint to help block until completion.
class AsyncExpectStringAsyncFetch : public ExpectStringAsyncFetch {
 public:
  AsyncExpectStringAsyncFetch(bool expect_success,
                              bool log_flush,
                              GoogleString* buffer,
                              ResponseHeaders* response_headers,
                              LoggingInfo* logging_info,
                              bool* done_value,
                              WorkerTestBase::SyncPoint* notify,
                              ThreadSynchronizer* sync)
      : ExpectStringAsyncFetch(expect_success),
        buffer_(buffer),
        done_value_(done_value),
        logging_info_(logging_info),
        notify_(notify),
        sync_(sync),
        log_flush_(log_flush) {
    buffer->clear();
    response_headers->Clear();
    logging_info->Clear();
    *done_value = false;
    set_response_headers(response_headers);
  }

  virtual ~AsyncExpectStringAsyncFetch() {}

  virtual void HandleHeadersComplete() {
    sync_->Wait(ProxyFetch::kHeadersSetupRaceWait);
    response_headers()->Add("HeadersComplete", "1");  // Dirties caching info.
    sync_->Signal(ProxyFetch::kHeadersSetupRaceFlush);
  }

  virtual void HandleDone(bool success) {
    *buffer_ = buffer();
    *done_value_ = success;
    logging_info_->CopyFrom(*logging_info());
    ExpectStringAsyncFetch::HandleDone(success);
    WorkerTestBase::SyncPoint* notify = notify_;
    delete this;
    notify->Notify();
  }

  virtual bool HandleFlush(MessageHandler* handler) {
    if (log_flush_) {
      HandleWrite("|Flush|", handler);
    }
    return true;
  }

 private:
  GoogleString* buffer_;
  bool* done_value_;
  LoggingInfo* logging_info_;
  WorkerTestBase::SyncPoint* notify_;
  ThreadSynchronizer* sync_;
  bool log_flush_;

  DISALLOW_COPY_AND_ASSIGN(AsyncExpectStringAsyncFetch);
};

// This class creates a proxy URL naming rule that encodes an "owner" domain
// and an "origin" domain, all inside a fixed proxy-domain.
class ProxyUrlNamer : public UrlNamer {
 public:
  static const char kProxyHost[];

  ProxyUrlNamer() : authorized_(true), options_(NULL) {}

  // Given the request_url, generate the original url.
  virtual bool Decode(const GoogleUrl& gurl,
                      GoogleUrl* domain,
                      GoogleString* decoded) const {
    if (gurl.Host() != kProxyHost) {
      return false;
    }
    StringPieceVector path_vector;
    SplitStringPieceToVector(gurl.PathAndLeaf(), "/", &path_vector, false);
    if (path_vector.size() < 3) {
      return false;
    }
    if (domain != NULL) {
      domain->Reset(StrCat("http://", path_vector[1]));
    }

    // [0] is "" because PathAndLeaf returns a string with a leading slash
    *decoded = StrCat(gurl.Scheme(), ":/");
    for (size_t i = 2, n = path_vector.size(); i < n; ++i) {
      StrAppend(decoded, "/", path_vector[i]);
    }
    return true;
  }

  virtual bool IsAuthorized(const GoogleUrl& gurl,
                            const RewriteOptions& options) const {
    return authorized_;
  }

  // Given the request url and request headers, generate the rewrite options.
  virtual void DecodeOptions(const GoogleUrl& request_url,
                             const RequestHeaders& request_headers,
                             Callback* callback,
                             MessageHandler* handler) const {
    callback->Done((options_ == NULL) ? NULL : options_->Clone());
  }

  void set_authorized(bool authorized) { authorized_ = authorized; }
  void set_options(RewriteOptions* options) { options_ = options; }

 private:
  bool authorized_;
  RewriteOptions* options_;
  DISALLOW_COPY_AND_ASSIGN(ProxyUrlNamer);
};

const char ProxyUrlNamer::kProxyHost[] = "proxy_host.com";

// Mock filter which gets passed to the new rewrite driver created in
// proxy_fetch.
//
// This is used to check the flow for injecting data into filters via the
// ProxyInterface, including:
//     property_cache.
class MockFilter : public EmptyHtmlFilter {
 public:
  explicit MockFilter(RewriteDriver* driver)
      : driver_(driver),
        num_elements_(0),
        num_elements_property_(NULL) {
  }

  virtual void StartDocument() {
    num_elements_ = 0;
    PropertyCache* page_cache =
        driver_->server_context()->page_property_cache();
    const PropertyCache::Cohort* cohort =
        page_cache->GetCohort(RewriteDriver::kDomCohort);
    PropertyPage* page = driver_->property_page();
    if (page != NULL) {
      num_elements_property_ = page->GetProperty(cohort, "num_elements");
    } else {
      num_elements_property_ = NULL;
    }

    client_id_ = driver_->client_id();
    client_state_ = driver_->client_state();
    if (client_state_ != NULL) {
      // Set or clear the client state based on its current value, so we can
      // check whether it is being written back to the property cache correctly.
      if (!client_state_->InCache("http://www.fakeurl.com")) {
        client_state_->Set("http://www.fakeurl.com", 1000*1000);
      } else {
        client_state_->Clear();
      }
    }
  }

  virtual void StartElement(HtmlElement* element) {
    if (num_elements_ == 0) {
      // Before the start of the first element, print out the number
      // of elements that we expect based on the cache.
      GoogleString comment = " ";
      PropertyCache* page_cache =
          driver_->server_context()->page_property_cache();

      if (!client_id_.empty()) {
        StrAppend(&comment, "ClientID: ", client_id_, " ");
      }
      if (client_state_ != NULL) {
        StrAppend(&comment, "ClientStateID: ",
                  client_state_->ClientId(),
                  " InCache: ",
                  client_state_->InCache("http://www.fakeurl.com") ?
                  "true" : "false", " ");
      }
      if ((num_elements_property_ != NULL) &&
                 num_elements_property_->has_value()) {
        StrAppend(&comment, num_elements_property_->value(),
                  " elements ",
                  page_cache->IsStable(num_elements_property_)
                  ? "stable " : "unstable ");
      }
      HtmlNode* node = driver_->NewCommentNode(element->parent(), comment);
      driver_->InsertElementBeforeCurrent(node);
    }
    ++num_elements_;
  }

  virtual void EndDocument() {
    // We query IsCacheable for the HTML file only to ensure that
    // the test will crash if ComputeCaching() was never called.
    //
    // IsCacheable is true for HTML files because of kHtmlCacheTimeSec
    // above.
    EXPECT_TRUE(driver_->response_headers()->IsCacheable());

    if (num_elements_property_ != NULL) {
      PropertyCache* page_cache =
          driver_->server_context()->page_property_cache();
      page_cache->UpdateValue(IntegerToString(num_elements_),
                              num_elements_property_);
      num_elements_property_ = NULL;
    }
  }

  virtual const char* Name() const { return "MockFilter"; }

 private:
  RewriteDriver* driver_;
  int num_elements_;
  PropertyValue* num_elements_property_;
  GoogleString client_id_;
  AbstractClientState* client_state_;
};

// Hook provided to TestRewriteDriverFactory to add a new filter when
// a rewrite_driver is created.
class CreateFilterCallback
    : public TestRewriteDriverFactory::CreateFilterCallback {
 public:
  CreateFilterCallback() {}
  virtual ~CreateFilterCallback() {}

  virtual HtmlFilter* Done(RewriteDriver* driver) {
    return new MockFilter(driver);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(CreateFilterCallback);
};

// Subclass of AsyncFetch that adds a response header indicating whether the
// fetch is for a user-facing request, or a background rewrite.
class BackgroundFetchCheckingAsyncFetch : public SharedAsyncFetch {
 public:
  explicit BackgroundFetchCheckingAsyncFetch(AsyncFetch* base_fetch)
      : SharedAsyncFetch(base_fetch) {}
  virtual ~BackgroundFetchCheckingAsyncFetch() {}

  virtual void HandleHeadersComplete() {
    base_fetch()->HeadersComplete();
    response_headers()->Add(kBackgroundFetchHeader,
                            base_fetch()->IsBackgroundFetch() ? "1" : "0");
    // Call ComputeCaching again since Add sets cache_fields_dirty_ to true.
    response_headers()->ComputeCaching();
  }

  virtual void HandleDone(bool success) {
    base_fetch()->Done(success);
    delete this;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(BackgroundFetchCheckingAsyncFetch);
};

class FakeCriticalImagesFinder : public CriticalImagesFinder {
 public:
  explicit FakeCriticalImagesFinder(Statistics* stats)
      : CriticalImagesFinder(stats) {}
  ~FakeCriticalImagesFinder() {}

  virtual bool IsMeaningful() const { return true; }

  virtual void UpdateCriticalImagesSetInDriver(RewriteDriver* driver) {
    if (css_critical_images_ != NULL) {
      StringSet* css_critical_images = new StringSet;
      *css_critical_images = *css_critical_images_;
      driver->set_css_critical_images(css_critical_images);
    }
  }

  virtual void ComputeCriticalImages(StringPiece url,
                                     RewriteDriver* driver,
                                     bool must_compute) {
    // Do Nothing
  }

  virtual const char* GetCriticalImagesCohort() const {
    return "critical_images";
  }

  void set_css_critical_images(StringSet* css_critical_images) {
    css_critical_images_.reset(css_critical_images);
  }

 private:
  scoped_ptr<StringSet> css_critical_images_;
  DISALLOW_COPY_AND_ASSIGN(FakeCriticalImagesFinder);
};

class LatencyUrlAsyncFetcher : public UrlAsyncFetcher {
 public:
  explicit LatencyUrlAsyncFetcher(UrlAsyncFetcher* fetcher)
      : base_fetcher_(fetcher),
        latency_ms_(-1) {}
  virtual ~LatencyUrlAsyncFetcher() {}

  virtual void Fetch(const GoogleString& url,
                     MessageHandler* message_handler,
                     AsyncFetch* fetch) {
    if (latency_ms_ != -1) {
      fetch->log_record()->logging_info()->mutable_timing_info()->
          set_header_fetch_ms(latency_ms_);
    }
    base_fetcher_->Fetch(url, message_handler, fetch);
  }

  void set_latency(int64 latency) { latency_ms_ = latency; }

 private:
  UrlAsyncFetcher* base_fetcher_;
  int64 latency_ms_;
  DISALLOW_COPY_AND_ASSIGN(LatencyUrlAsyncFetcher);
};

// Subclass of UrlAsyncFetcher that wraps the AsyncFetch with a
// BackgroundFetchCheckingAsyncFetch.
class BackgroundFetchCheckingUrlAsyncFetcher : public UrlAsyncFetcher {
 public:
  explicit BackgroundFetchCheckingUrlAsyncFetcher(UrlAsyncFetcher* fetcher)
      : base_fetcher_(fetcher),
        num_background_fetches_(0) {}
  virtual ~BackgroundFetchCheckingUrlAsyncFetcher() {}

  virtual void Fetch(const GoogleString& url,
                     MessageHandler* message_handler,
                     AsyncFetch* fetch) {
    if (fetch->IsBackgroundFetch()) {
      num_background_fetches_++;
    }
    BackgroundFetchCheckingAsyncFetch* new_fetch =
        new BackgroundFetchCheckingAsyncFetch(fetch);
    base_fetcher_->Fetch(url, message_handler, new_fetch);
  }

  int num_background_fetches() { return num_background_fetches_; }
  void clear_num_background_fetches() { num_background_fetches_ = 0; }

 private:
  UrlAsyncFetcher* base_fetcher_;
  int num_background_fetches_;
  DISALLOW_COPY_AND_ASSIGN(BackgroundFetchCheckingUrlAsyncFetcher);
};

}  // namespace

// TODO(morlovich): This currently relies on ResourceManagerTestBase to help
// setup fetchers; and also indirectly to prevent any rewrites from timing out
// (as it runs the tests with real scheduler but mock timer). It would probably
// be better to port this away to use TestRewriteDriverFactory directly.
class ProxyInterfaceTest : public RewriteTestBase {
 public:
  // Helper function to run the fetch for ProxyInterfaceTest.HeadersSetupRace
  // in a thread so we can control it with signals using ThreadSynchronizer.
  //
  // It must be declared in the public section to be used in MakeFunction.
  void TestHeadersSetupRace() {
    mock_url_fetcher()->SetResponseFailure(AbsolutifyUrl(kPageUrl));
    TestPropertyCache(kPageUrl, true, true, false);
  }

 protected:
  static const int kHtmlCacheTimeSec = 5000;

  ProxyInterfaceTest()
      : fake_critical_images_finder_(
          new FakeCriticalImagesFinder(statistics())),
        max_age_300_("max-age=300"),
        request_start_time_ms_(-1),
        callback_done_value_(false) {
    ConvertTimeToString(MockTimer::kApr_5_2010_ms, &start_time_string_);
    ConvertTimeToString(MockTimer::kApr_5_2010_ms + 5 * Timer::kMinuteMs,
                        &start_time_plus_300s_string_);
    ConvertTimeToString(MockTimer::kApr_5_2010_ms - 2 * Timer::kDayMs,
                        &old_time_string_);
  }
  virtual ~ProxyInterfaceTest() {}

  virtual void SetUp() {
    RewriteOptions* options = server_context()->global_options();
    server_context_->set_enable_property_cache(true);
    page_property_cache()->AddCohort(RewriteDriver::kDomCohort);
    server_context_->client_property_cache()->AddCohort(
        ClientState::kClientStateCohort);
    options->ClearSignatureForTesting();
    options->EnableFilter(RewriteOptions::kRewriteCss);
    options->set_max_html_cache_time_ms(kHtmlCacheTimeSec * Timer::kSecondMs);
    options->set_ajax_rewriting_enabled(true);
    options->Disallow("*blacklist*");
    server_context()->ComputeSignature(options);
    RewriteTestBase::SetUp();
    ProxyInterface::InitStats(statistics());
    // The original url_async_fetcher() is still owned by RewriteDriverFactory.
    background_fetch_fetcher_.reset(new BackgroundFetchCheckingUrlAsyncFetcher(
        factory()->ComputeUrlAsyncFetcher()));
    latency_fetcher_.reset(
        new LatencyUrlAsyncFetcher(background_fetch_fetcher_.get()));
    server_context()->set_default_system_fetcher(latency_fetcher_.get());
    server_context()->set_critical_images_finder(fake_critical_images_finder_);

    proxy_interface_.reset(
        new ProxyInterface("localhost", 80, server_context(), statistics()));
    start_time_ms_ = timer()->NowMs();

    SetResponseWithDefaultHeaders(kImageFilenameLackingExt, kContentTypeJpeg,
                                  "image data", 300);
    SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                  "<div><p></p></div>", 0);
  }

  virtual void TearDown() {
    // Make sure all the jobs are over before we check for leaks ---
    // someone might still be trying to clean themselves up.
    mock_scheduler()->AwaitQuiescence();
    EXPECT_EQ(0, server_context()->num_active_rewrite_drivers());
    RewriteTestBase::TearDown();
  }

  // Initiates a fetch using the proxy interface, and waits for it to
  // complete.
  void FetchFromProxy(const StringPiece& url,
                      const RequestHeaders& request_headers,
                      bool expect_success,
                      GoogleString* string_out,
                      ResponseHeaders* headers_out) {
    FetchFromProxyNoWait(url, request_headers, expect_success,
                         false /* log_flush*/, headers_out);
    WaitForFetch();
    *string_out = callback_buffer_;
    logging_info_.CopyFrom(callback_logging_info_);
  }

  // TODO(jmarantz): eliminate this interface as it's annoying to have
  // the function overload just to save an empty RequestHeaders arg.
  void FetchFromProxy(const StringPiece& url,
                      bool expect_success,
                      GoogleString* string_out,
                      ResponseHeaders* headers_out) {
    RequestHeaders request_headers;
    FetchFromProxy(url, request_headers, expect_success, string_out,
                   headers_out);
  }

  void FetchFromProxyLoggingFlushes(const StringPiece& url,
                                    bool expect_success,
                                    GoogleString* string_out) {
    RequestHeaders request_headers;
    ResponseHeaders response_headers;
    FetchFromProxyNoWait(url, request_headers, expect_success,
                         true /* log_flush*/, &response_headers);
    WaitForFetch();
    *string_out = callback_buffer_;
    logging_info_.CopyFrom(callback_logging_info_);
  }

  // Initiates a fetch using the proxy interface, without waiting for it to
  // complete.  The usage model here is to delay callbacks and/or fetches
  // to control their order of delivery, then call WaitForFetch.
  void FetchFromProxyNoWait(const StringPiece& url,
                            const RequestHeaders& request_headers,
                            bool expect_success,
                            bool log_flush,
                            ResponseHeaders* headers_out) {
    sync_.reset(new WorkerTestBase::SyncPoint(
        server_context()->thread_system()));
    AsyncFetch* fetch = new AsyncExpectStringAsyncFetch(
        expect_success, log_flush, &callback_buffer_,
        &callback_response_headers_, &callback_logging_info_,
        &callback_done_value_, sync_.get(),
        server_context()->thread_synchronizer());
    fetch->set_response_headers(headers_out);
    fetch->request_headers()->CopyFrom(request_headers);
    proxy_interface_->Fetch(AbsolutifyUrl(url), message_handler(), fetch);
  }

  // This must be called after FetchFromProxyNoWait, once all of the required
  // resources (fetches, cache lookups) have been released.
  void WaitForFetch() {
    sync_->Wait();
    mock_scheduler()->AwaitQuiescence();
  }

  void CheckHeaders(const ResponseHeaders& headers,
                    const ContentType& expect_type) {
    ASSERT_TRUE(headers.has_status_code());
    EXPECT_EQ(HttpStatus::kOK, headers.status_code());
    EXPECT_STREQ(expect_type.mime_type(),
                 headers.Lookup1(HttpAttributes::kContentType));
  }

  void CheckBackgroundFetch(const ResponseHeaders& headers,
                            bool is_background_fetch) {
    EXPECT_STREQ(is_background_fetch ? "1" : "0",
              headers.Lookup1(kBackgroundFetchHeader));
  }

  void CheckNumBackgroundFetches(int num) {
    EXPECT_EQ(num, background_fetch_fetcher_->num_background_fetches());
  }

  virtual void ClearStats() {
    RewriteTestBase::ClearStats();
    background_fetch_fetcher_->clear_num_background_fetches();
  }

  // Serve a trivial HTML page with initial Cache-Control header set to
  // input_cache_control and return the Cache-Control header after running
  // through ProxyInterface.
  //
  // A unique id must be set to assure different websites are requested.
  // id is put in a URL, so it probably shouldn't have spaces and other
  // special chars.
  GoogleString RewriteHtmlCacheHeader(const StringPiece& id,
                                      const StringPiece& input_cache_control) {
    GoogleString url = StrCat("http://www.example.com/", id, ".html");
    ResponseHeaders input_headers;
    DefaultResponseHeaders(kContentTypeHtml, 100, &input_headers);
    input_headers.Replace(HttpAttributes::kCacheControl, input_cache_control);
    SetFetchResponse(url, input_headers, "<body>Foo</body>");

    GoogleString body;
    ResponseHeaders output_headers;
    FetchFromProxy(url, true, &body, &output_headers);
    ConstStringStarVector values;
    output_headers.Lookup(HttpAttributes::kCacheControl, &values);
    return JoinStringStar(values, ", ");
  }

  // Tests a single flow through the property-cache, optionally delaying or
  // threading property-cache lookups, and using the ThreadSynchronizer to
  // tease out race conditions.
  //
  // delay_pcache indicates that we will suspend the PropertyCache lookup
  // until after the FetchFromProxy call.  This is used in the
  // PropCacheNoWritesIfNonHtmlDelayedCache below, which tests the flow where
  // we have already detached the ProxyFetchPropertyCallbackCollector before
  // Done() is called.
  //
  // thread_pcache forces the property-cache to issue the lookup
  // callback in a different thread.  This lets us reproduce a
  // potential race conditions where a context switch in
  // ProxyFetchPropertyCallbackCollector::Done() would lead to a
  // double-deletion of the collector object.
  void TestPropertyCache(const StringPiece& url,
                         bool delay_pcache, bool thread_pcache,
                         bool expect_success) {
    RequestHeaders request_headers;
    ResponseHeaders response_headers;
    GoogleString output;
    TestPropertyCacheWithHeadersAndOutput(
        url, delay_pcache, thread_pcache, expect_success, true, true, false,
        request_headers, &response_headers, &output);
  }

  void TestPropertyCacheWithHeadersAndOutput(
      const StringPiece& url, bool delay_pcache, bool thread_pcache,
      bool expect_success, bool check_stats, bool add_create_filter_callback,
      bool expect_detach_before_pcache, const RequestHeaders& request_headers,
      ResponseHeaders* response_headers, GoogleString* output) {
    scoped_ptr<QueuedWorkerPool> pool;
    QueuedWorkerPool::Sequence* sequence = NULL;

    ThreadSynchronizer* sync = server_context()->thread_synchronizer();
    sync->EnableForPrefix(ProxyFetch::kCollectorDelete);
    GoogleString delay_pcache_key, delay_http_cache_key;
    if (delay_pcache || thread_pcache) {
      PropertyCache* pcache = page_property_cache();
      const PropertyCache::Cohort* cohort =
          pcache->GetCohort(RewriteDriver::kDomCohort);
      delay_http_cache_key = AbsolutifyUrl(url);
      delay_pcache_key = pcache->CacheKey(delay_http_cache_key, cohort);
      delay_cache()->DelayKey(delay_pcache_key);
      if (thread_pcache) {
        delay_cache()->DelayKey(delay_http_cache_key);
        pool.reset(new QueuedWorkerPool(
            1, server_context()->thread_system()));
        sequence = pool->NewSequence();
      }
    }

    CreateFilterCallback create_filter_callback;
    if (add_create_filter_callback) {
      factory()->AddCreateFilterCallback(&create_filter_callback);
    }

    if (thread_pcache) {
      FetchFromProxyNoWait(url, request_headers, expect_success,
                           false /* don't log flushes*/, response_headers);
      delay_cache()->ReleaseKeyInSequence(delay_pcache_key, sequence);

      // Wait until the property-cache-thread is in
      // ProxyFetchPropertyCallbackCollector::Done(), just after the
      // critical section when it will signal kCollectorReady, and
      // then block waiting for the test (in mainline) to signal
      // kCollectorDone.
      sync->Wait(ProxyFetch::kCollectorReady);

      // Now release the HTTPCache lookup, which allows the mock-fetch
      // to stream the bytes in the ProxyFetch and call HandleDone().
      // Note that we release this key in mainline, so that call
      // sequence happens directly from ReleaseKey.
      delay_cache()->ReleaseKey(delay_http_cache_key);

      // Now we can release the property-cache thread.
      sync->Signal(ProxyFetch::kCollectorDone);
      WaitForFetch();
      *output = callback_buffer_;
      sync->Wait(ProxyFetch::kCollectorDelete);
      pool->ShutDown();
    } else {
      FetchFromProxyNoWait(url, request_headers, expect_success, false,
                           response_headers);
      if (expect_detach_before_pcache) {
        WaitForFetch();
      }
      if (delay_pcache) {
        delay_cache()->ReleaseKey(delay_pcache_key);
      }
      if (!expect_detach_before_pcache) {
        WaitForFetch();
      }
      *output = callback_buffer_;
      sync->Wait(ProxyFetch::kCollectorDelete);
    }

    if (check_stats) {
      EXPECT_EQ(1, lru_cache()->num_inserts());  // http-cache
      EXPECT_EQ(2, lru_cache()->num_misses());   // http-cache & prop-cache
    }
  }

  int GetStatusCodeInPropertyCache(const GoogleString& url) {
    PropertyCache* pcache = page_property_cache();
    MockPage page(factory_->thread_system()->NewMutex(), url);
    const PropertyCache::Cohort* cohort = pcache->GetCohort(
        RewriteDriver::kDomCohort);
    PropertyValue* value;
    pcache->Read(&page);
    value = page.GetProperty(cohort, RewriteDriver::kStatusCodePropertyName);
    int status_code;
    EXPECT_TRUE(StringToInt(value->value().as_string(), &status_code));
    return status_code;
  }

  void TestOptionsUsedInCacheKey() {
    GoogleUrl gurl("http://www.test.com/");
    StringAsyncFetch callback;
    scoped_ptr<ProxyFetchPropertyCallbackCollector> callback_collector(
        proxy_interface_->InitiatePropertyCacheLookup(
        false, gurl, options(), &callback));
    EXPECT_NE(static_cast<ProxyFetchPropertyCallbackCollector*>(NULL),
              callback_collector.get());
    PropertyPage* page = callback_collector->GetPropertyPageWithoutOwnership(
        ProxyFetchPropertyCallback::kPagePropertyCache);
    EXPECT_NE(static_cast<PropertyPage*>(NULL), page);
    server_context()->ComputeSignature(options());
    GoogleString expected = StrCat(gurl.Spec(), "_", options()->signature());
    EXPECT_EQ(expected, page->key());
  }

  void DisableAjax() {
    RewriteOptions* options = server_context()->global_options();
    options->ClearSignatureForTesting();
    options->set_ajax_rewriting_enabled(false);
    server_context()->ComputeSignature(options);
  }

  void RejectBlacklisted() {
    RewriteOptions* options = server_context()->global_options();
    options->ClearSignatureForTesting();
    options->set_reject_blacklisted(true);
    options->set_reject_blacklisted_status_code(HttpStatus::kImATeapot);
    server_context()->ComputeSignature(options);
  }

  void SetupForFlushEarlyFlow(bool enable_experimental) {
    // Setup
    ResponseHeaders headers;
    headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
    headers.Add(HttpAttributes::kSetCookie, "CG=US:CA:Mountain+View");
    headers.Add(HttpAttributes::kSetCookie, "UA=chrome");
    headers.Add(HttpAttributes::kSetCookie, "path=/");

    headers.SetStatusAndReason(HttpStatus::kOK);
    mock_url_fetcher_.SetResponse(kTestDomain, headers, kFlushEarlyHtml);

    // Enable FlushSubresourcesFilter filter.
    RewriteOptions* rewrite_options = server_context()->global_options();
    rewrite_options->ClearSignatureForTesting();
    rewrite_options->EnableFilter(RewriteOptions::kFlushSubresources);
    rewrite_options->EnableFilter(RewriteOptions::kCombineCss);
    rewrite_options->EnableFilter(RewriteOptions::kCombineJavascript);
    rewrite_options->set_enable_flush_subresources_experimental(
        enable_experimental);
    rewrite_options->EnableExtendCacheFilters();
    // Disabling the inline filters so that the resources get flushed early
    // else our dummy resources are too small and always get inlined.
    rewrite_options->DisableFilter(RewriteOptions::kInlineCss);
    rewrite_options->DisableFilter(RewriteOptions::kInlineJavascript);
    rewrite_options->ComputeSignature(hasher());

    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.css"), kContentTypeCss,
                                  kCssContent, kHtmlCacheTimeSec * 2);
    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "2.css"), kContentTypeCss,
                                  kCssContent, kHtmlCacheTimeSec * 2);
    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "3.css"), kContentTypeCss,
                                  kCssContent, kHtmlCacheTimeSec * 2);
    const char kContent[] = "function f() {alert('foo');}";
    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.js"),
                                  kContentTypeJavascript, kContent,
                                  kHtmlCacheTimeSec * 2);
    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "2.js"),
                                  kContentTypeJavascript, kContent,
                                  kHtmlCacheTimeSec * 2);
    SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.jpg"),
                                  kContentTypeJpeg, "image",
                                  kHtmlCacheTimeSec * 2);
    ResponseHeaders private_headers;
    DefaultResponseHeaders(kContentTypeJavascript, kHtmlCacheTimeSec,
                           &private_headers);
    private_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                      300 * Timer::kSecondMs, ", private");
    private_headers.ComputeCaching();
    SetFetchResponse(AbsolutifyUrl("private.js"), private_headers, "a");
  }

  void VerifyCharset(ResponseHeaders* headers) {
    EXPECT_TRUE(StringCaseEqual(headers->Lookup1(HttpAttributes::kContentType),
                                "text/html; charset=utf-8"));
  }

  GoogleString FlushEarlyRewrittenHtml(
      UserAgentMatcher::PrefetchMechanism value,
      bool defer_js_enabled, bool insert_dns_prefetch) {
    return FlushEarlyRewrittenHtml(value, defer_js_enabled,
                                   insert_dns_prefetch, false, true);
  }

  GoogleString GetDeferJsCode() {
    return StrCat("<script src=\"",
                  server_context()->static_javascript_manager()->GetDeferJsUrl(
                      options_),
                  "\" type=\"text/javascript\"></script>");
  }

  GoogleString FlushEarlyRewrittenHtml(
      UserAgentMatcher::PrefetchMechanism value,
      bool defer_js_enabled, bool insert_dns_prefetch,
      bool split_html_enabled, bool lazyload_enabled) {
    GoogleString rewritten_css_url_1 = Encode(kTestDomain,
                                              "cf", "0", "1.css", "css");
    GoogleString rewritten_css_url_2 = Encode(kTestDomain,
                                              "cf", "0", "2.css", "css");
    GoogleString rewritten_css_url_3 = Encode(kTestDomain,
                                              "cf", "0", "3.css", "css");
    GoogleString rewritten_js_url_1 = Encode(kTestDomain,
                                             "jm", "0", "1.js", "js");
    GoogleString rewritten_js_url_2 = Encode(kTestDomain,
                                             "jm", "0", "2.js", "js");
    GoogleString combined_js_url = Encode(
        kTestDomain, "jc", "0",
        "1.js.pagespeed.jm.0.jsX2.js.pagespeed.jm.0.js", "js");;
    GoogleString rewritten_img_url_1 = Encode(kTestDomain,
                                              "ce", "0", "1.jpg", "jpg");
    GoogleString redirect_url = StrCat(kTestDomain, "?ModPagespeed=noscript");
    GoogleString cookie_script =
        "<script type=\"text/javascript\" pagespeed_no_defer=\"\">"
        "(function(){"
          "var data = [\"CG=US:CA:Mountain+View\",\"UA=chrome\",\"path=/\"];"
          "for (var i = 0; i < data.length; i++) {"
          "document.cookie = data[i];"
         "}})()"
        "</script>";
    combined_js_url[combined_js_url.find('X')] = '+';
    if (value == UserAgentMatcher::kPrefetchLinkScriptTag && defer_js_enabled) {
      return StringPrintf(
          kRewrittenHtmlLazyloadDeferJsScriptFlushedEarly,
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          rewritten_css_url_3.data(),
          LazyloadImagesFilter::GetLazyloadJsSnippet(
              options_,
              server_context()->static_javascript_manager()).c_str(),
          StrCat("<script type=\"text/javascript\">",
                 JsDisableFilter::GetJsDisableScriptSnippet(options_),
                 "</script>").c_str(),
          GetDeferJsCode().c_str(),
          cookie_script.data(),
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          rewritten_js_url_1.data(), rewritten_js_url_2.data(),
          rewritten_img_url_1.data(),
          StringPrintf(kNoScriptRedirectFormatter, redirect_url.c_str(),
                                 redirect_url.c_str()).c_str(),
          rewritten_css_url_3.data());
    } else if (value == UserAgentMatcher::kPrefetchLinkScriptTag &&
        split_html_enabled) {
      return StringPrintf(
          kRewrittenSplitHtmlWithLazyloadScriptFlushedEarly,
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          rewritten_css_url_3.data(),
          (lazyload_enabled ?
              StrCat("<script type=\"text/javascript\">",
                     LazyloadImagesFilter::GetLazyloadJsSnippet(
                         options_,
                         server_context()->static_javascript_manager()),
                     "</script>").c_str() : ""),
          cookie_script.data(),
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          rewritten_js_url_1.data(), rewritten_js_url_2.data(),
          lazyload_enabled ?
              StringPrintf(
                  kRewrittenPageSpeedLazyImg,
                  rewritten_img_url_1.data()).c_str() :
              StringPrintf(kRewrittenPageSpeedImg,
                           rewritten_img_url_1.data()).c_str(),
          StrCat("<script type=\"text/javascript\" pagespeed_no_defer=\"\">",
                 JsDisableFilter::GetJsDisableScriptSnippet(options_),
                 "</script>",
                 lazyload_enabled ? "" : SplitHtmlFilter::kPagespeedFunc,
                 SplitHtmlFilter::kSplitInit).c_str(),
          StringPrintf(kNoScriptRedirectFormatter, redirect_url.c_str(),
                                 redirect_url.c_str()).c_str(),
          rewritten_css_url_3.data());
    } else if (value == UserAgentMatcher::kPrefetchNotSupported) {
      return StringPrintf(kRewrittenHtml, rewritten_css_url_1.data(),
          rewritten_css_url_2.data(), rewritten_js_url_1.data(),
          rewritten_js_url_2.data(), rewritten_img_url_1.data(),
          rewritten_css_url_3.data());
    } else if (defer_js_enabled) {
      return StringPrintf(
          value == UserAgentMatcher::kPrefetchLinkRelSubresource ?
          kFlushEarlyRewrittenHtmlLinkRelSubresourceWithDeferJs :
          kFlushEarlyRewrittenHtmlImageTagWithDeferJs,
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          rewritten_css_url_3.data(), cookie_script.data(),
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          rewritten_js_url_1.data(), rewritten_js_url_2.data(),
          rewritten_img_url_1.data(),
          StringPrintf(kNoScriptRedirectFormatter, redirect_url.c_str(),
                       redirect_url.c_str()).c_str(),
          rewritten_css_url_3.data());
    } else {
      GoogleString output_format;
      if (insert_dns_prefetch) {
        output_format = kFlushEarlyRewrittenHtmlImageTagInsertDnsPrefetch;
      } else if (value == UserAgentMatcher::kPrefetchLinkRelSubresource) {
        output_format = kFlushEarlyRewrittenHtmlLinkRelSubresource;
      } else if (value == UserAgentMatcher::kPrefetchLinkScriptTag) {
        output_format = kFlushEarlyRewrittenHtmlLinkScript;
      } else {
        output_format = kFlushEarlyRewrittenHtmlImageTag;
      }
      return StringPrintf(
          output_format.c_str(),
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          rewritten_js_url_1.data(), rewritten_js_url_2.data(),
          rewritten_css_url_3.data(), cookie_script.data(),
          rewritten_css_url_1.data(), rewritten_css_url_2.data(),
          rewritten_js_url_1.data(), rewritten_js_url_2.data(),
          rewritten_img_url_1.data(),
          StringPrintf(kNoScriptRedirectFormatter, redirect_url.c_str(),
                                 redirect_url.c_str()).c_str(),
          rewritten_css_url_3.data());
    }
  }

  void ExperimentalFlushEarlyFlowTestHelper(
      const GoogleString& user_agent,
      UserAgentMatcher::PrefetchMechanism mechanism, bool inject_error) {
    ExperimentalFlushEarlyFlowTestHelperWithPropertyCache(
        user_agent, mechanism, false, false, inject_error);
    ExperimentalFlushEarlyFlowTestHelperWithPropertyCache(
        user_agent, mechanism, false, true, inject_error);
    ExperimentalFlushEarlyFlowTestHelperWithPropertyCache(
        user_agent, mechanism, true, true, inject_error);
    ExperimentalFlushEarlyFlowTestHelperWithPropertyCache(
        user_agent, mechanism, true, false, inject_error);
  }

  void ExperimentalFlushEarlyFlowTestHelperWithPropertyCache(
      const GoogleString& user_agent,
      UserAgentMatcher::PrefetchMechanism mechanism,
      bool delay_pcache, bool thread_pcache, bool inject_error) {
    lru_cache()->Clear();
    SetupForFlushEarlyFlow(true);
    GoogleString text;
    RequestHeaders request_headers;
    request_headers.Replace(HttpAttributes::kUserAgent, user_agent);
    ResponseHeaders headers;
    TestPropertyCacheWithHeadersAndOutput(
        kTestDomain, delay_pcache, thread_pcache, true, false, false,
        false, request_headers, &headers, &text);

    if (inject_error) {
      ResponseHeaders error_headers;
      error_headers.SetStatusAndReason(HttpStatus::kOK);
      mock_url_fetcher_.SetResponse(
          kTestDomain, error_headers, "");
    }

    // Fetch the url again. This time FlushEarlyFlow should not be triggered.
    // None
    TestPropertyCacheWithHeadersAndOutput(
        kTestDomain, delay_pcache, thread_pcache, true, false, false,
        inject_error, request_headers, &headers, &text);
    GoogleString expected_output = FlushEarlyRewrittenHtml(mechanism,
                                                           false, false);
    if (!inject_error) {
      EXPECT_EQ(expected_output, text);
      VerifyCharset(&headers);
    }
  }

  // This function is primarily meant to enable writes to the dom cohort of the
  // property cache. Writes to this cohort are predicated on a filter that uses
  // that cohort being enabled, which includes the insert dns prefetch filter.
  void EnableDomCohortWritesWithDnsPrefetch() {
    RewriteOptions* options = server_context()->global_options();
    options->ClearSignatureForTesting();
    options->EnableFilter(RewriteOptions::kInsertDnsPrefetch);
    server_context()->ComputeSignature(options);
  }

  scoped_ptr<ProxyInterface> proxy_interface_;
  scoped_ptr<BackgroundFetchCheckingUrlAsyncFetcher> background_fetch_fetcher_;
  scoped_ptr<LatencyUrlAsyncFetcher> latency_fetcher_;
  FakeCriticalImagesFinder* fake_critical_images_finder_;
  int64 start_time_ms_;
  GoogleString start_time_string_;
  GoogleString start_time_plus_300s_string_;
  GoogleString old_time_string_;
  LoggingInfo logging_info_;
  const GoogleString max_age_300_;
  int64 request_start_time_ms_;

  scoped_ptr<WorkerTestBase::SyncPoint> sync_;
  ResponseHeaders callback_response_headers_;
  GoogleString callback_buffer_;
  bool callback_done_value_;
  LoggingInfo callback_logging_info_;

 private:
  friend class FilterCallback;

  DISALLOW_COPY_AND_ASSIGN(ProxyInterfaceTest);
};

// TODO(mmohabey): Create separate test class for FlushEarlyFlow and move all
// related test from proxy_interface_test.cc to new test class.
TEST_F(ProxyInterfaceTest, FlushEarlyFlowTest) {
  SetupForFlushEarlyFlow(false);
  GoogleString text;
  RequestHeaders request_headers;
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  // Check total number of cache inserts.
  // 7 for 1.css, 2.css, 3.css, 1.js, 2.js, 1.jpg and private.js.
  // 6 for I.1.css.pagespeed.cf.0.css, I.2.css.pagespeed.cf.0.css,
  //       I.3.css.pagespeed.cf.0.css, 1.js.pagespeed.jm.0.js and
  //       2.js.pagespeed.jm.0.js and 1.jpg.pagespeed.ce.0.jpg.
  // 19 metadata cache enties - three for cf and jm, seven for ce and
  //       six for fs.
  // 1 for DomCohort write in property cache.
  EXPECT_EQ(33, lru_cache()->num_inserts());

  // Fetch the url again. This time FlushEarlyFlow should not be triggered.
  // None
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchNotSupported, false, false), text);
  VerifyCharset(&headers);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestPrefetch) {
  SetupForFlushEarlyFlow(false);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_rel_subresource");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  // Check total number of cache inserts.
  // 7 for 1.css, 2.css, 3.css, 1.js, 2.js, 1.jpg and private.js.
  // 6 for I.1.css.pagespeed.cf.0.css, I.2.css.pagespeed.cf.0.css,
  //       I.3.css.pagespeed.cf.0.css, 1.js.pagespeed.jm.0.js and
  //       2.js.pagespeed.jm.0.js and 1.jpg.pagespeed.ce.0.jpg.
  // 19 metadata cache enties - three for cf and jm, seven for ce and
  //       six for fs.
  // 1 for DomCohort write in property cache.
  EXPECT_EQ(33, lru_cache()->num_inserts());

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkRelSubresource, false, false),
      text);
  EXPECT_STREQ("cf,ei,jm", logging_info_.applied_rewriters());
  VerifyCharset(&headers);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowStatusCodeUnstable) {
  // Test that the flush early flow is not triggered when the status code is
  // unstable.
  SetupForFlushEarlyFlow(true);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_rel_subresource");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkRelSubresource, false, false),
      text);

  SetFetchResponse404(kTestDomain);
  // Fetch again so that 404 is populated in repsonse headers
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should not be triggered as
  // the status code is not stable.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(HttpStatus::kNotFound, headers.status_code());

  // Delete the 404 form cache and again set up for 200 response.
  lru_cache()->Delete(kTestDomain);
  SetupForFlushEarlyFlow(true);

  // Flush early flow is again not triggered as the status code is not
  // stable for property_cache_http_status_stability_threshold number of
  // requests.
  for (int i = 0, n = server_context()->global_options()->
       property_cache_http_status_stability_threshold(); i < n; ++i) {
    FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
    EXPECT_TRUE(text.find("link rel=\"subresource\"") == GoogleString::npos);
  }
  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkRelSubresource, false, false),
      text);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestImageTag) {
  SetupForFlushEarlyFlow(false);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  // Check total number of cache inserts.
  // 7 for 1.css, 2.css, 3.css, 1.js, 2.js, 1.jpg and private.js.
  // 6 for I.1.css.pagespeed.cf.0.css, I.2.css.pagespeed.cf.0.css,
  //       I.3.css.pagespeed.cf.0.css, 1.js.pagespeed.jm.0.js and
  //       2.js.pagespeed.jm.0.js and 1.jpg.pagespeed.ce.0.jpg.
  // 19 metadata cache enties - three for cf and jm, seven for ce and
  //       six for fs.
  // 1 for DomCohort write in property cache.
  EXPECT_EQ(33, lru_cache()->num_inserts());

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchImageTag, false, false), text);
  VerifyCharset(&headers);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestLinkScript) {
  SetupForFlushEarlyFlow(false);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_script_tag");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  // Check total number of cache inserts.
  // 7 for 1.css, 2.css, 3.css, 1.js, 2.js, 1.jpg and private.js.
  // 6 for I.1.css.pagespeed.cf.0.css, I.2.css.pagespeed.cf.0.css,
  //       I.3.css.pagespeed.cf.0.css, 1.js.pagespeed.jm.0.js and
  //       2.js.pagespeed.jm.0.js and 1.jpg.pagespeed.ce.0.jpg.
  // 19 metadata cache enties - three for cf and jm, seven for ce and
  //       six for fs.
  // 1 for DomCohort write in property cache.
  EXPECT_EQ(33, lru_cache()->num_inserts());

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkScriptTag, false, false), text);
  VerifyCharset(&headers);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestWithDeferJsImageTag) {
  SetupForFlushEarlyFlow(false);
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchImageTag, true, false), text);
  VerifyCharset(&headers);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestWithDeferJsPrefetch) {
  SetupForFlushEarlyFlow(false);
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_rel_subresource");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkRelSubresource, true, false), text);
  VerifyCharset(&headers);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTest) {
  ExperimentalFlushEarlyFlowTestHelper(
      "", UserAgentMatcher::kPrefetchNotSupported, false);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestError) {
  ExperimentalFlushEarlyFlowTestHelper(
      "", UserAgentMatcher::kPrefetchNotSupported, true);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestPrefetch) {
  ExperimentalFlushEarlyFlowTestHelper(
      "prefetch_link_rel_subresource",
      UserAgentMatcher::kPrefetchLinkRelSubresource, false);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestPrefetchError) {
  ExperimentalFlushEarlyFlowTestHelper(
       "prefetch_link_rel_subresource",
      UserAgentMatcher::kPrefetchLinkRelSubresource, true);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestImageTag) {
  ExperimentalFlushEarlyFlowTestHelper(
      "prefetch_image_tag", UserAgentMatcher::kPrefetchImageTag, false);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestImageTagError) {
  ExperimentalFlushEarlyFlowTestHelper(
      "prefetch_image_tag", UserAgentMatcher::kPrefetchImageTag, true);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestLinkScript) {
  ExperimentalFlushEarlyFlowTestHelper(
      "prefetch_link_script_tag", UserAgentMatcher::kPrefetchLinkScriptTag,
      false);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestLinkScriptError) {
  ExperimentalFlushEarlyFlowTestHelper(
      "prefetch_link_script_tag", UserAgentMatcher::kPrefetchLinkScriptTag,
      true);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestWithDeferJsImageTag) {
  SetupForFlushEarlyFlow(true);
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchImageTag, true, false), text);
  VerifyCharset(&headers);
}

TEST_F(ProxyInterfaceTest, ExperimentalFlushEarlyFlowTestWithDeferJsPrefetch) {
  SetupForFlushEarlyFlow(true);
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_rel_subresource");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkRelSubresource, true, false), text);
  VerifyCharset(&headers);
}

TEST_F(ProxyInterfaceTest,
       ExperimentalFlushEarlyFlowTestWithInsertDnsPrefetch) {
  SetupForFlushEarlyFlow(true);
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kInsertDnsPrefetch);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");
  ResponseHeaders headers;

  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered but not
  // insert dns prefetch filter as domains are not yet stable.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time InsertDnsPrefetch filter should applied.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchImageTag, false, true), text);
}

TEST_F(ProxyInterfaceTest, LazyloadAndDeferJsScriptFlushedEarly) {
  SetupForFlushEarlyFlow(true);
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  custom_options->EnableFilter(RewriteOptions::kLazyloadImages);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);
  GoogleString text;
  RequestHeaders request_headers;
  // Useragent is set to Firefox/ 9.0 because all flush early flow, defer
  // javascript and lazyload filter are enabled for this user agent.
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "Firefox/ 9.0");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkScriptTag, true, false), text);
}

TEST_F(ProxyInterfaceTest, SplitHtmlWithLazyloadScriptFlushedEarly) {
  SetupForFlushEarlyFlow(true);
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  custom_options->EnableFilter(RewriteOptions::kSplitHtml);
  custom_options->EnableFilter(RewriteOptions::kLazyloadImages);
  custom_options->set_critical_line_config(
       "div[@id = \"container\"]/div[4],"
       "img[3]:h1[@id = \"footer\"]");
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);
  GoogleString text;
  RequestHeaders request_headers;
  // Useragent is set to Firefox/ 9.0 because all flush early flow, defer
  // javascript and lazyload filter are enabled for this user agent.
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "Firefox/ 9.0");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkScriptTag, false, false, true, true),
      text);
}

TEST_F(ProxyInterfaceTest, SplitHtmlWithLazyloadScriptNotFlushedEarly) {
  SetupForFlushEarlyFlow(true);
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kDeferJavascript);
  custom_options->EnableFilter(RewriteOptions::kSplitHtml);
  custom_options->set_critical_line_config(
       "div[@id = \"container\"]/div[4],"
       "img[3]:h1[@id = \"footer\"]");
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);
  GoogleString text;
  RequestHeaders request_headers;
  // Useragent is set to Firefox/ 9.0 because all flush early flow, defer
  // javascript and lazyload filter are enabled for this user agent.
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "Firefox/ 9.0");
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(FlushEarlyRewrittenHtml(
      UserAgentMatcher::kPrefetchLinkScriptTag, false, false, true, false),
      text);
}

TEST_F(ProxyInterfaceTest, NoLazyloadScriptFlushedOutIfNoImagePresent) {
  const char kInputHtml[] =
      "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
      "<html>"
      "<head>"
      "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
      "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
      "<meta charset=\"UTF-8\"/>"
      "<title>Flush Subresources Early example</title>"
      "<link rel=\"stylesheet\" type=\"text/css\" href=\"1.css\">"
      "</head>"
      "<body>"
      "Hello, mod_pagespeed!"
      "</body>"
      "</html>";

  GoogleString redirect_url = StrCat(kTestDomain, "?ModPagespeed=noscript");
  GoogleString kOutputHtml = StrCat(
      "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
      "<html>"
      "<head>"
      "<link rel=\"stylesheet\""
      " href=\"http://test.com/I.1.css.pagespeed.cf.0.css\""
      " media=\"print\" disabled=\"true\"/>\n"
      "<script type='text/javascript'>"
      "window.mod_pagespeed_prefetch_start = Number(new Date());"
      "window.mod_pagespeed_num_resources_prefetched = 1"
      "</script>"
      "</head>"
      "<head>"
      "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
      "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
      "<meta charset=\"UTF-8\"/>"
      "<title>Flush Subresources Early example</title>"
      "<link rel=\"stylesheet\" type=\"text/css\""
      " href=\"http://test.com/I.1.css.pagespeed.cf.0.css\"></head>"
      "<body>",
      StringPrintf(kNoScriptRedirectFormatter, redirect_url.c_str(),
                   redirect_url.c_str()),
      "Hello, mod_pagespeed!</body></html>");

  ResponseHeaders headers;
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  mock_url_fetcher_.SetResponse(kTestDomain, headers, kInputHtml);

  // Enable FlushSubresourcesFilter filter.
  RewriteOptions* rewrite_options = server_context()->global_options();
  rewrite_options->ClearSignatureForTesting();
  rewrite_options->EnableFilter(RewriteOptions::kFlushSubresources);
  rewrite_options->set_enable_flush_subresources_experimental(true);
  rewrite_options->EnableExtendCacheFilters();
  // Disabling the inline filters so that the resources get flushed early
  // else our dummy resources are too small and always get inlined.
  rewrite_options->DisableFilter(RewriteOptions::kInlineCss);
  rewrite_options->DisableFilter(RewriteOptions::kInlineJavascript);
  rewrite_options->ComputeSignature(hasher());

  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.css"), kContentTypeCss,
                                kCssContent, kHtmlCacheTimeSec * 2);

  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kLazyloadImages);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  // Useragent is set to Firefox/ 9.0 because all flush early flow, defer
  // javascript and lazyload filter is enabled for this user agent.
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "Firefox/ 9.0");

  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(kOutputHtml, text);
}

TEST_F(ProxyInterfaceTest, FlushEarlyMoreResourcesIfTimePermits) {
  latency_fetcher_->set_latency(600);
  StringSet* css_critical_images = new StringSet;
  css_critical_images->insert(StrCat(kTestDomain, "1.jpg"));
  fake_critical_images_finder_->set_css_critical_images(css_critical_images);
  GoogleString redirect_url = StrCat(kTestDomain, "?ModPagespeed=noscript");

  GoogleString kOutputHtml = StrCat(
      "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
      "<html>"
      "<head>"
      "<script type=\"text/javascript\">(function(){"
      "new Image().src=\"http://test.com/I.1.css.pagespeed.cf.0.css\";"
      "new Image().src=\"http://test.com/1.jpg.pagespeed.ce.0.jpg\";"
      "new Image().src=\"http://test.com/1.js.pagespeed.ce.0.js\";})()</script>"
      "<script type='text/javascript'>"
      "window.mod_pagespeed_prefetch_start = Number(new Date());"
      "window.mod_pagespeed_num_resources_prefetched = 3"
      "</script>"
      "</head>"
      "<head>"
      "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
      "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
      "<meta charset=\"UTF-8\"/>"
      "<title>Flush Subresources Early example</title>"
      "<link rel=\"stylesheet\" type=\"text/css\""
      " href=\"http://test.com/I.1.css.pagespeed.cf.0.css\"></head>"
      "<body>",
      StringPrintf(kNoScriptRedirectFormatter, redirect_url.c_str(),
                   redirect_url.c_str()),
      "<script src=\"http://test.com/1.js.pagespeed.ce.0.js\"></script>"
      "Hello, mod_pagespeed!</body></html>");

  ResponseHeaders headers;
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  mock_url_fetcher_.SetResponse(kTestDomain, headers,
                                kFlushEarlyMoreResourcesInputHtml);

  // Enable FlushSubresourcesFilter filter.
  RewriteOptions* rewrite_options = server_context()->global_options();
  rewrite_options->ClearSignatureForTesting();
  rewrite_options->EnableFilter(RewriteOptions::kFlushSubresources);
  rewrite_options->set_enable_flush_subresources_experimental(true);

  rewrite_options->set_flush_more_resources_early_if_time_permits(true);
  rewrite_options->EnableExtendCacheFilters();
  // Disabling the inline filters so that the resources get flushed early
  // else our dummy resources are too small and always get inlined.
  rewrite_options->DisableFilter(RewriteOptions::kInlineCss);
  rewrite_options->DisableFilter(RewriteOptions::kInlineJavascript);
  rewrite_options->DisableFilter(RewriteOptions::kInlineImages);
  rewrite_options->ComputeSignature(hasher());

  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.jpg"), kContentTypeJpeg,
                                "image", kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.css"), kContentTypeCss,
                                kCssContent, kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.js"),
                                kContentTypeJavascript, "javascript",
                                kHtmlCacheTimeSec * 2);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");

  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered but
  // all resources may not be flushed.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time all resources based on time will be flushed.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  EXPECT_EQ(kOutputHtml, text);
  fake_critical_images_finder_->set_css_critical_images(NULL);
}

TEST_F(ProxyInterfaceTest, InsertLazyloadJsOnlyIfResourceHtmlNotEmpty) {
  const char kInputHtml[] =
      "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
      "<html>"
      "<head>"
      "<title>Flush Subresources Early example</title>"
      "</head>"
      "<body>"
      "<img src=1.jpg />"
      "Hello, mod_pagespeed!"
      "</body>"
      "</html>";

  GoogleString redirect_url = StrCat(kTestDomain, "?ModPagespeed=noscript");
  GoogleString kOutputHtml = StrCat(
      "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
      "<html>"
      "<head>"
      "<title>Flush Subresources Early example</title>"
      "</head>"
      "<body>",
      StringPrintf(kNoScriptRedirectFormatter, redirect_url.c_str(),
                   redirect_url.c_str()),
      "<script type=\"text/javascript\">",
      LazyloadImagesFilter::GetLazyloadJsSnippet(
          options_, server_context()->static_javascript_manager()),
      "</script>"
      "<img pagespeed_lazy_src=http://test.com/1.jpg.pagespeed.ce.0.jpg"
      " src=\"data:image/gif;"
      "base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==\""
      " onload=\"pagespeed.lazyLoadImages.loadIfVisible(this);\"/>"
      "Hello, mod_pagespeed!"
      "<script type=\"text/javascript\" pagespeed_no_defer=\"\">"
      "pagespeed.lazyLoadImages.overrideAttributeFunctions();</script>"
      "</body></html>");

  ResponseHeaders headers;
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  mock_url_fetcher_.SetResponse(kTestDomain, headers, kInputHtml);

  // Enable FlushSubresourcesFilter filter.
  RewriteOptions* rewrite_options = server_context()->global_options();
  rewrite_options->ClearSignatureForTesting();
  rewrite_options->EnableFilter(RewriteOptions::kFlushSubresources);
  rewrite_options->set_enable_flush_subresources_experimental(true);
  rewrite_options->EnableExtendCacheFilters();
  // Disabling the inline filters so that the resources get flushed early
  // else our dummy resources are too small and always get inlined.
  rewrite_options->DisableFilter(RewriteOptions::kInlineCss);
  rewrite_options->DisableFilter(RewriteOptions::kInlineJavascript);
  rewrite_options->ComputeSignature(hasher());

  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.jpg"), kContentTypeJpeg,
                                "image", kHtmlCacheTimeSec * 2);

  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->EnableFilter(RewriteOptions::kLazyloadImages);
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  GoogleString text;
  RequestHeaders request_headers;
  // Useragent is set to Firefox/ 9.0 because all flush early flow, defer
  // javascript and lazyload filter is enabled for this user agent.
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "Firefox/ 9.0");

  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow should be triggered but no
  // lazyload js will be flushed early as no resource is present in the html.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(kOutputHtml, text);
}

TEST_F(ProxyInterfaceTest, PreconnectTest) {
  latency_fetcher_->set_latency(200);
  const char kInputHtml[] =
      "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
      "<html>"
      "<head>"
      "<title>Flush Subresources Early example</title>"
      "<link rel=\"stylesheet\" type=\"text/css\" href=\"1.css\">"
      "</head>"
      "<body>"
      "<img src=1.jpg />"
      "<img src=2.jpg />"
      "<img src=3.jpg />"
      "Hello, mod_pagespeed!"
      "</body>"
      "</html>";

  GoogleString redirect_url = StrCat(kTestDomain, "?ModPagespeed=noscript");
  const char pre_connect_tag[] =
      "<link rel=\"stylesheet\" href=\"http://cdn.com/pre_connect?id=%s\"/>";
  const char image_tag[] =
      "<img src=http://cdn.com/http/test.com/http/test.com/%s />";

  GoogleString pre_connect_url = "http://cdn.com/pre_connect";
  GoogleString kOutputHtml = StrCat(
      "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
      "<html>"
      "<head>"
      "<script type=\"text/javascript\">"
      "(function(){new Image().src=\"http://cdn.com/http/test.com/http/"
      "test.com/I.1.css.pagespeed.cf.0.css\";})()</script>"
      "<script type='text/javascript'>"
      "window.mod_pagespeed_prefetch_start = Number("
      "new Date());window.mod_pagespeed_num_resources_prefetched = 1</script>",
      StringPrintf(pre_connect_tag, "0"),
      StringPrintf(pre_connect_tag, "1"),
      "</head><head><title>Flush Subresources Early example</title>"
      "<link rel=\"stylesheet\" type=\"text/css\" href=\"http://cdn.com/http/"
          "test.com/http/test.com/I.1.css.pagespeed.cf.0.css\">"
      "</head>"
      "<body>", StrCat(
      StringPrintf(kNoScriptRedirectFormatter, redirect_url.c_str(),
                   redirect_url.c_str()),
      StringPrintf(image_tag, "1.jpg.pagespeed.ce.0.jpg"),
      StringPrintf(image_tag, "2.jpg.pagespeed.ce.0.jpg"),
      StringPrintf(image_tag, "3.jpg.pagespeed.ce.0.jpg"),
      "Hello, mod_pagespeed!"
      "</body>"
      "</html>"));

  ResponseHeaders headers;
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  mock_url_fetcher_.SetResponse(kTestDomain, headers, kInputHtml);

  // Enable FlushSubresourcesFilter filter.
  RewriteOptions* rewrite_options = server_context()->global_options();
  rewrite_options->ClearSignatureForTesting();
  rewrite_options->EnableFilter(RewriteOptions::kFlushSubresources);
  rewrite_options->set_enable_flush_subresources_experimental(true);
  rewrite_options->EnableExtendCacheFilters();
  // Disabling the inline filters so that the resources get flushed early
  // else our dummy resources are too small and always get inlined.
  rewrite_options->DisableFilter(RewriteOptions::kInlineCss);
  rewrite_options->DisableFilter(RewriteOptions::kInlineJavascript);
  rewrite_options->set_pre_connect_url(pre_connect_url);
  rewrite_options->ComputeSignature(hasher());

  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.css"), kContentTypeCss,
                                kCssContent, kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.jpg"), kContentTypeJpeg,
                                "image", kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "2.jpg"), kContentTypeJpeg,
                                "image", kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "3.jpg"), kContentTypeJpeg,
                                "image", kHtmlCacheTimeSec * 2);
  TestUrlNamer url_namer;
  server_context()->set_url_namer(&url_namer);
  url_namer.SetProxyMode(true);

  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent, "prefetch_image_tag");

  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);

  // Fetch the url again. This time FlushEarlyFlow and pre connect should be
  // triggered.
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  EXPECT_EQ(kOutputHtml, text);
}

TEST_F(ProxyInterfaceTest, FlushEarlyFlowTestWithLocalStorageDoesNotCrash) {
  SetupForFlushEarlyFlow(true);
  GoogleString text;
  RequestHeaders request_headers;
  request_headers.Replace(HttpAttributes::kUserAgent,
                          "prefetch_link_rel_subresource");

  RewriteOptions* rewrite_options = server_context()->global_options();
  rewrite_options->ClearSignatureForTesting();
  rewrite_options->EnableFilter(RewriteOptions::kLocalStorageCache);
  rewrite_options->ForceEnableFilter(RewriteOptions::kInlineImages);
  rewrite_options->ForceEnableFilter(RewriteOptions::kInlineCss);
  rewrite_options->ComputeSignature(hasher());

  // This sequence of requests used to cause a crash earlier. Here, we just test
  // that this server doesn't crash and don't check the output.
  ResponseHeaders headers;
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
  FetchFromProxy(kTestDomain, request_headers, true, &text, &headers);
}

TEST_F(ProxyInterfaceTest, LoggingInfo) {
  GoogleString url = "http://www.example.com/";
  GoogleString text;
  RequestHeaders request_headers;
  ResponseHeaders headers;
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  mock_url_fetcher_.SetResponse("http://www.example.com/", headers,
                                "<html></html>");

  FetchFromProxy(url, request_headers, true, &text, &headers);
  CheckBackgroundFetch(headers, false);
  CheckNumBackgroundFetches(0);
  const TimingInfo timing_info_ = logging_info_.timing_info();
  ASSERT_TRUE(timing_info_.has_cache1_ms());
  EXPECT_EQ(timing_info_.cache1_ms(), 0);
  EXPECT_FALSE(timing_info_.has_cache2_ms());
  EXPECT_FALSE(timing_info_.has_header_fetch_ms());
  EXPECT_FALSE(timing_info_.has_fetch_ms());
}

TEST_F(ProxyInterfaceTest, HeadRequest) {
  // Test to check if we are handling Head requests correctly.
  GoogleString url = "http://www.example.com/";
  GoogleString set_text, get_text;
  RequestHeaders request_headers;
  ResponseHeaders set_headers, get_headers;

  set_headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  set_headers.SetStatusAndReason(HttpStatus::kOK);

  set_text = "<html></html>";

  mock_url_fetcher_.SetResponse(url, set_headers, set_text);
  FetchFromProxy(url, request_headers, true, &get_text, &get_headers);

  // Headers and body are correct for a Get request.
  EXPECT_EQ("HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "X-Background-Fetch: 0\r\n"
            "Date: Tue, 02 Feb 2010 18:51:26 GMT\r\n"
            "Expires: Tue, 02 Feb 2010 18:51:26 GMT\r\n"
            "Cache-Control: max-age=0, private\r\n"
            "X-Page-Speed: \r\n"
            "HeadersComplete: 1\r\n\r\n", get_headers.ToString());
  EXPECT_EQ(set_text, get_text);

  // Remove from the cache so we can actually test a HEAD fetch.
  http_cache()->Delete(url);

  ClearStats();

  // Headers and body are correct for a Head request.
  request_headers.set_method(RequestHeaders::kHead);
  FetchFromProxy(url, request_headers, true, &get_text, &get_headers);

  EXPECT_EQ(0, http_cache()->cache_hits()->Get());

  EXPECT_EQ("HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "X-Background-Fetch: 0\r\n"
            "X-Page-Speed: \r\n"
            "HeadersComplete: 1\r\n\r\n", get_headers.ToString());
  EXPECT_TRUE(get_text.empty());
}

TEST_F(ProxyInterfaceTest, HeadResourceRequest) {
  // Test to check if we are handling Head requests correctly in pagespeed
  // resource flow.
  const char kCssWithEmbeddedImage[] = "*{background-image:url(%s)}";
  const char kBackgroundImage[] = "1.png";

  GoogleString text;
  RequestHeaders request_headers;
  ResponseHeaders response_headers;
  GoogleString expected_response_headers_string = "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/css\r\n"
      "X-Background-Fetch: 0\r\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\r\n"
      "Expires: Tue, 02 Feb 2010 18:56:26 GMT\r\n"
      "Cache-Control: max-age=300,private\r\n"
      "X-Page-Speed: \r\n"
      "HeadersComplete: 1\r\n\r\n";

  // We're not going to image-compress so we don't need our mock image
  // to really be an image.
  SetResponseWithDefaultHeaders(kBackgroundImage, kContentTypePng, "image",
                                kHtmlCacheTimeSec * 2);
  GoogleString orig_css = StringPrintf(kCssWithEmbeddedImage, kBackgroundImage);
  SetResponseWithDefaultHeaders("embedded.css", kContentTypeCss,
                                orig_css, kHtmlCacheTimeSec * 2);

  // By default, cache extension is off in the default options.
  server_context()->global_options()->SetDefaultRewriteLevel(
      RewriteOptions::kPassThrough);

  // Because cache-extension was turned off, the image in the CSS file
  // will not be changed.
  FetchFromProxy("I.embedded.css.pagespeed.cf.0.css", request_headers,
                 true, &text, &response_headers);
  EXPECT_EQ(expected_response_headers_string, response_headers.ToString());
  EXPECT_EQ(orig_css, text);
  // Headers and body are correct for a Head request.
  request_headers.set_method(RequestHeaders::kHead);
  FetchFromProxy("I.embedded.css.pagespeed.cf.0.css", request_headers,
                 true, &text, &response_headers);

  // This leads to a conditional refresh of the original resource.
  expected_response_headers_string = "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/css\r\n"
      "X-Background-Fetch: 0\r\n"
      "Etag: W/\"PSA-0\"\r\n"
      "Date: Tue, 02 Feb 2010 18:51:26 GMT\r\n"
      "Expires: Tue, 02 Feb 2010 18:56:26 GMT\r\n"
      "Cache-Control: max-age=300,private\r\n"
      "X-Page-Speed: \r\n"
      "HeadersComplete: 1\r\n\r\n";

  EXPECT_EQ(expected_response_headers_string, response_headers.ToString());
  EXPECT_TRUE(text.empty());
}

TEST_F(ProxyInterfaceTest, FetchFailure) {
  GoogleString text;
  ResponseHeaders headers;

  // We don't want fetcher to fail the test, merely the fetch.
  SetFetchFailOnUnexpected(false);
  FetchFromProxy("invalid", false, &text, &headers);
  CheckBackgroundFetch(headers, false);
  CheckNumBackgroundFetches(0);
}

TEST_F(ProxyInterfaceTest, ReturnUnavailableForBlockedUrls) {
  GoogleString text;
  ResponseHeaders response_headers;
  response_headers.SetStatusAndReason(HttpStatus::kOK);
  mock_url_fetcher_.SetResponse(AbsolutifyUrl("blocked"), response_headers,
                                "<html></html>");
  FetchFromProxy("blocked", true,
                 &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());

  text.clear();
  response_headers.Clear();

  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());

  custom_options->AddRejectedUrlWildcard(AbsolutifyUrl("block*"));
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  FetchFromProxy("blocked", false, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kProxyDeclinedRequest, response_headers.status_code());
}

TEST_F(ProxyInterfaceTest, ReturnUnavailableForBlockedHeaders) {
  GoogleString text;
  RequestHeaders request_headers;
  ResponseHeaders response_headers;
  response_headers.SetStatusAndReason(HttpStatus::kOK);
  mock_url_fetcher_.SetResponse(kTestDomain, response_headers, "<html></html>");
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());

  custom_options->AddRejectedHeaderWildcard(HttpAttributes::kUserAgent,
                                            "*Chrome*");
  custom_options->AddRejectedHeaderWildcard(HttpAttributes::kXForwardedFor,
                                            "10.3.4.*");
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  request_headers.Add(HttpAttributes::kUserAgent, "Firefox");
  request_headers.Add(HttpAttributes::kXForwardedFor, "10.0.0.11");
  FetchFromProxy(kTestDomain, request_headers, true,
                 &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());

  request_headers.Clear();
  response_headers.Clear();

  request_headers.Add(HttpAttributes::kUserAgent, "abc");
  request_headers.Add(HttpAttributes::kUserAgent, "xyz Chrome abc");
  FetchFromProxy(kTestDomain, request_headers, false,
                 &text, &response_headers);
  EXPECT_EQ(HttpStatus::kProxyDeclinedRequest, response_headers.status_code());

  request_headers.Clear();
  response_headers.Clear();

  request_headers.Add(HttpAttributes::kXForwardedFor, "10.3.4.32");
  FetchFromProxy(kTestDomain, request_headers, false,
                 &text, &response_headers);
  EXPECT_EQ(HttpStatus::kProxyDeclinedRequest, response_headers.status_code());
}

TEST_F(ProxyInterfaceTest, PassThrough404) {
  GoogleString text;
  ResponseHeaders headers;
  SetFetchResponse404("404");
  FetchFromProxy("404", true, &text, &headers);
  ASSERT_TRUE(headers.has_status_code());
  EXPECT_EQ(HttpStatus::kNotFound, headers.status_code());
}

TEST_F(ProxyInterfaceTest, PassThroughResource) {
  GoogleString text;
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";

  SetResponseWithDefaultHeaders("text.txt", kContentTypeText, kContent,
                                kHtmlCacheTimeSec * 2);
  FetchFromProxy("text.txt", true, &text, &headers);
  CheckHeaders(headers, kContentTypeText);
  CheckBackgroundFetch(headers, false);
  CheckNumBackgroundFetches(0);
  EXPECT_EQ(kContent, text);
}

TEST_F(ProxyInterfaceTest, PassThroughEmptyResource) {
  ResponseHeaders headers;
  const char kContent[] = "";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(kContent, text2);
  // The HTTP response is found but the ajax metadata is not found.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
}

TEST_F(ProxyInterfaceTest, SetCookieNotCached) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.Add(HttpAttributes::kSetCookie, "cookie");
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has Set-Cookie headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_STREQ("cookie", response_headers.Lookup1(HttpAttributes::kSetCookie));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // The next response that is served from cache does not have any Set-Cookie
  // headers.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(NULL, response_headers2.Lookup1(HttpAttributes::kSetCookie));
  EXPECT_EQ(kContent, text2);
  // The HTTP response is found but the ajax metadata is not found.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
}

TEST_F(ProxyInterfaceTest, SetCookie2NotCached) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.Add(HttpAttributes::kSetCookie2, "cookie");
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has Set-Cookie headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_STREQ("cookie", response_headers.Lookup1(HttpAttributes::kSetCookie2));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // The next response that is served from cache does not have any Set-Cookie
  // headers.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(NULL, response_headers2.Lookup1(HttpAttributes::kSetCookie2));
  EXPECT_EQ(kContent, text2);
  // The HTTP response is found but the ajax metadata is not found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
}

TEST_F(ProxyInterfaceTest, NotCachedIfAuthorizedAndNotPublic) {
  // We should not cache things which are default cache-control if we
  // are sending Authorization:. See RFC 2616, 14.8.
  ReflectingTestFetcher reflect;
  server_context()->set_default_system_fetcher(&reflect);

  RequestHeaders request_headers;
  request_headers.Add("Was", "Here");
  request_headers.Add(HttpAttributes::kAuthorization, "Secret");
  // This will get reflected as well, and hence will determine whether
  // cacheable or not.
  request_headers.Replace(HttpAttributes::kCacheControl, "max-age=600000");

  ResponseHeaders out_headers;
  GoogleString out_text;
  // Using .txt here so we don't try any AJAX rewriting.
  FetchFromProxy("http://test.com/file.txt",
                 request_headers,  true, &out_text, &out_headers);
  // We should see the request headers we sent back as the response headers
  // as we're using a ReflectingTestFetcher.
  EXPECT_STREQ("Here", out_headers.Lookup1("Was"));

  // Not cross-domain, so should propagate out header.
  EXPECT_TRUE(out_headers.Has(HttpAttributes::kAuthorization));

  // Should not have written anything to cache, due to the authorization
  // header.
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());

  ClearStats();

  // Now try again. This time no authorization header, different 'Was'.
  request_headers.Replace("Was", "There");
  request_headers.RemoveAll(HttpAttributes::kAuthorization);

  FetchFromProxy("http://test.com/file.txt",
                 request_headers,  true, &out_text, &out_headers);
  // Should get different headers since we should not be cached.
  EXPECT_STREQ("There", out_headers.Lookup1("Was"));
  EXPECT_FALSE(out_headers.Has(HttpAttributes::kAuthorization));

  // And should be a miss per stats.
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());

  mock_scheduler()->AwaitQuiescence();
}

TEST_F(ProxyInterfaceTest, CachedIfAuthorizedAndPublic) {
  // This with Cache-Control: public should be cached even if
  // we are sending Authorization:. See RFC 2616.
  ReflectingTestFetcher reflect;
  server_context()->set_default_system_fetcher(&reflect);

  RequestHeaders request_headers;
  request_headers.Add("Was", "Here");
  request_headers.Add(HttpAttributes::kAuthorization, "Secret");
  // This will get reflected as well, and hence will determine whether
  // cacheable or not.
  request_headers.Replace(HttpAttributes::kCacheControl, "max-age=600000");
  request_headers.Add(HttpAttributes::kCacheControl, "public");  // unlike above

  ResponseHeaders out_headers;
  GoogleString out_text;
  // Using .txt here so we don't try any AJAX rewriting.
  FetchFromProxy("http://test.com/file.txt",
                 request_headers,  true, &out_text, &out_headers);
  EXPECT_STREQ("Here", out_headers.Lookup1("Was"));

  // Not cross-domain, so should propagate out header.
  EXPECT_TRUE(out_headers.Has(HttpAttributes::kAuthorization));

  // Should have written the result to the cache, despite the request having
  // Authorization: thanks to cache-control: public,
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());

  ClearStats();

  // Now try again. This time no authorization header, different 'Was'.
  request_headers.Replace("Was", "There");
  request_headers.RemoveAll(HttpAttributes::kAuthorization);

  FetchFromProxy("http://test.com/file.txt",
                 request_headers,  true, &out_text, &out_headers);
  // Should get old headers, since original was cacheable.
  EXPECT_STREQ("Here", out_headers.Lookup1("Was"));

  // ... of course hopefully a real server won't serve secrets on a
  // cache-control: public page.
  EXPECT_STREQ("Secret", out_headers.Lookup1(HttpAttributes::kAuthorization));

  // And should be a hit per stats.
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());

  mock_scheduler()->AwaitQuiescence();
}

TEST_F(ProxyInterfaceTest, ImplicitCachingHeadersForCss) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kContent);

  // The first response served by the fetcher has caching headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata, one for the HTTP response and one by the css
  // filter which looks up metadata while rewriting. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One hit for ajax metadata and one for the HTTP response.
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, CacheableSize) {
  // Test to check that we are not caching responses which have content length >
  // max_cacheable_response_content_length.
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.SetDateAndCaching(MockTimer::kApr_5_2010_ms, 300 * Timer::kSecondMs);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);

  // Set the set_max_cacheable_response_content_length to 10 bytes.
  http_cache()->set_max_cacheable_response_content_length(10);

  // Fetch once.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.html", true, &text, &response_headers);

  // One lookup for ajax metadata, one for the HTTP response and one for the
  // property cache entry. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_inserts());

  // Fetch again. It has the same caching headers.
  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  // None are found as the size is bigger than
  // max_cacheable_response_content_length.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  // Set the set_max_cacheable_response_content_length to 1024 bytes.
  http_cache()->set_max_cacheable_response_content_length(1024);
  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);
  // None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(1, lru_cache()->num_inserts());

  // Fetch again.
  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  // One hit for the HTTP response as content is smaller than
  // max_cacheable_response_content_length.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, CacheableSizeAjax) {
  // Test to check that we are not caching responses which have content length >
  // max_cacheable_response_content_length in Ajax flow.
  ResponseHeaders headers;
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kCssContent);

  http_cache()->set_max_cacheable_response_content_length(0);
  // The first response served by the fetcher and is not rewritten. An ajax
  // rewrite should not be triggered as the content length is greater than
  // max_cacheable_response_content_length.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_EQ(kCssContent, text);
  // One lookup for ajax metadata, one for the HTTP response. None are found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, lru_cache()->num_inserts());

  ClearStats();
  // Fetch again. Optimized version is not served.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_EQ(kCssContent, text);
  // None are found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
}

TEST_F(ProxyInterfaceTest, CacheableSizeResource) {
  // Test to check that we are not caching responses which have content length >
  // max_cacheable_response_content_length in resource flow.
  GoogleString text;
  ResponseHeaders headers;

  // Fetching of a rewritten resource we did not just create
  // after an HTML rewrite.
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);
  // Set the set_max_cacheable_response_content_length to 0 bytes.
  http_cache()->set_max_cacheable_response_content_length(0);
  // Fetch fails as original is not accessible.
  FetchFromProxy(Encode("", "cf", "0", "a.css", "css"), false, &text, &headers);
}

TEST_F(ProxyInterfaceTest, InvalidationForCacheableHtml) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.SetDateAndCaching(MockTimer::kApr_5_2010_ms, 300 * Timer::kSecondMs);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);

  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata, one for the HTTP response and one for the
  // property cache entry. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());

  // Change the response.
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, "new");

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We continue to serve the previous response since we've cached it.
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());

  // Invalidate the cache.
  scoped_ptr<RewriteOptions> custom_options(
      server_context()->global_options()->Clone());
  custom_options->set_cache_invalidation_timestamp(timer()->NowMs());
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We get the new response since we've invalidated the cache.
  EXPECT_EQ("new", text);
  // The HTTP response is found in the LRU cache but counts as a miss in the
  // HTTPCache since it has been invalidated. Also, cache misses for the ajax
  // metadata and property cache entry.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, UrlInvalidationForCacheableHtml) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.SetDateAndCaching(MockTimer::kApr_5_2010_ms, 300 * Timer::kSecondMs);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);

  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata, one for the HTTP response and one for the
  // property cache entry. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());

  // Change the response.
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, "new");

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We continue to serve the previous response since we've cached it.
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());


  // Invalidate the cache for some URL other than 'text.html'.
  scoped_ptr<RewriteOptions> custom_options_1(
      server_context()->global_options()->Clone());
  custom_options_1->AddUrlCacheInvalidationEntry(
      AbsolutifyUrl("foo.bar"), timer()->NowMs(), true);
  ProxyUrlNamer url_namer_1;
  url_namer_1.set_options(custom_options_1.get());
  server_context()->set_url_namer(&url_namer_1);

  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We continue to serve the previous response since we've cached it.
  EXPECT_EQ(kContent, text);
  // One hit for the HTTP response. Misses for the property cache entry and the
  // ajax metadata.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());


  // Invalidate the cache.
  scoped_ptr<RewriteOptions> custom_options_2(
      server_context()->global_options()->Clone());
  // Strictness of URL cache invalidation entry (last argument below) does not
  // matter in this test since there is nothing cached in metadata or property
  // caches.
  custom_options_2->AddUrlCacheInvalidationEntry(
      AbsolutifyUrl("text.html"), timer()->NowMs(), true);
  ProxyUrlNamer url_namer_2;
  url_namer_2.set_options(custom_options_2.get());
  server_context()->set_url_namer(&url_namer_2);

  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  // We get the new response since we've invalidated the cache.
  EXPECT_EQ("new", text);
  // The HTTP response is found in the LRU cache but counts as a miss in the
  // HTTPCache since it has been invalidated. Also, cache misses for the ajax
  // metadata and property cache entry.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, NoImplicitCachingHeadersForHtml) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);

  // The first response served by the fetcher does not have implicit caching
  // headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.html", true, &text, &response_headers);
  EXPECT_STREQ(NULL, response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // Lookups for: (1) ajax metadata (2) HTTP response (3) Property cache.
  // None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // Fetch again. Not found in cache.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.html", true, &text, &response_headers);
  EXPECT_EQ(NULL, response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // Lookups for: (1) ajax metadata (2) HTTP response (3) Property cache.
  // None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
}

TEST_F(ProxyInterfaceTest, ModifiedImplicitCachingHeadersForCss) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_implicit_cache_ttl_ms(500 * Timer::kSecondMs);
  server_context()->ComputeSignature(options);

  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  // Do not call ComputeCaching before calling SetFetchResponse because it will
  // add an explicit max-age=300 cache control header. We do not want that
  // header in this test.
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kContent);

  // The first response served by the fetcher has caching headers.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.css", true, &text, &response_headers);

  GoogleString max_age_500 = "max-age=500";
  GoogleString start_time_plus_500s_string;
  ConvertTimeToString(MockTimer::kApr_5_2010_ms + 500 * Timer::kSecondMs,
                      &start_time_plus_500s_string);

  EXPECT_STREQ(max_age_500,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_500s_string,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata, one for the HTTP response and one by the css
  // filter which looks up metadata while rewriting. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // Fetch again from cache. It has the same caching headers.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_500,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_500s_string,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kContent, text);
  // One hit for ajax metadata and one for the HTTP response.
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, EtagsAddedWhenAbsent) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.RemoveAll(HttpAttributes::kEtag);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has no Etag in the response.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
  EXPECT_EQ(NULL, response_headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  ClearStats();

  // An Etag is added before writing to cache. The next response is served from
  // cache and has an Etag.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(HttpStatus::kOK, response_headers2.status_code());
  EXPECT_STREQ("W/\"PSA-0\"", response_headers2.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text2);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  ClearStats();

  // The Etag matches and a 304 is served out.
  GoogleString text3;
  ResponseHeaders response_headers3;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kIfNoneMatch, "W/\"PSA-0\"");
  FetchFromProxy("text.txt", request_headers, true, &text3, &response_headers3);
  EXPECT_EQ(HttpStatus::kNotModified, response_headers3.status_code());
  EXPECT_STREQ(NULL, response_headers3.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ("", text3);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
}

TEST_F(ProxyInterfaceTest, EtagMatching) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.Replace(HttpAttributes::kEtag, "etag");
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has an Etag in the response.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
  EXPECT_STREQ("etag", response_headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());

  ClearStats();
  // The next response is served from cache.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(HttpStatus::kOK, response_headers2.status_code());
  EXPECT_STREQ("etag", response_headers2.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text2);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  ClearStats();

  // The Etag matches and a 304 is served out.
  GoogleString text3;
  ResponseHeaders response_headers3;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kIfNoneMatch, "etag");
  FetchFromProxy("text.txt", request_headers, true, &text3, &response_headers3);
  EXPECT_EQ(HttpStatus::kNotModified, response_headers3.status_code());
  EXPECT_STREQ(NULL, response_headers3.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ("", text3);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());

  ClearStats();
  // The Etag doesn't match and the full response is returned.
  GoogleString text4;
  ResponseHeaders response_headers4;
  request_headers.Replace(HttpAttributes::kIfNoneMatch, "mismatch");
  FetchFromProxy("text.txt", request_headers, true, &text4, &response_headers4);
  EXPECT_EQ(HttpStatus::kOK, response_headers4.status_code());
  EXPECT_STREQ("etag", response_headers4.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(kContent, text4);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
}

TEST_F(ProxyInterfaceTest, LastModifiedMatch) {
  ResponseHeaders headers;
  const char kContent[] = "A very compelling article";
  SetDefaultLongCacheHeaders(&kContentTypeText, &headers);
  headers.SetLastModified(MockTimer::kApr_5_2010_ms);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.txt"), headers, kContent);

  // The first response served by the fetcher has an Etag in the response.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.txt", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ(kContent, text);
  // One lookup for ajax metadata and one for the HTTP response. Neither are
  // found.
  EXPECT_EQ(2, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());

  ClearStats();
  // The next response is served from cache.
  GoogleString text2;
  ResponseHeaders response_headers2;
  FetchFromProxy("text.txt", true, &text2, &response_headers2);
  EXPECT_EQ(HttpStatus::kOK, response_headers2.status_code());
  EXPECT_STREQ(start_time_string_,
               response_headers2.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ(kContent, text2);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());

  ClearStats();
  // The last modified timestamp matches and a 304 is served out.
  GoogleString text3;
  ResponseHeaders response_headers3;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kIfModifiedSince, start_time_string_);
  FetchFromProxy("text.txt", request_headers, true, &text3, &response_headers3);
  EXPECT_EQ(HttpStatus::kNotModified, response_headers3.status_code());
  EXPECT_STREQ(NULL, response_headers3.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ("", text3);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());

  ClearStats();
  // The last modified timestamp doesn't match and the full response is
  // returned.
  GoogleString text4;
  ResponseHeaders response_headers4;
  request_headers.Replace(HttpAttributes::kIfModifiedSince,
                          "Fri, 02 Apr 2010 18:51:26 GMT");
  FetchFromProxy("text.txt", request_headers, true, &text4, &response_headers4);
  EXPECT_EQ(HttpStatus::kOK, response_headers4.status_code());
  EXPECT_STREQ(start_time_string_,
               response_headers4.Lookup1(HttpAttributes::kLastModified));
  EXPECT_EQ(kContent, text4);
  // One lookup for ajax metadata and one for the HTTP response. The metadata is
  // not found but the HTTP response is found.`
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
}

TEST_F(ProxyInterfaceTest, AjaxRewritingForCss) {
  ResponseHeaders headers;
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kCssContent);

  // The first response served by the fetcher and is not rewritten. An ajax
  // rewrite is triggered.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kCssContent, text);
  CheckBackgroundFetch(response_headers, false);
  CheckNumBackgroundFetches(0);
  // One lookup for ajax metadata, one for the HTTP response and one by the css
  // filter which looks up metadata while rewriting. None are found.
  EXPECT_EQ(3, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // The rewrite is complete and the optimized version is served.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kMinimizedCssContent, text);
  // One hit for ajax metadata and one for the rewritten HTTP response.
  EXPECT_EQ(2, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
  CheckNumBackgroundFetches(0);

  ClearStats();
  // Advance close to expiry.
  AdvanceTimeUs(270 * Timer::kSecondUs);
  // The rewrite is complete and the optimized version is served. A freshen is
  // triggered to refresh the original CSS file.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);

  EXPECT_STREQ("max-age=30",
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ("Mon, 05 Apr 2010 18:55:56 GMT",
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kMinimizedCssContent, text);
  // One hit for ajax metadata, one for the rewritten HTTP response and one for
  // the original HTTP response while freshening.
  EXPECT_EQ(3, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
  // One background fetch is triggered while freshening.
  CheckNumBackgroundFetches(1);

  // Disable ajax rewriting. We now received the response fetched while
  // freshening. This response has kBackgroundFetchHeader set to 1.
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_ajax_rewriting_enabled(false);
  server_context()->ComputeSignature(options);

  ClearStats();
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", true, &text, &response_headers);
  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ("Mon, 05 Apr 2010 19:00:56 GMT",
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ("Mon, 05 Apr 2010 18:55:56 GMT",
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kCssContent, text);
  CheckNumBackgroundFetches(0);
  CheckBackgroundFetch(response_headers, true);
  // Done HTTP cache hit for the original response.
  EXPECT_EQ(1, lru_cache()->num_hits());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
}

TEST_F(ProxyInterfaceTest, NoAjaxRewritingWhenAuthorizationSent) {
  // We should not do ajax rewriting when sending over an authorization
  // header if the original isn't cache-control: public.
  ResponseHeaders headers;
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kCssContent);

  // The first response served by the fetcher and is not rewritten. An ajax
  // rewrite is triggered.
  GoogleString text;
  ResponseHeaders response_headers;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kAuthorization, "Paperwork");
  FetchFromProxy("text.css", request_headers, true, &text, &response_headers);
  EXPECT_EQ(kCssContent, text);

  // The second version should still be unoptimized, since original wasn't
  // cacheable.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", request_headers, true, &text, &response_headers);
  EXPECT_EQ(kCssContent, text);
}

TEST_F(ProxyInterfaceTest, AjaxRewritingWhenAuthorizationButPublic) {
  // We should do ajax rewriting when sending over an authorization
  // header if the original is cache-control: public.
  ResponseHeaders headers;
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.Add(HttpAttributes::kCacheControl, "public, max-age=400");
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("text.css"), headers, kCssContent);

  // The first response served by the fetcher and is not rewritten. An ajax
  // rewrite is triggered.
  GoogleString text;
  ResponseHeaders response_headers;
  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kAuthorization, "Paperwork");
  FetchFromProxy("text.css", request_headers, true, &text, &response_headers);
  EXPECT_EQ(kCssContent, text);

  // The second version should be optimized in this case.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("text.css", request_headers, true, &text, &response_headers);
  EXPECT_EQ(kMinimizedCssContent, text);
}

TEST_F(ProxyInterfaceTest, AjaxRewritingDisabledByGlobalDisable) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_enabled(false);
  server_context()->ComputeSignature(options);

  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("a.css", true, &text, &response_headers);
  // First fetch will not get rewritten no matter what.
  EXPECT_STREQ(kCssContent, text);

  // Second fetch would get minified if ajax rewriting were on; but
  // it got disabled by the global toggle.
  text.clear();
  FetchFromProxy("a.css", true, &text, &response_headers);
  EXPECT_STREQ(kCssContent, text);
}

TEST_F(ProxyInterfaceTest, AjaxRewritingSkippedIfBlacklisted) {
  ResponseHeaders headers;
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("blacklist.css"), headers, kCssContent);

  // The first response is served by the fetcher. Since the url is blacklisted,
  // no ajax rewriting happens.
  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("blacklist.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kCssContent, text);
  // Since no ajax rewriting happens, there is only a single cache lookup for
  // the resource.
  EXPECT_EQ(1, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());
  EXPECT_EQ(0, lru_cache()->num_hits());

  ClearStats();
  // The same thing happens on the second request.
  text.clear();
  response_headers.Clear();
  FetchFromProxy("blacklist.css", true, &text, &response_headers);

  EXPECT_STREQ(max_age_300_,
               response_headers.Lookup1(HttpAttributes::kCacheControl));
  EXPECT_STREQ(start_time_plus_300s_string_,
               response_headers.Lookup1(HttpAttributes::kExpires));
  EXPECT_STREQ(start_time_string_,
               response_headers.Lookup1(HttpAttributes::kDate));
  EXPECT_EQ(kCssContent, text);
  // The resource is found in cache this time.
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(0, http_cache()->cache_misses()->Get());
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
  EXPECT_EQ(1, lru_cache()->num_hits());
}

TEST_F(ProxyInterfaceTest, AjaxRewritingBlacklistReject) {
  // Makes sure that we honor reject_blacklisted() when ajax rewriting may
  // have normally happened.
  RejectBlacklisted();

  ResponseHeaders headers;
  SetTimeMs(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeCss.mime_type());
  headers.SetDate(MockTimer::kApr_5_2010_ms);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("blacklistCoffee.css"), headers, kCssContent);
  SetFetchResponse(AbsolutifyUrl("tea.css"), headers, kCssContent);

  GoogleString text;
  ResponseHeaders response_headers;
  FetchFromProxy("blacklistCoffee.css", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kImATeapot, response_headers.status_code());
  EXPECT_TRUE(text.empty());

  // Non-blacklisted stuff works OK.
  FetchFromProxy("tea.css", true, &text, &response_headers);
  EXPECT_EQ(HttpStatus::kOK, response_headers.status_code());
  EXPECT_EQ(kCssContent, text);
}

TEST_F(ProxyInterfaceTest, EatCookiesOnReconstructFailure) {
  // Make sure we don't pass through a Set-Cookie[2] when reconstructing
  // a resource on demand fails.
  GoogleString abs_path = AbsolutifyUrl("a.css");
  ResponseHeaders response_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &response_headers);
  response_headers.Add(HttpAttributes::kSetCookie, "a cookie");
  response_headers.Add(HttpAttributes::kSetCookie2, "a weird old-time cookie");
  response_headers.ComputeCaching();
  SetFetchResponse(abs_path, response_headers, "broken_css{");

  ResponseHeaders out_response_headers;
  GoogleString text;
  FetchFromProxy(Encode(kTestDomain, "cf", "0", "a.css", "css"), true,
                 &text, &out_response_headers);
  EXPECT_EQ(NULL, out_response_headers.Lookup1(HttpAttributes::kSetCookie));
  EXPECT_EQ(NULL, out_response_headers.Lookup1(HttpAttributes::kSetCookie2));
}

TEST_F(ProxyInterfaceTest, RewriteHtml) {
  GoogleString text;
  ResponseHeaders headers;

  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  server_context()->ComputeSignature(options);

  headers.Add(HttpAttributes::kEtag, "something");
  headers.SetDateAndCaching(MockTimer::kApr_5_2010_ms,
                            kHtmlCacheTimeSec * 2 * Timer::kSecondMs);
  headers.SetLastModified(MockTimer::kApr_5_2010_ms);
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl(kPageUrl), headers, CssLinkHref("a.css"));

  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  text.clear();
  headers.Clear();
  FetchFromProxy(kPageUrl, true, &text, &headers);
  CheckBackgroundFetch(headers, false);
  CheckNumBackgroundFetches(1);
  CheckHeaders(headers, kContentTypeHtml);
  EXPECT_EQ(CssLinkHref(Encode(kTestDomain, "cf", "0", "a.css", "css")), text);
  headers.ComputeCaching();
  EXPECT_LE(start_time_ms_ + kHtmlCacheTimeSec * Timer::kSecondMs,
            headers.CacheExpirationTimeMs());
  EXPECT_EQ(NULL, headers.Lookup1(HttpAttributes::kEtag));
  EXPECT_EQ(NULL, headers.Lookup1(HttpAttributes::kLastModified));
  EXPECT_STREQ("cf", logging_info_.applied_rewriters());

  // Fetch the rewritten resource as well.
  text.clear();
  headers.Clear();
  ClearStats();
  FetchFromProxy(Encode(kTestDomain, "cf", "0", "a.css", "css"), true,
                 &text, &headers);
  CheckHeaders(headers, kContentTypeCss);
  // Note that the fetch for the original resource was triggered as a result of
  // the initial HTML request. Hence, its headers indicate that it is a
  // background request
  // This response has kBackgroundFetchHeader set to 1 since a fetch was
  // triggered for it in the background while rewriting the original html.
  CheckBackgroundFetch(headers, true);
  CheckNumBackgroundFetches(0);
  headers.ComputeCaching();
  EXPECT_LE(start_time_ms_ + Timer::kYearMs, headers.CacheExpirationTimeMs());
  EXPECT_EQ(kMinimizedCssContent, text);
}

TEST_F(ProxyInterfaceTest, FlushHugeHtml) {
  // Test the forced flushing of HTML controlled by flush_buffer_limit_bytes().
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_flush_buffer_limit_bytes(8);  // 2 self-closing tags ("<p/>")
  options->set_flush_html(true);
  server_context()->ComputeSignature(options);

  SetResponseWithDefaultHeaders("page.html", kContentTypeHtml,
                                "<a/><b/><c/><d/><e/><f/><g/><h/>",
                                kHtmlCacheTimeSec * 2);

  GoogleString out;
  FetchFromProxyLoggingFlushes("page.html", true /*success*/, &out);
  EXPECT_EQ(
      "<a/><b/>|Flush|<c/><d/>|Flush|<e/><f/>|Flush|<g/><h/>|Flush||Flush|",
      out);

  // Now tell to flush after 3 self-closing tags.
  options->ClearSignatureForTesting();
  options->set_flush_buffer_limit_bytes(12);  // 3 self-closing tags
  server_context()->ComputeSignature(options);

  FetchFromProxyLoggingFlushes("page.html", true /*success*/, &out);
  EXPECT_EQ(
      "<a/><b/><c/>|Flush|<d/><e/><f/>|Flush|<g/><h/>|Flush|", out);

  // And now with 2.5. This means we will flush 2 (as that many are complete),
  // then 5, and 7.
  options->ClearSignatureForTesting();
  options->set_flush_buffer_limit_bytes(10);
  server_context()->ComputeSignature(options);

  FetchFromProxyLoggingFlushes("page.html", true /*success*/, &out);
  EXPECT_EQ(
      "<a/><b/>|Flush|<c/><d/><e/>|Flush|<f/><g/>|Flush|<h/>|Flush|", out);

  // Now 9 bytes, e.g. 2 1/4 of a self-closing tag. Looks almost the same as
  // every 2 self-closing tags (8 bytes), but we don't get an extra flush
  // at the end.
  options->ClearSignatureForTesting();
  options->set_flush_buffer_limit_bytes(9);
  server_context()->ComputeSignature(options);
  FetchFromProxyLoggingFlushes("page.html", true /*success*/, &out);
  EXPECT_EQ(
      "<a/><b/>|Flush|<c/><d/>|Flush|<e/><f/>|Flush|<g/><h/>|Flush|",
      out);
}

TEST_F(ProxyInterfaceTest, DontRewriteDisallowedHtml) {
  // Blacklisted URL should not be rewritten.
  SetResponseWithDefaultHeaders("blacklist.html", kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2),
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy("blacklist.html", true, &text, &headers);
  CheckHeaders(headers, kContentTypeHtml);
  EXPECT_EQ(CssLinkHref("a.css"), text);
}

TEST_F(ProxyInterfaceTest, DontRewriteDisallowedHtmlRejectMode) {
  // If we're in reject_blacklisted mode, we should just respond with the
  // configured status.
  RejectBlacklisted();
  SetResponseWithDefaultHeaders("blacklistCoffee.html", kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2),
  SetResponseWithDefaultHeaders("tea.html", kContentTypeHtml,
                                "tasty", kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy("blacklistCoffee.html", true, &text, &headers);
  EXPECT_EQ(HttpStatus::kImATeapot, headers.status_code());
  EXPECT_TRUE(text.empty());

  // Fetching non-blacklisted one works fine.
  FetchFromProxy("tea.html", true, &text, &headers);
  EXPECT_EQ(HttpStatus::kOK, headers.status_code());
  EXPECT_STREQ("tasty", text);
}

TEST_F(ProxyInterfaceTest, DontRewriteMislabeledAsHtml) {
  // Make sure we don't rewrite things that claim to be HTML, but aren't.
  GoogleString text;
  ResponseHeaders headers;

  SetResponseWithDefaultHeaders("page.js", kContentTypeHtml,
                                StrCat("//", CssLinkHref("a.css")),
                                kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  FetchFromProxy("page.js", true, &text, &headers);
  CheckHeaders(headers, kContentTypeHtml);
  EXPECT_EQ(StrCat("//", CssLinkHref("a.css")), text);
}

TEST_F(ProxyInterfaceTest, ReconstructResource) {
  GoogleString text;
  ResponseHeaders headers;

  // Fetching of a rewritten resource we did not just create
  // after an HTML rewrite.
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);
  FetchFromProxy(Encode("", "cf", "0", "a.css", "css"), true, &text, &headers);
  CheckHeaders(headers, kContentTypeCss);
  headers.ComputeCaching();
  CheckBackgroundFetch(headers, false);
  EXPECT_LE(start_time_ms_ + Timer::kYearMs, headers.CacheExpirationTimeMs());
  EXPECT_EQ(kMinimizedCssContent, text);
}

TEST_F(ProxyInterfaceTest, ReconstructResourceCustomOptions) {
  const char kCssWithEmbeddedImage[] = "*{background-image:url(%s)}";
  const char kBackgroundImage[] = "1.png";

  GoogleString text;
  ResponseHeaders headers;

  // We're not going to image-compress so we don't need our mock image
  // to really be an image.
  SetResponseWithDefaultHeaders(kBackgroundImage, kContentTypePng, "image",
                                kHtmlCacheTimeSec * 2);
  GoogleString orig_css = StringPrintf(kCssWithEmbeddedImage, kBackgroundImage);
  SetResponseWithDefaultHeaders("embedded.css", kContentTypeCss,
                                orig_css, kHtmlCacheTimeSec * 2);

  // By default, cache extension is off in the default options.
  server_context()->global_options()->SetDefaultRewriteLevel(
      RewriteOptions::kPassThrough);
  ASSERT_FALSE(options()->Enabled(RewriteOptions::kExtendCacheCss));
  ASSERT_FALSE(options()->Enabled(RewriteOptions::kExtendCacheImages));
  ASSERT_FALSE(options()->Enabled(RewriteOptions::kExtendCacheScripts));
  ASSERT_FALSE(options()->Enabled(RewriteOptions::kExtendCachePdfs));
  ASSERT_EQ(RewriteOptions::kPassThrough, options()->level());

  // Because cache-extension was turned off, the image in the CSS file
  // will not be changed.
  FetchFromProxy("I.embedded.css.pagespeed.cf.0.css", true, &text, &headers);
  EXPECT_EQ(orig_css, text);

  // Now turn on cache-extension for custom options.  Invalidate cache entries
  // up to and including the current timestamp and advance by 1ms, otherwise
  // the previously stored embedded.css.pagespeed.cf.0.css will get re-used.
  scoped_ptr<RewriteOptions> custom_options(factory()->NewRewriteOptions());
  custom_options->EnableFilter(RewriteOptions::kExtendCacheCss);
  custom_options->EnableFilter(RewriteOptions::kExtendCacheImages);
  custom_options->EnableFilter(RewriteOptions::kExtendCacheScripts);
  custom_options->EnableFilter(RewriteOptions::kExtendCachePdfs);
  custom_options->set_cache_invalidation_timestamp(timer()->NowMs());
  AdvanceTimeUs(Timer::kMsUs);

  // Inject the custom options into the flow via a custom URL namer.
  ProxyUrlNamer url_namer;
  url_namer.set_options(custom_options.get());
  server_context()->set_url_namer(&url_namer);

  // Use EncodeNormal because it matches the logic used by ProxyUrlNamer.
  const GoogleString kExtendedBackgroundImage =
      EncodeNormal(kTestDomain, "ce", "0", kBackgroundImage, "png");

  // Now when we fetch the options, we'll find the image in the CSS
  // cache-extended.
  text.clear();
  FetchFromProxy("I.embedded.css.pagespeed.cf.0.css", true, &text, &headers);
  EXPECT_EQ(StringPrintf(kCssWithEmbeddedImage,
                         kExtendedBackgroundImage.c_str()),
            text);
}

TEST_F(ProxyInterfaceTest, MinResourceTimeZero) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  options->set_min_resource_cache_time_to_rewrite_ms(
      kHtmlCacheTimeSec * Timer::kSecondMs);
  server_context()->ComputeSignature(options);

  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy(kPageUrl, true, &text, &headers);
  EXPECT_EQ(CssLinkHref(Encode(kTestDomain, "cf", "0", "a.css", "css")), text);
}

TEST_F(ProxyInterfaceTest, MinResourceTimeLarge) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  options->set_min_resource_cache_time_to_rewrite_ms(
      4 * kHtmlCacheTimeSec * Timer::kSecondMs);
  server_context()->ComputeSignature(options);

  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                CssLinkHref("a.css"), kHtmlCacheTimeSec * 2);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCssContent,
                                kHtmlCacheTimeSec * 2);

  GoogleString text;
  ResponseHeaders headers;
  FetchFromProxy(kPageUrl, true, &text, &headers);
  EXPECT_EQ(CssLinkHref("a.css"), text);
}

TEST_F(ProxyInterfaceTest, CacheRequests) {
  ResponseHeaders html_headers;
  DefaultResponseHeaders(kContentTypeHtml, kHtmlCacheTimeSec, &html_headers);
  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "2");
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");

  // Original response is still cached in both cases, so we do not
  // fetch the new values.
  text.clear();
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);
}

// Verifies that we proxy uncacheable resources, but do not insert them in the
// cache.
TEST_F(ProxyInterfaceTest, UncacheableResourcesNotCachedOnProxy) {
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                     300 * Timer::kSecondMs, ", private");
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  ProxyUrlNamer url_namer;
  server_context()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;

  // We should not cache while fetching via kProxyHost.
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/style.css"),
      true, &out_text, &out_headers);
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());  // mapping, input resource
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // input resource
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());

  // We should likewise not cache while fetching on the origin domain.
  out_text.clear();
  ClearStats();
  FetchFromProxy("style.css", true, &out_text, &out_headers);
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());  // mapping, input resource
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // input resource
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());

  // Since the original response is not cached, we should pick up changes in the
  // input resource immediately.
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");
  out_text.clear();
  ClearStats();
  FetchFromProxy("style.css", true, &out_text, &out_headers);
  EXPECT_EQ("b", out_text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(2, lru_cache()->num_misses());  // mapping, input resource
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // input resource
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(0, http_cache()->cache_inserts()->Get());
}

// Verifies that we retrieve and serve uncacheable resources, but do not insert
// them in the cache.
TEST_F(ProxyInterfaceTest, UncacheableResourcesNotCachedOnResourceFetch) {
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                     300 * Timer::kSecondMs, ", private");
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->SetRewriteLevel(RewriteOptions::kPassThrough);
  options->EnableFilter(RewriteOptions::kRewriteCss);
  server_context()->ComputeSignature(options);

  ResponseHeaders out_headers;
  GoogleString out_text;

  // cf is not on-the-fly, and we can reconstruct it while keeping it private.
  FetchFromProxy(Encode(kTestDomain, "cf", "0", "style.css", "css"),
                 true, &out_text, &out_headers);
  EXPECT_TRUE(out_headers.HasValue(HttpAttributes::kCacheControl, "private"));
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(4, lru_cache()->num_misses());  // 2x output, metadata, input
  EXPECT_EQ(3, http_cache()->cache_misses()->Get());  // 2x output, input
  EXPECT_EQ(2, lru_cache()->num_inserts());  // mapping, uncacheable memo
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());  // uncacheable memo

  out_text.clear();
  ClearStats();
  // ce is on-the-fly, and we can recover even though style.css is private.
  FetchFromProxy(Encode(kTestDomain, "ce", "0", "style.css", "css"),
                 true, &out_text, &out_headers);
  EXPECT_TRUE(out_headers.HasValue(HttpAttributes::kCacheControl, "private"));
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(1, lru_cache()->num_hits());  // input uncacheable memo
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // input uncacheable memo
  EXPECT_EQ(1, lru_cache()->num_inserts());  // mapping
  EXPECT_EQ(1, lru_cache()->num_identical_reinserts());  // uncacheable memo
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());  // uncacheable memo

  out_text.clear();
  ClearStats();
  FetchFromProxy(Encode(kTestDomain, "ce", "0", "style.css", "css"),
                 true, &out_text, &out_headers);
  EXPECT_TRUE(out_headers.HasValue(HttpAttributes::kCacheControl, "private"));
  EXPECT_EQ("a", out_text);
  EXPECT_EQ(1, lru_cache()->num_hits());  // uncacheable memo
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // uncacheable memo
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_identical_reinserts())
      << "uncacheable memo, metadata";
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());  // uncacheable memo

  // Since the original response is not cached, we should pick up changes in the
  // input resource immediately.
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");
  out_text.clear();
  ClearStats();
  FetchFromProxy(Encode(kTestDomain, "ce", "0", "style.css", "css"),
                 true, &out_text, &out_headers);
  EXPECT_TRUE(out_headers.HasValue(HttpAttributes::kCacheControl, "private"));
  EXPECT_EQ("b", out_text);
  EXPECT_EQ(1, lru_cache()->num_hits());  // uncacheable memo
  EXPECT_EQ(0, http_cache()->cache_hits()->Get());
  EXPECT_EQ(0, lru_cache()->num_misses());
  EXPECT_EQ(1, http_cache()->cache_misses()->Get());  // uncacheable memo
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_identical_reinserts())
      << "uncacheable memo, metadata";
  EXPECT_EQ(1, http_cache()->cache_inserts()->Get());  // uncacheable memo
}

// No matter what options->respect_vary() is set to we will respect HTML Vary
// headers.
TEST_F(ProxyInterfaceTest, NoCacheVaryHtml) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_respect_vary(false);
  server_context()->ComputeSignature(options);

  ResponseHeaders html_headers;
  DefaultResponseHeaders(kContentTypeHtml, kHtmlCacheTimeSec, &html_headers);
  html_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  html_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "2");
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");

  // HTML was not cached because of Vary: User-Agent header.
  // So we do fetch the new value.
  text.clear();
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("2", text);
  // Resource was cached because we have respect_vary == false.
  // So we serve the old value.
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);
}

// Test https HTML responses are never cached, while https resources are cached.
TEST_F(ProxyInterfaceTest, NoCacheHttpsHtml) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_respect_vary(false);
  server_context()->ComputeSignature(options);
  http_cache()->set_disable_html_caching_on_https(true);

  ResponseHeaders html_headers;
  DefaultResponseHeaders(kContentTypeHtml, kHtmlCacheTimeSec, &html_headers);
  html_headers.ComputeCaching();
  SetFetchResponse(kHttpsPageUrl, html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.ComputeCaching();
  SetFetchResponse(kHttpsCssUrl, resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy(kHttpsPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy(kHttpsCssUrl, true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(kHttpsPageUrl, html_headers, "2");
  SetFetchResponse(kHttpsCssUrl, resource_headers, "b");

  ClearStats();
  // HTML was not cached because it was via https. So we do fetch the new value.
  text.clear();
  FetchFromProxy(kHttpsPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("2", text);
  EXPECT_EQ(0, lru_cache()->num_hits());
  // Resource was cached, so we serve the old value.
  text.clear();
  FetchFromProxy(kHttpsCssUrl, true, &text, &actual_headers);
  EXPECT_EQ("a", text);
  EXPECT_EQ(1, http_cache()->cache_hits()->Get());
}

// Respect Vary for resources if options tell us to.
TEST_F(ProxyInterfaceTest, NoCacheVaryAll) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_respect_vary(true);
  server_context()->ComputeSignature(options);

  ResponseHeaders html_headers;
  DefaultResponseHeaders(kContentTypeHtml, kHtmlCacheTimeSec, &html_headers);
  html_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  html_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "1");
  ResponseHeaders resource_headers;
  DefaultResponseHeaders(kContentTypeCss, kHtmlCacheTimeSec, &resource_headers);
  resource_headers.Add(HttpAttributes::kVary, HttpAttributes::kUserAgent);
  resource_headers.ComputeCaching();
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "a");

  GoogleString text;
  ResponseHeaders actual_headers;
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("1", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("a", text);

  SetFetchResponse(AbsolutifyUrl(kPageUrl), html_headers, "2");
  SetFetchResponse(AbsolutifyUrl("style.css"), resource_headers, "b");

  // Original response was not cached in either case, so we do fetch the
  // new value.
  text.clear();
  FetchFromProxy(kPageUrl, true, &text, &actual_headers);
  EXPECT_EQ("2", text);
  text.clear();
  FetchFromProxy("style.css", true, &text, &actual_headers);
  EXPECT_EQ("b", text);
}

TEST_F(ProxyInterfaceTest, Blacklist) {
  const char content[] =
      "<html>\n"
      "  <head/>\n"
      "  <body>\n"
      "    <script src='tiny_mce.js'></script>\n"
      "  </body>\n"
      "</html>\n";
  SetResponseWithDefaultHeaders("tiny_mce.js", kContentTypeJavascript, "", 100);
  ValidateNoChanges("blacklist", content);

  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml, content, 0);
  GoogleString text_out;
  ResponseHeaders headers_out;
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_STREQ(content, text_out);
}

TEST_F(ProxyInterfaceTest, RepairMismappedResource) {
  // Teach the mock fetcher to serve origin content for
  // "http://test.com/foo.js".
  const char kContent[] = "function f() {alert('foo');}";
  SetResponseWithDefaultHeaders("foo.js", kContentTypeHtml, kContent,
                                kHtmlCacheTimeSec * 2);

  // Set up a Mock Namer that will mutate output resources to
  // be served on proxy_host.com, encoding the origin URL.
  ProxyUrlNamer url_namer;
  ResponseHeaders headers;
  GoogleString text;
  server_context()->set_url_namer(&url_namer);

  // Now fetch the origin content.  This will simply hit the
  // mock fetcher and always worked.
  FetchFromProxy("foo.js", true, &text, &headers);
  EXPECT_EQ(kContent, text);

  // Now make a weird URL encoding of the origin resource using the
  // proxy host.  This may happen via javascript that detects its
  // own path and initiates a 'load()' of another js file from the
  // same path.  In this variant, the resource is served from the
  // "source domain", so it is automatically whitelisted.
  text.clear();
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost, "/test.com/test.com/foo.js"),
      true, &text, &headers);
  EXPECT_EQ(kContent, text);

  // In the next case, the resource is served from a different domain.  This
  // is an open-proxy vulnerability and thus should fail.
  text.clear();
  url_namer.set_authorized(false);
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost, "/test.com/evil.com/foo.js"),
      false, &text, &headers);
}

TEST_F(ProxyInterfaceTest, CrossDomainHeaders) {
  // If we're serving content from test.com via kProxyHost URL, we need to make
  // sure that cookies are not propagated, as evil.com could also be potentially
  // proxied via kProxyHost.
  const char kText[] = "* { pretty; }";

  ResponseHeaders orig_headers;
  DefaultResponseHeaders(kContentTypeCss, 100, &orig_headers);
  orig_headers.Add(HttpAttributes::kSetCookie, "tasty");
  SetFetchResponse("http://test.com/file.css", orig_headers, kText);

  ProxyUrlNamer url_namer;
  server_context()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/file.css"),
      true, &out_text, &out_headers);
  EXPECT_STREQ(kText, out_text);
  EXPECT_STREQ(NULL, out_headers.Lookup1(HttpAttributes::kSetCookie));
}

TEST_F(ProxyInterfaceTest, CrossDomainAuthorization) {
  // If we're serving content from evil.com via kProxyHostUrl, we need to make
  // sure we don't propagate through any (non-proxy) authorization headers, as
  // they may have been cached from good.com (as both would look like
  // kProxyHost to the browser).
  ReflectingTestFetcher reflect;
  server_context()->set_default_system_fetcher(&reflect);

  ProxyUrlNamer url_namer;
  server_context()->set_url_namer(&url_namer);

  RequestHeaders request_headers;
  request_headers.Add("Was", "Here");
  request_headers.Add(HttpAttributes::kAuthorization, "Secret");
  request_headers.Add(HttpAttributes::kProxyAuthorization, "OurSecret");

  ResponseHeaders out_headers;
  GoogleString out_text;
  // Using .txt here so we don't try any AJAX rewriting.
  FetchFromProxy(StrCat("http://", ProxyUrlNamer::kProxyHost,
                        "/test.com/test.com/file.txt"),
                 request_headers,  true, &out_text, &out_headers);
  EXPECT_STREQ("Here", out_headers.Lookup1("Was"));
  EXPECT_FALSE(out_headers.Has(HttpAttributes::kAuthorization));
  EXPECT_FALSE(out_headers.Has(HttpAttributes::kProxyAuthorization));
  mock_scheduler()->AwaitQuiescence();
}

TEST_F(ProxyInterfaceTest, CrossDomainHeadersWithUncacheableResourceOnProxy) {
  // Check that we do not propagate cookies from test.com via kProxyHost URL,
  // as in CrossDomainHeaders above.  Also check that we do propagate cache
  // control.
  const char kText[] = "* { pretty; }";

  ResponseHeaders orig_headers;
  DefaultResponseHeaders(kContentTypeCss, 100, &orig_headers);
  orig_headers.Add(HttpAttributes::kSetCookie, "tasty");
  orig_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                 400 * Timer::kSecondMs, ", private");
  orig_headers.ComputeCaching();
  SetFetchResponse("http://test.com/file.css", orig_headers, kText);

  ProxyUrlNamer url_namer;
  server_context()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/file.css"),
      true, &out_text, &out_headers);

  // Check that we ate the cookies.
  EXPECT_STREQ(kText, out_text);
  ConstStringStarVector values;
  out_headers.Lookup(HttpAttributes::kSetCookie, &values);
  EXPECT_EQ(0, values.size());

  // Check that the resource Cache-Control has been preserved.
  values.clear();
  out_headers.Lookup(HttpAttributes::kCacheControl, &values);
  ASSERT_EQ(2, values.size());
  EXPECT_STREQ("max-age=400", *values[0]);
  EXPECT_STREQ("private", *values[1]);
}

TEST_F(ProxyInterfaceTest, CrossDomainHeadersWithUncacheableResourceOnFetch) {
  // Check that we do not propagate cookies from test.com via a resource fetch,
  // as in CrossDomainHeaders above.  Also check that we do propagate cache
  // control, and that we run the filter specified in the resource fetch URL.
  // Note that the running of filters at present can only happen if
  // the filter is on the-fly.
  const char kText[] = "* { pretty; }";

  ResponseHeaders orig_headers;
  DefaultResponseHeaders(kContentTypeCss, 100, &orig_headers);
  orig_headers.Add(HttpAttributes::kSetCookie, "tasty");
  orig_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                 400 * Timer::kSecondMs, ", private");
  orig_headers.ComputeCaching();
  SetFetchResponse("http://test.com/file.css", orig_headers, kText);

  ProxyUrlNamer url_namer;
  server_context()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(Encode(kTestDomain, "ce", "0", "file.css", "css"),
                 true, &out_text, &out_headers);

  // Check that we passed through the CSS.
  EXPECT_STREQ(kText, out_text);
  // Check that we ate the cookies.
  ConstStringStarVector values;
  out_headers.Lookup(HttpAttributes::kSetCookie, &values);
  EXPECT_EQ(0, values.size());

  // Check that the resource Cache-Control has been preserved.
  // max-age actually gets smaller, though, since this also triggers
  // a rewrite failure.
  values.clear();
  out_headers.Lookup(HttpAttributes::kCacheControl, &values);
  ASSERT_EQ(2, values.size());
  EXPECT_STREQ("max-age=300", *values[0]);
  EXPECT_STREQ("private", *values[1]);
}

TEST_F(ProxyInterfaceTest, CrossDomainHeadersWithUncacheableResourceOnFetch2) {
  // Variant of the above with a non-on-the-fly filter.
  const char kText[] = "* { pretty; }";

  ResponseHeaders orig_headers;
  DefaultResponseHeaders(kContentTypeCss, 100, &orig_headers);
  orig_headers.Add(HttpAttributes::kSetCookie, "tasty");
  orig_headers.SetDateAndCaching(http_cache()->timer()->NowMs(),
                                 400 * Timer::kSecondMs, ", private");
  orig_headers.ComputeCaching();
  SetFetchResponse("http://test.com/file.css", orig_headers, kText);

  ProxyUrlNamer url_namer;
  server_context()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(Encode(kTestDomain, "cf", "0", "file.css", "css"),
                 true, &out_text, &out_headers);
  // Proper output
  EXPECT_STREQ("*{pretty}", out_text);

  // Private.
  ConstStringStarVector values;
  out_headers.Lookup(HttpAttributes::kCacheControl, &values);
  ASSERT_EQ(2, values.size());
  EXPECT_STREQ("max-age=400", *values[0]);
  EXPECT_STREQ("private", *values[1]);

  // Check that we ate the cookies.
  EXPECT_FALSE(out_headers.Has(HttpAttributes::kSetCookie));
}

TEST_F(ProxyInterfaceTest, ProxyResourceQueryOnly) {
  // At one point we had a bug where if we optimized a pagespeed resource
  // whose original name was a bare query, we would loop infinitely when
  // trying to fetch it from a separate-domain proxy.
  const char kUrl[] = "?somestuff";
  SetResponseWithDefaultHeaders(kUrl, kContentTypeJavascript,
                                "var a = 2;// stuff", kHtmlCacheTimeSec * 2);

  ProxyUrlNamer url_namer;
  server_context()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/",
             EncodeNormal("", "jm", "0", kUrl, "css")),
      true, &out_text, &out_headers);
  EXPECT_STREQ("var a=2;", out_text);
  CheckBackgroundFetch(out_headers, false);
}

TEST_F(ProxyInterfaceTest, NoRehostIncompatMPS) {
  // Make sure we don't try to interpret a URL from an incompatible
  // mod_pagespeed version at our proxy host level.

  // This url will be rejected by CssUrlEncoder
  const char kOldName[] = "style.css.pagespeed.cf.0.css";
  const char kContent[] = "*     {}";
  SetResponseWithDefaultHeaders(kOldName, kContentTypeCss, kContent, 100);

  ProxyUrlNamer url_namer;
  server_context()->set_url_namer(&url_namer);
  ResponseHeaders out_headers;
  GoogleString out_text;
  FetchFromProxy(
      StrCat("http://", ProxyUrlNamer::kProxyHost,
             "/test.com/test.com/",
             EncodeNormal("", "ce", "0", kOldName, "css")),
      true, &out_text, &out_headers);
  EXPECT_EQ(HttpStatus::kOK, out_headers.status_code());
  EXPECT_STREQ(kContent, out_text);
}

// Test that we serve "Cache-Control: no-store" only when original page did.
TEST_F(ProxyInterfaceTest, NoStore) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_max_html_cache_time_ms(0);
  server_context()->ComputeSignature(options);

  // Most headers get converted to "no-cache, max-age=0".
  EXPECT_STREQ("max-age=0, no-cache",
               RewriteHtmlCacheHeader("empty", ""));
  EXPECT_STREQ("max-age=0, no-cache",
               RewriteHtmlCacheHeader("private", "private, max-age=100"));
  EXPECT_STREQ("max-age=0, no-cache",
               RewriteHtmlCacheHeader("no-cache", "no-cache"));

  // Headers with "no-store", preserve that header as well.
  EXPECT_STREQ("max-age=0, no-cache, no-store",
               RewriteHtmlCacheHeader("no-store", "no-cache, no-store"));
  EXPECT_STREQ("max-age=0, no-cache, no-store",
               RewriteHtmlCacheHeader("no-store2", "no-store, max-age=300"));
}

TEST_F(ProxyInterfaceTest, PropCacheFilter) {
  CreateFilterCallback create_filter_callback;
  factory()->AddCreateFilterCallback(&create_filter_callback);
  EnableDomCohortWritesWithDnsPrefetch();

  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                "<div><p></p></div>", 0);
  GoogleString text_out;
  ResponseHeaders headers_out;

  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ("<!-- --><div><p></p></div>", text_out);

  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ("<!-- 2 elements unstable --><div><p></p></div>", text_out);

  // How many refreshes should we require before it's stable?  That
  // tuning can be done in the PropertyCacheTest.  For this
  // system-test just do a hundred blind refreshes and check again for
  // stability.
  const int kFetchIterations = 100;
  for (int i = 0; i < kFetchIterations; ++i) {
    FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  }

  // Must be stable by now!
  EXPECT_EQ("<!-- 2 elements stable --><div><p></p></div>", text_out);

  // In this algorithm we will spend a property-cache-write per fetch.
  //
  // We'll also check that we do no cache writes when there are no properties
  // to save.
  EXPECT_EQ(2 + kFetchIterations, lru_cache()->num_inserts());

  // Now change the HTML and watch the #elements change.
  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                "<div><span><p></p></span></div>", 0);
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ("<!-- 3 elements stable --><div><span><p></p></span></div>",
            text_out);

  ClearStats();

  // Finally, disable the property-cache and note that the element-count
  // annotatation reverts to "unknown mode"
  server_context_->set_enable_property_cache(false);
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ("<!-- --><div><span><p></p></span></div>", text_out);
}

TEST_F(ProxyInterfaceTest, DomCohortWritten) {
  // Other than the write of DomCohort, there will be no properties added to
  // the cache in this test because we have not enabled the filter with
  //     CreateFilterCallback create_filter_callback;
  //     factory()->AddCreateFilterCallback(&callback);

  DisableAjax();
  GoogleString text_out;
  ResponseHeaders headers_out;

  // No writes should occur if no filter that uses the dom cohort is enabled.
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_misses());  // property-cache + http-cache

  // Enable a filter that uses the dom cohort and make sure property cache is
  // updated.
  ClearStats();
  EnableDomCohortWritesWithDnsPrefetch();
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ(1, lru_cache()->num_inserts());
  EXPECT_EQ(2, lru_cache()->num_misses());  // property-cache + http-cache

  ClearStats();
  server_context_->set_enable_property_cache(false);
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_misses());  // http-cache only.
}

TEST_F(ProxyInterfaceTest, StatusCodePropertyWritten) {
  DisableAjax();
  EnableDomCohortWritesWithDnsPrefetch();

  GoogleString text_out;
  ResponseHeaders headers_out;

  // Status code 404 gets written when page is not available.
  SetFetchResponse404(kPageUrl);
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ(HttpStatus::kNotFound,
            GetStatusCodeInPropertyCache(StrCat(kTestDomain, kPageUrl)));

  // Status code 200 gets written when page is available.
  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                "<html></html>", kHtmlCacheTimeSec);
  lru_cache()->Clear();
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ(HttpStatus::kOK,
            GetStatusCodeInPropertyCache(StrCat(kTestDomain, kPageUrl)));
  // Status code 301 gets written when it is a permanent redirect.
  headers_out.Clear();
  text_out.clear();
  headers_out.SetStatusAndReason(HttpStatus::kMovedPermanently);
  SetFetchResponse(StrCat(kTestDomain, kPageUrl), headers_out, text_out);
  lru_cache()->Clear();
  FetchFromProxy(kPageUrl, true, &text_out, &headers_out);
  EXPECT_EQ(HttpStatus::kMovedPermanently,
            GetStatusCodeInPropertyCache(StrCat(kTestDomain, kPageUrl)));
}

TEST_F(ProxyInterfaceTest, PropCacheNoWritesIfHtmlEndsWithTxt) {
  CreateFilterCallback create_filter_callback;
  factory()->AddCreateFilterCallback(&create_filter_callback);

  // There will be no properties added to the cache set in this test because
  // we have not enabled the filter with
  //     CreateFilterCallback create_filter_callback;
  //     factory()->AddCreateFilterCallback(&callback);

  DisableAjax();
  SetResponseWithDefaultHeaders("page.txt", kContentTypeHtml,
                                "<div><p></p></div>", 0);
  GoogleString text_out;
  ResponseHeaders headers_out;

  FetchFromProxy("page.txt", true, &text_out, &headers_out);
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_misses());  // http-cache only

  ClearStats();
  server_context_->set_enable_property_cache(false);
  FetchFromProxy("page.txt", true, &text_out, &headers_out);
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_misses());  // http-cache only
}

TEST_F(ProxyInterfaceTest, PropCacheNoWritesForNonGetRequests) {
  CreateFilterCallback create_filter_callback;
  factory()->AddCreateFilterCallback(&create_filter_callback);

  DisableAjax();
  SetResponseWithDefaultHeaders("page.txt", kContentTypeHtml,
                                "<div><p></p></div>", 0);
  GoogleString text_out;
  ResponseHeaders headers_out;
  RequestHeaders request_headers;
  request_headers.set_method(RequestHeaders::kPost);

  FetchFromProxy("page.txt", true, &text_out, &headers_out);
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_misses());  // http-cache only

  ClearStats();
  server_context_->set_enable_property_cache(false);
  FetchFromProxy("page.txt", true, &text_out, &headers_out);
  EXPECT_EQ(0, lru_cache()->num_inserts());
  EXPECT_EQ(1, lru_cache()->num_misses());  // http-cache only
}

TEST_F(ProxyInterfaceTest, PropCacheNoWritesIfNonHtmlDelayedCache) {
  DisableAjax();
  TestPropertyCache(kImageFilenameLackingExt, true, false, true);
}

TEST_F(ProxyInterfaceTest, PropCacheNoWritesIfNonHtmlImmediateCache) {
  // Tests rewriting a file that turns out to be a jpeg, but lacks an
  // extension, where the property-cache lookup is delivered immediately.
  DisableAjax();
  TestPropertyCache(kImageFilenameLackingExt, false, false, true);
}

TEST_F(ProxyInterfaceTest, PropCacheNoWritesIfNonHtmlThreadedCache) {
  // Tests rewriting a file that turns out to be a jpeg, but lacks an
  // extension, where the property-cache lookup is delivered in a
  // separate thread.
  DisableAjax();
  ThreadSynchronizer* sync = server_context()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kCollectorPrefix);
  TestPropertyCache(kImageFilenameLackingExt, true, true, true);
}

TEST_F(ProxyInterfaceTest, StatusCodeUpdateRace) {
  // Tests rewriting a file that turns out to be a jpeg, but lacks an
  // extension, where the property-cache lookup is delivered in a
  // separate thread. Use sync points to ensure that Done() deletes the
  // collector just after the Detach() critical block is executed.
  DisableAjax();
  ThreadSynchronizer* sync = server_context()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kCollectorDetach);
  sync->EnableForPrefix(ProxyFetch::kCollectorDoneDelete);
  TestPropertyCache(kImageFilenameLackingExt, false, true, true);
}

TEST_F(ProxyInterfaceTest, ThreadedHtml) {
  // Tests rewriting HTML resource where property-cache lookup is delivered
  // in a separate thread.
  DisableAjax();
  EnableDomCohortWritesWithDnsPrefetch();
  ThreadSynchronizer* sync = server_context()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kCollectorPrefix);
  TestPropertyCache(kPageUrl, true, true, true);
}

TEST_F(ProxyInterfaceTest, ThreadedHtmlFetcherFailure) {
  // Tests rewriting HTML resource where property-cache lookup is delivered
  // in a separate thread, but the HTML lookup fails after emitting the
  // body.
  DisableAjax();
  EnableDomCohortWritesWithDnsPrefetch();
  mock_url_fetcher()->SetResponseFailure(AbsolutifyUrl(kPageUrl));
  TestPropertyCache(kPageUrl, true, true, false);
}

TEST_F(ProxyInterfaceTest, HtmlFetcherFailure) {
  // Tests rewriting HTML resource where property-cache lookup is
  // delivered in a blocking fashion, and the HTML lookup fails after
  // emitting the body.
  DisableAjax();
  EnableDomCohortWritesWithDnsPrefetch();
  mock_url_fetcher()->SetResponseFailure(AbsolutifyUrl(kPageUrl));
  TestPropertyCache(kPageUrl, false, false, false);
}

TEST_F(ProxyInterfaceTest, HeadersSetupRace) {
  //
  // This crash occured where an Idle-callback is used to flush HTML.
  // In this bug, we were connecting the property-cache callback to
  // the ProxyFetch and then mutating response-headers.  The property-cache
  // callback was waking up the QueuedWorkerPool::Sequence used by
  // the ProxyFetch, which was waking up and calling HeadersComplete.
  // If the implementation of HeadersComplete mutated headers itself,
  // we'd have a deadly race.
  //
  // This test uses the ThreadSynchronizer class to induce the desired
  // race, with strategically placed calls to Signal and Wait.
  //
  // Note that the fix for the race means that one of the Signals does
  // not occur at all, so we have to declare it as "Sloppy" so the
  // ThreadSynchronizer class doesn't vomit on destruction.
  const int kIdleCallbackTimeoutMs = 10;
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_idle_flush_time_ms(kIdleCallbackTimeoutMs);
  options->set_flush_html(true);
  server_context()->ComputeSignature(options);
  DisableAjax();
  EnableDomCohortWritesWithDnsPrefetch();
  ThreadSynchronizer* sync = server_context()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kHeadersSetupRacePrefix);
  ThreadSystem* thread_system = server_context()->thread_system();
  QueuedWorkerPool pool(1, thread_system);
  QueuedWorkerPool::Sequence* sequence = pool.NewSequence();
  WorkerTestBase::SyncPoint sync_point(thread_system);
  sequence->Add(MakeFunction(static_cast<ProxyInterfaceTest*>(this),
                             &ProxyInterfaceTest::TestHeadersSetupRace));
  sequence->Add(new WorkerTestBase::NotifyRunFunction(&sync_point));
  sync->TimedWait(ProxyFetch::kHeadersSetupRaceAlarmQueued,
                  ProxyFetch::kTestSignalTimeoutMs);
  {
    // Trigger the idle-callback, if it has been queued.
    ScopedMutex lock(mock_scheduler()->mutex());
    mock_scheduler()->ProcessAlarms(kIdleCallbackTimeoutMs * Timer::kMsUs);
  }
  sync->Wait(ProxyFetch::kHeadersSetupRaceDone);
  sync_point.Wait();
  pool.ShutDown();
  sync->AllowSloppyTermination(ProxyFetch::kHeadersSetupRaceAlarmQueued);
}

TEST_F(ProxyInterfaceTest, BothClientAndPropertyCache) {
  // Ensure that the ProxyFetchPropertyCallbackCollector calls its Post function
  // only once, despite the fact that we are doing two property-cache lookups.
  //
  // Note that ProxyFetchPropertyCallbackCollector::Done waits for
  // ProxyFetch::kCollectorDone.  We will signal it ahead of time so
  // if this is working properly, it won't block.  However, if the system
  // incorrectly calls Done() twice, then it will block forever on the
  // second call to Wait(ProxyFetch::kCollectorDone), since we only offer
  // one Signal here.
  ThreadSynchronizer* sync = server_context()->thread_synchronizer();
  sync->EnableForPrefix(ProxyFetch::kCollectorPrefix);
  sync->Signal(ProxyFetch::kCollectorDone);

  RequestHeaders request_headers;
  ResponseHeaders response_headers;
  request_headers.Add(HttpAttributes::kXGooglePagespeedClientId, "1");

  DisableAjax();
  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml,
                                "<div><p></p></div>", 0);
  GoogleString response;
  FetchFromProxy(kPageUrl, request_headers, true, &response, &response_headers);
  sync->Wait(ProxyFetch::kCollectorReady);  // Clears Signal from PFPCC::Done.
  sync->Wait(ProxyFetch::kCollectorDelete);
}

// TODO(jmarantz): add a test with a simulated slow cache to see what happens
// when the rest of the system must block, buffering up incoming HTML text,
// waiting for the property-cache lookups to complete.

// Test that we set the Furious cookie up appropriately.
TEST_F(ProxyInterfaceTest, FuriousTest) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->set_ga_id("123-455-2341");
  options->set_running_furious_experiment(true);
  NullMessageHandler handler;
  options->AddFuriousSpec("id=2;enable=extend_cache;percent=100", &handler);
  server_context()->ComputeSignature(options);

  SetResponseWithDefaultHeaders("example.jpg", kContentTypeJpeg,
                                "image data", 300);

  ResponseHeaders headers;
  const char kContent[] = "<html><head></head><body>A very compelling "
      "article with an image: <img src=example.jpg></body></html>";
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);
  headers.Clear();

  GoogleString text;
  FetchFromProxy("text.html", true, &text, &headers);
  // Assign all visitors to a furious_spec.
  EXPECT_TRUE(headers.Has(HttpAttributes::kSetCookie));
  ConstStringStarVector values;
  headers.Lookup(HttpAttributes::kSetCookie, &values);
  bool found = false;
  for (int i = 0, n = values.size(); i < n; ++i) {
    if (values[i]->find(furious::kFuriousCookie) == 0) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
  // Image cache-extended and including furious_spec 'a'.
  EXPECT_TRUE(text.find("example.jpg.pagespeed.a.ce") != GoogleString::npos);

  headers.Clear();
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  SetFetchResponse(AbsolutifyUrl("text2.html"), headers, kContent);
  headers.Clear();
  text.clear();

  RequestHeaders req_headers;
  req_headers.Add(HttpAttributes::kCookie, "_GFURIOUS=2");

  FetchFromProxy("text2.html", req_headers, true, &text, &headers);
  // Visitor already has cookie with id=2; don't give them a new one.
  EXPECT_FALSE(headers.Has(HttpAttributes::kSetCookie));
  // Image cache-extended and including furious_spec 'a'.
  EXPECT_TRUE(text.find("example.jpg.pagespeed.a.ce") != GoogleString::npos);

  // Check that we don't include a furious_spec index in urls for the "no
  // experiment" group (id=0).
  headers.Clear();
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  SetFetchResponse(AbsolutifyUrl("text3.html"), headers, kContent);
  headers.Clear();
  text.clear();

  RequestHeaders req_headers2;
  req_headers2.Add(HttpAttributes::kCookie, "_GFURIOUS=0");

  FetchFromProxy("text3.html", req_headers2, true, &text, &headers);
  EXPECT_FALSE(headers.Has(HttpAttributes::kSetCookie));
  EXPECT_TRUE(text.find("example.jpg.pagespeed.ce") != GoogleString::npos);
}

TEST_F(ProxyInterfaceTest, UrlAttributeTest) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->EnableFilter(RewriteOptions::kRewriteDomains);
  options->set_domain_rewrite_hyperlinks(true);
  NullMessageHandler handler;
  options->domain_lawyer()->AddRewriteDomainMapping(
      "http://dst.example.com", "http://src.example.com", &handler);
  options->AddUrlValuedAttribute(
      "span", "src", semantic_type::kHyperlink);
  options->AddUrlValuedAttribute("hr", "imgsrc", semantic_type::kImage);
  server_context()->ComputeSignature(options);

  SetResponseWithDefaultHeaders(
      "http://src.example.com/null", kContentTypeHtml, "", 0);
  ResponseHeaders headers;
  const char kContent[] = "<html><head></head><body>"
      "<img src=\"http://src.example.com/null\">"
      "<hr imgsrc=\"http://src.example.com/null\">"
      "<span src=\"http://src.example.com/null\"></span>"
      "<other src=\"http://src.example.com/null\"></other></body></html>";
  headers.Add(HttpAttributes::kContentType, kContentTypeHtml.mime_type());
  headers.SetStatusAndReason(HttpStatus::kOK);
  SetFetchResponse(AbsolutifyUrl("text.html"), headers, kContent);
  headers.Clear();
  GoogleString text;
  FetchFromProxy("text.html", true, &text, &headers);

  // img.src, hr.imgsrc, and span.src are all rewritten
  EXPECT_TRUE(text.find("<img src=\"http://dst.example.com/null\"") !=
              GoogleString::npos);
  EXPECT_TRUE(text.find("<hr imgsrc=\"http://dst.example.com/null\"") !=
              GoogleString::npos);
  EXPECT_TRUE(text.find("<span src=\"http://dst.example.com/null\"") !=
              GoogleString::npos);
  // other.src not rewritten
  EXPECT_TRUE(text.find("<other src=\"http://src.example.com/null\"") !=
              GoogleString::npos);
}

// Test that ClientState is properly read from the client property cache.
TEST_F(ProxyInterfaceTest, ClientStateTest) {
  CreateFilterCallback create_filter_callback;
  factory()->AddCreateFilterCallback(&create_filter_callback);
  EnableDomCohortWritesWithDnsPrefetch();

  SetResponseWithDefaultHeaders("page.html", kContentTypeHtml,
                                "<div><p></p></div>", 0);
  GoogleString text_out;
  ResponseHeaders headers_out;

  RequestHeaders request_headers;
  request_headers.Add(HttpAttributes::kXGooglePagespeedClientId, "clientid");

  // First pass: Should add fake URL to cache.
  FetchFromProxy("page.html",
                 request_headers,
                 true,
                 &text_out,
                 &headers_out);
  EXPECT_EQ(StrCat("<!-- ClientID: clientid ClientStateID: ",
                   "clientid InCache: true --><div><p></p></div>"),
            text_out);

  // Second pass: Should clear fake URL from cache.
  FetchFromProxy("page.html",
                 request_headers,
                 true,
                 &text_out,
                 &headers_out);
  EXPECT_EQ(StrCat("<!-- ClientID: clientid ClientStateID: clientid ",
                   "InCache: false 2 elements unstable --><div><p></p></div>"),
            text_out);
}

TEST_F(ProxyInterfaceTest, TestOptionsUsedInCacheKey) {
  TestOptionsUsedInCacheKey();
}

TEST_F(ProxyInterfaceTest, BailOutOfParsing) {
  RewriteOptions* options = server_context()->global_options();
  options->ClearSignatureForTesting();
  options->EnableExtendCacheFilters();
  options->set_max_html_parse_bytes(60);
  server_context()->ComputeSignature(options);

  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "1.jpg"), kContentTypeJpeg,
                                "image", kHtmlCacheTimeSec * 2);

  // This is larger than 60 bytes.
  const char kContent[] = "<html><head></head><body>"
      "<img src=\"1.jpg\">"
      "<p>Some very long and very boring text</p>"
      "</body></html>";
  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml, kContent, 0);
  ResponseHeaders headers;
  GoogleString text;
  FetchFromProxy(kPageUrl, true, &text, &headers);
  // For the first request, we bail out of parsing and insert the redirect. We
  // also update the pcache.
  EXPECT_EQ("<html><script type=\"text/javascript\">"
            "window.location=\"http://test.com/page.html?ModPagespeed=off\";"
            "</script></html>", text);

  headers.Clear();
  text.clear();
  // We look up the pcache and find that we should skip parsing. Hence, we just
  // pass the bytes through.
  FetchFromProxy(kPageUrl, true, &text, &headers);
  EXPECT_EQ(kContent, text);

  // This is smaller than 60 bytes.
  const char kNewContent[] = "<html><head></head><body>"
       "<img src=\"1.jpg\"></body></html>";

  SetResponseWithDefaultHeaders(kPageUrl, kContentTypeHtml, kNewContent, 0);
  headers.Clear();
  text.clear();
  // We still remember that we should skip parsing. Hence, we pass the bytes
  // through. However, after this request, we update the pcache to indicate that
  // we should no longer skip parsing.
  FetchFromProxy(kPageUrl, true, &text, &headers);
  EXPECT_EQ(kNewContent, text);

  headers.Clear();
  text.clear();
  // This request is rewritten.
  FetchFromProxy(kPageUrl, true, &text, &headers);
  EXPECT_EQ("<html><head></head><body>"
            "<img src=\"http://test.com/1.jpg.pagespeed.ce.0.jpg\">"
            "</body></html>", text);
}

}  // namespace net_instaweb
