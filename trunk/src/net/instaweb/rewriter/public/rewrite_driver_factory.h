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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_DRIVER_FACTORY_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_DRIVER_FACTORY_H_

#include "base/scoped_ptr.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class AbstractMutex;
class CacheInterface;
class CacheUrlAsyncFetcher;
class CacheUrlFetcher;
class DelayController;
class FileDriver;
class FileSystem;
class FilenameEncoder;
class Hasher;
class HtmlParse;
class HTTPCache;
class LRUCache;
class MessageHandler;
class ResourceManager;
class RewriteDriver;
class Timer;
class UrlAsyncFetcher;
class UrlFetcher;

// A base RewriteDriverFactory.
class RewriteDriverFactory {
 public:
  RewriteDriverFactory();
  virtual ~RewriteDriverFactory();

  // The RewriteDriveFactory will create objects of default type through the
  // New* method from drived classs.  Here are the objects that can be
  // replaced before creating the RewriteDriver.
  // Note: RewriteDriver takes ownership of these.
  void set_html_parse_message_handler(MessageHandler* message_handler);
  void set_file_system(FileSystem* file_system);
  void set_hasher(Hasher* hasher);
  void set_filename_encoder(FilenameEncoder* filename_encoder);

  void set_combine_css(bool x) { combine_css_ = x; }
  void set_outline_css(bool x) { outline_css_ = x; }
  void set_outline_javascript(bool x) { outline_javascript_ = x; }
  void set_rewrite_images(bool x) { rewrite_images_ = x; }
  void set_extend_cache(bool x) { extend_cache_ = x; }
  void set_add_head(bool x) { add_head_ = x; }
  void set_add_base_tag(bool x) { add_base_tag_ = x; }
  void set_remove_quotes(bool x) { remove_quotes_ = x; }
  void set_force_caching(bool x) { force_caching_ = x; }

  // Setting HTTP caching on causes both the fetcher and the async
  // fecher to return cached versions.
  void set_use_http_cache(bool u) { use_http_cache_ = u; }
  void set_use_threadsafe_cache(bool u) { use_threadsafe_cache_ = u; }

  // You should either call set_url_fetcher, set_url_async_fetcher, or
  // neither.  Do not set both.  If you want to enable real async
  // fetching, because you are serving or want to model live traffic,
  // then turn on http caching, and call url_async_fetcher or
  // set_url_async_fetcher before calling url_fetcher.
  //
  // There is an asymmetry because a synchronous URL fetcher can be
  // created from an asynchronous one only if it's cached.
  //
  // In that scenario,  url_fetcher() will  provide a fetcher that will
  // return a cached entry, or will return
  // false on a fetch, but will queue up an async request to prime the
  // cache for the next query.
  //
  // Before you set an async fetcher, you must turn on http caching.
  void set_url_fetcher(UrlFetcher* url_fetcher);
  void set_url_async_fetcher(UrlAsyncFetcher* url_fetcher);

  // if http_caching is on, these methods return cached fetchers.
  virtual UrlFetcher* url_fetcher();
  virtual UrlAsyncFetcher* url_async_fetcher();

  void set_filename_prefix(StringPiece p) { p.CopyToString(&filename_prefix_); }
  void set_url_prefix(StringPiece p) { p.CopyToString(&url_prefix_); }
  void set_num_shards(int num_shards) { num_shards_ = num_shards; }

  MessageHandler* html_parse_message_handler();
  FileSystem* file_system();
  Hasher* hasher();
  FilenameEncoder* filename_encoder();
  HtmlParse* html_parse();
  Timer* timer();
  HTTPCache* http_cache();


  StringPiece filename_prefix();
  StringPiece url_prefix();
  int num_shards() { return num_shards_; }
  ResourceManager* resource_manager();

  // Generates a mutex.
  virtual AbstractMutex* NewMutex() = 0;

  // Generates a new RewriteDriver.  Each RewriteDriver is not
  // thread-safe, but you can generate a RewriteDriver* for each
  // thread.  The returned drivers are deleted by the factory; they do
  // not need to be deleted by the allocator.
  RewriteDriver* NewRewriteDriver();

 protected:
  // Provide default fetchers.
  virtual UrlFetcher* DefaultUrlFetcher() = 0;
  virtual UrlAsyncFetcher* DefaultAsyncUrlFetcher() = 0;

  // Implementors of RewriteDriverFactory must supply default definitions
  // for each of these methods, although they may be overridden via set_
  // methods above
  virtual MessageHandler* NewHtmlParseMessageHandler() = 0;
  virtual FileSystem* NewFileSystem() = 0;
  virtual Hasher* NewHasher() = 0;
  virtual HtmlParse* NewHtmlParse() = 0;
  virtual Timer* NewTimer() = 0;
  virtual CacheInterface* NewCacheInterface() = 0;

  // Implementors of RewriteDriverFactory must supply two mutexes.
  virtual AbstractMutex* cache_mutex() = 0;
  virtual AbstractMutex* rewrite_drivers_mutex() = 0;

 private:
  scoped_ptr<FileSystem> file_system_;
  scoped_ptr<UrlFetcher> url_fetcher_;
  scoped_ptr<UrlAsyncFetcher> url_async_fetcher_;
  scoped_ptr<Hasher> hasher_;
  scoped_ptr<FilenameEncoder> filename_encoder_;
  scoped_ptr<Timer> timer_;
  HtmlParse* html_parse_;

  std::string filename_prefix_;
  std::string url_prefix_;
  int num_shards_;
  bool use_http_cache_;
  bool use_threadsafe_cache_;
  bool combine_css_;
  bool outline_css_;
  bool outline_javascript_;
  bool rewrite_images_;
  bool extend_cache_;
  bool add_head_;
  bool add_base_tag_;
  bool remove_quotes_;
  bool force_caching_;

  scoped_ptr<ResourceManager> resource_manager_;

  std::vector<RewriteDriver*> rewrite_drivers_;

  // Caching support
  scoped_ptr<MessageHandler> html_parse_message_handler_;
  scoped_ptr<HTTPCache> http_cache_;
  scoped_ptr<CacheInterface> threadsafe_cache_;
  scoped_ptr<CacheUrlFetcher> cache_fetcher_;
  scoped_ptr<CacheUrlAsyncFetcher> cache_async_fetcher_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_DRIVER_FACTORY_H_
