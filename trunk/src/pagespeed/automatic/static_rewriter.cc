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

// Author: jmarantz@google.com (Joshua Marantz)

#include "pagespeed/automatic/public/static_rewriter.h"

#include <cstdio>
#include <cstdlib>  // for exit()

#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/http/public/wget_url_fetcher.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_gflags.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/md5_hasher.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/cache/threadsafe_cache.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"

namespace net_instaweb {

class CacheInterface;
class FileSystem;
class Hasher;
class MessageHandler;
class ProcessContext;
class RewriteOptions;
class Statistics;
class UrlAsyncFetcher;
class Writer;

namespace {

class FileServerContext : public ServerContext {
 public:
  explicit FileServerContext(RewriteDriverFactory* factory)
      : ServerContext(factory) {
  }

  virtual ~FileServerContext() {
  }

  virtual bool ProxiesHtml() const { return false; }
};

}  // namespace

FileRewriter::FileRewriter(const ProcessContext& process_context,
                           const net_instaweb::RewriteGflags* gflags,
                           bool echo_errors_to_stdout)
    : RewriteDriverFactory(process_context, Platform::CreateThreadSystem()),
      gflags_(gflags),
      simple_stats_(thread_system()),
      echo_errors_to_stdout_(echo_errors_to_stdout) {
  net_instaweb::RewriteDriverFactory::InitStats(&simple_stats_);
  SetStatistics(&simple_stats_);
}

FileRewriter::~FileRewriter() {
}

Hasher* FileRewriter::NewHasher() {
  return new MD5Hasher;
}

UrlAsyncFetcher* FileRewriter::DefaultAsyncUrlFetcher() {
  return new WgetUrlFetcher;
}

MessageHandler* FileRewriter::DefaultHtmlParseMessageHandler() {
  if (echo_errors_to_stdout_) {
    return new GoogleMessageHandler;
  }
  return new NullMessageHandler;
}

MessageHandler* FileRewriter::DefaultMessageHandler() {
  return DefaultHtmlParseMessageHandler();
}

FileSystem* FileRewriter::DefaultFileSystem() {
  return new StdioFileSystem;
}

void FileRewriter::SetupCaches(ServerContext* server_context) {
  LRUCache* lru_cache = new LRUCache(gflags_->lru_cache_size_bytes());
  CacheInterface* cache = new ThreadsafeCache(lru_cache,
                                              thread_system()->NewMutex());
  HTTPCache* http_cache = new HTTPCache(cache, timer(), hasher(), statistics());
  server_context->set_http_cache(http_cache);
  server_context->set_metadata_cache(cache);
  server_context->MakePagePropertyCache(
      server_context->CreatePropertyStore(cache));
}

Statistics* FileRewriter::statistics() {
  return &simple_stats_;
}

ServerContext* FileRewriter::NewServerContext() {
  return new FileServerContext(this);
}

ServerContext* FileRewriter::NewDecodingServerContext() {
  ServerContext* sc = NewServerContext();
  InitStubDecodingServerContext(sc);
  return sc;
}

StaticRewriter::StaticRewriter(const ProcessContext& process_context, int* argc,
                               char*** argv)
    : gflags_((*argv)[0], argc, argv),
      file_rewriter_(process_context, &gflags_, true),
      server_context_(NULL) {
  RewriteOptions* options = file_rewriter_.default_options();
  if (!gflags_.SetOptions(&file_rewriter_, options)) {
    exit(1);
  }
  server_context_ = file_rewriter_.CreateServerContext();
}

StaticRewriter::StaticRewriter(const ProcessContext& process_context)
    : file_rewriter_(process_context, &gflags_, false),
      server_context_(file_rewriter_.CreateServerContext()) {
  if (!gflags_.SetOptions(&file_rewriter_,
                          server_context_->global_options())) {
    exit(1);
  }
}

StaticRewriter::~StaticRewriter() {
}

bool StaticRewriter::ParseText(const StringPiece& url,
                               const StringPiece& id,
                               const StringPiece& text,
                               const StringPiece& output_dir,
                               Writer* writer) {
  RewriteDriver* driver = server_context_->NewRewriteDriver(
      RequestContext::NewTestRequestContext(server_context_->thread_system()));

  // For this simple file transformation utility we always want to perform
  // any optimizations we can, so we wait until everything is done rather
  // than using a deadline, the way a server deployment would.
  driver->set_fully_rewrite_on_flush(true);

  file_rewriter_.set_filename_prefix(output_dir);
  driver->SetWriter(writer);
  if (!driver->StartParseId(url, id, kContentTypeHtml)) {
    fprintf(stderr, "StartParseId failed on url %s\n", url.as_string().c_str());
    driver->Cleanup();
    return false;
  }

  // Note that here we are sending the entire buffer into the parser
  // in one chunk, but it's also fine to break up the calls to
  // driver->ParseText as data streams in.  It's up to the caller when
  // to call driver->Flush().  If no calls are ever made to
  // driver->Flush(), then no HTML will be serialized until the end of
  // the document is reached, but rewriters tha work over document
  // structure will have the maximum benefit.
  driver->ParseText(text);
  driver->FinishParse();

  return true;
}

FileSystem* StaticRewriter::file_system() {
  return file_rewriter_.file_system();
}

MessageHandler* StaticRewriter::message_handler() {
  return file_rewriter_.message_handler();
}

}  // namespace net_instaweb
