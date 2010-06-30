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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_ADD_HEAD_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_ADD_HEAD_FILTER_H_

#include "net/instaweb/htmlparse/public/empty_html_filter.h"
#include "net/instaweb/util/public/atom.h"

namespace net_instaweb {

// Adds a 'head' element before the 'body', if none was found
// during parsing.  This enables downstream filters to assume
// that there will be a head.
class AddHeadFilter : public EmptyHtmlFilter {
 public:
  explicit AddHeadFilter(HtmlParse* parser);

  virtual void StartDocument();
  virtual void StartElement(HtmlElement* element);
  virtual void EndDocument();

 private:
  bool found_head_;
  Atom s_head_;
  Atom s_body_;
  HtmlParse* html_parse_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_ADD_HEAD_FILTER_H_
