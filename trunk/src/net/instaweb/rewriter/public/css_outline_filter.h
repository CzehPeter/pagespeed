/*
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

// Author: sligocki@google.com (Shawn Ligocki)

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_CSS_OUTLINE_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_CSS_OUTLINE_FILTER_H_

#include <cstddef>

#include "net/instaweb/rewriter/public/common_filter.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {
class HtmlCdataNode;
class HtmlCharactersNode;
class HtmlCommentNode;
class HtmlDirectiveNode;
class HtmlElement;
class HtmlIEDirectiveNode;
class MessageHandler;
class OutputResource;
class RewriteDriver;

// Filter to take explicit <style> and <script> tags and outline them to files.
class CssOutlineFilter : public CommonFilter {
 public:
  explicit CssOutlineFilter(RewriteDriver* driver);
  static const char kFilterId[];

  virtual void StartDocumentImpl();

  virtual void StartElementImpl(HtmlElement* element);
  virtual void EndElementImpl(HtmlElement* element);

  virtual void Flush();

  // HTML Events we expect to be in <style> elements.
  virtual void Characters(HtmlCharactersNode* characters);

  // HTML Events we do not expect to be in <style> and <script> elements.
  virtual void Comment(HtmlCommentNode* comment);
  virtual void Cdata(HtmlCdataNode* cdata);
  virtual void IEDirective(HtmlIEDirectiveNode* directive);

  // Ignored HTML Events.
  virtual void EndDocument() {}
  virtual void Directive(HtmlDirectiveNode* directive) {}

  virtual const char* Name() const { return "OutlineCss"; }

 private:
  bool WriteResource(const StringPiece& content, OutputResource* resource,
                     MessageHandler* handler);
  void OutlineStyle(HtmlElement* element, const GoogleString& content);

  // The style or script element we are in (if it hasn't been flushed).
  // If we are not in a script or style element, inline_element_ == NULL.
  HtmlElement* inline_element_;
  // Temporarily buffers the content between open and close of inline_element_.
  GoogleString buffer_;
  size_t size_threshold_bytes_;
  // HTML strings interned into a symbol table.

  DISALLOW_COPY_AND_ASSIGN(CssOutlineFilter);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_CSS_OUTLINE_FILTER_H_
