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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_CSS_COMBINE_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_CSS_COMBINE_FILTER_H_

#include <vector>

#include "net/instaweb/rewriter/public/css_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_filter.h"
#include "net/instaweb/util/public/atom.h"
#include <string>

namespace net_instaweb {

class InputResource;
class OutputResource;
class ResourceManager;
class Variable;

class CssCombineFilter : public RewriteFilter {
 public:
  CssCombineFilter(const char* path_prefix, HtmlParse* html_parse,
                  ResourceManager* resource_manager);

  virtual void StartDocument();
  virtual void StartElement(HtmlElement* element);
  virtual void EndElement(HtmlElement* element);
  virtual void Flush();
  virtual void IEDirective(const std::string& directive);
  virtual bool Fetch(StringPiece url_safe_id,
                     Writer* writer,
                     const MetaData& request_header,
                     MetaData* response_headers,
                     UrlAsyncFetcher* fetcher,
                     MessageHandler* message_handler,
                     UrlAsyncFetcher::Callback* callback);

 private:
  typedef std::vector<InputResource*> InputResourceVector;

  void EmitCombinations(HtmlElement* head);
  bool WriteWithAbsoluteUrls(const StringPiece& contents,
                             OutputResource* combination,
                             const std::string& base_url,
                             MessageHandler* handler);
  bool WriteCombination(const InputResourceVector& combine_resources,
                        OutputResource* combination,
                        MessageHandler* handler);

  Atom s_head_;
  Atom s_type_;
  Atom s_link_;
  Atom s_href_;
  Atom s_rel_;
  Atom s_media_;

  std::vector<HtmlElement*> css_elements_;
  HtmlParse* html_parse_;
  HtmlElement* head_element_;  // Pointer to head element for future use
  ResourceManager* resource_manager_;
  CssTagScanner css_filter_;
  Variable* counter_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_CSS_COMBINE_FILTER_H_
