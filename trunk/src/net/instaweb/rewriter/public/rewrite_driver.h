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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_DRIVER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_DRIVER_H_

#include <map>
#include <vector>
#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/url_async_fetcher.h"

namespace net_instaweb {

class BaseTagFilter;
class FileSystem;
class Hasher;
class HtmlFilter;
class HtmlParse;
class HtmlWriterFilter;
class RewriteFilter;
class Statistics;
class Timer;
class UrlAsyncFetcher;
class UrlFetcher;
class UrlLeftTrimFilter;
class Variable;
class Writer;

class RewriteDriver {
 public:
  // TODO(jmarantz): provide string-constants so that callers, in particular,
  // tests, that want to enable a specific pass, can reference these rather
  // than replicating the string literatls.  Also provide programmatic mechanism
  // to generate simple and detailed help strings for the user enumerating the
  // naems of the filters.
  /*
  static const char kAddHead[];
  static const char kAddBaseTag[];
  static const char kMoveCssToHead[];
  static const char kOutlineCss[];
  static const char kOutlineJavascript[];
  */

  RewriteDriver(MessageHandler* message_handler,
                FileSystem* file_system,
                UrlAsyncFetcher* url_async_fetcher);

  // Need explicit destructors to allow destruction of scoped_ptr-controlled
  // instances without propagating the include files.
  ~RewriteDriver();

  // Adds a resource manager and/or resource_server, enabling the rewriting of
  // resources. This will replace any previous resource managers.
  void SetResourceManager(ResourceManager* resource_manager);

  // Adds the filters, specified by name in enabled_filters.
  void AddFilters(const StringSet& enabled_filters);

  void AddHead() { AddFiltersByCommaSeparatedList("add_head"); }

  // Add filters by comma-separated list
  void AddFiltersByCommaSeparatedList(const StringPiece& filters);

  // Add any HtmlFilter to the HtmlParse chain and take ownership of the filter.
  void AddFilter(HtmlFilter* filter);

  // Add any RewriteFilter and register the id with the RewriteDriver.
  void AddRewriteFilter(RewriteFilter* filter);

  // Controls how HTML output is written.  Be sure to call this last, after
  // all other filters have been established.
  //
  // TODO(jmarantz): fix this in the implementation so that the caller can
  // install filters in any order and the writer will always be last.
  void SetWriter(Writer* writer);

  // Sets the base url for resolving relative URLs in a document.  This
  // will *not* necessarily add a base-tag filter, but will change
  // it if AddBaseTagFilter has been called to use this base.
  //
  // SetBaseUrl may be called multiple times to change the base url.
  //
  // Neither AddBaseTagFilter or SetResourceManager should be called after this.
  void SetBaseUrl(const StringPiece& base);

  void FetchResource(const StringPiece& resource,
                     const MetaData& request_headers,
                     MetaData* response_headers,
                     Writer* writer,
                     MessageHandler* message_handler,
                     UrlAsyncFetcher::Callback* callback);

  HtmlParse* html_parse() { return &html_parse_; }
  FileSystem* file_system() { return resource_manager_->file_system(); }
  void set_async_fetcher(UrlAsyncFetcher* f) { url_async_fetcher_ = f; }

  ResourceManager* resource_manager() const { return resource_manager_; }
  Statistics* statistics() const;

  void set_outline_threshold(size_t t) { outline_threshold_ = t; }

 private:
  typedef std::map<std::string, RewriteFilter*> StringFilterMap;
  StringFilterMap resource_filter_map_;

  // These objects are provided on construction or later, and are
  // owned by the caller.
  HtmlParse html_parse_;
  FileSystem* file_system_;
  UrlAsyncFetcher* url_async_fetcher_;
  ResourceManager* resource_manager_;

  scoped_ptr<HtmlWriterFilter> html_writer_filter_;
  scoped_ptr<BaseTagFilter> base_tag_filter_;
  scoped_ptr<UrlLeftTrimFilter> left_trim_filter_;
  std::vector<HtmlFilter*> filters_;
  Variable* resource_fetches_;
  size_t outline_threshold_;

  DISALLOW_COPY_AND_ASSIGN(RewriteDriver);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_DRIVER_H_
