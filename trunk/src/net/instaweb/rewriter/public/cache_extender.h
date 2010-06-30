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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_CACHE_EXTENDER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_CACHE_EXTENDER_H_

#include <vector>

#include "net/instaweb/rewriter/public/resource_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_filter.h"
#include "net/instaweb/util/public/atom.h"
#include <string>

namespace net_instaweb {

class Hasher;
class ResourceManager;
class Timer;
class Variable;

// Rewrites resources to extend their cache lifetime, encoding the
// content hash into the new URL to ensure we do not serve stale
// data.
class CacheExtender : public RewriteFilter {
 public:
  CacheExtender(const char* path_prefix, HtmlParse* html_parse,
                ResourceManager* resource_manager,
                Hasher* hasher, Timer* timer);

  virtual void StartElement(HtmlElement* element);
  virtual bool Fetch(StringPiece resource, Writer* writer,
                     const MetaData& request_header,
                     MetaData* response_headers,
                     UrlAsyncFetcher* fetcher,
                     MessageHandler* message_handler,
                     UrlAsyncFetcher::Callback* callback);

 private:
  Atom s_href_;
  HtmlParse* html_parse_;
  ResourceManager* resource_manager_;
  Hasher* hasher_;
  ResourceTagScanner tag_scanner_;
  Timer* timer_;
  Variable* extension_count_;
  Variable* not_cacheable_count_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_CACHE_EXTENDER_H_
