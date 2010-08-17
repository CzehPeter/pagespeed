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

#ifndef NET_INSTAWEB_HTMLPARSE_HTML_EVENT_H_
#define NET_INSTAWEB_HTMLPARSE_HTML_EVENT_H_

#include <list>
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_filter.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include <string>

namespace net_instaweb {

class HtmlEvent {
 public:
  explicit HtmlEvent(int line_number) : line_number_(line_number) {
  }
  virtual ~HtmlEvent();
  virtual void Run(HtmlFilter* filter) = 0;
  virtual void ToString(std::string* buffer) = 0;
  virtual HtmlElement* GetStartElement() { return NULL; }
  virtual HtmlElement* GetEndElement() { return NULL; }
  virtual HtmlLeafNode* GetLeafNode() { return NULL; }
  virtual HtmlNode* GetNode() { return NULL; }
  virtual HtmlCharactersNode* GetCharactersNode() { return NULL; }
  void DebugPrint();

  int line_number() const { return line_number_; }
 private:
  int line_number_;
};

class HtmlStartDocumentEvent: public HtmlEvent {
 public:
  explicit HtmlStartDocumentEvent(int line_number) : HtmlEvent(line_number) {}
  virtual void Run(HtmlFilter* filter) { filter->StartDocument(); }
  virtual void ToString(std::string* str) { *str += "StartDocument"; }
};

class HtmlEndDocumentEvent: public HtmlEvent {
 public:
  explicit HtmlEndDocumentEvent(int line_number) : HtmlEvent(line_number) {}
  virtual void Run(HtmlFilter* filter) { filter->EndDocument(); }
  virtual void ToString(std::string* str) { *str += "EndDocument"; }
};

class HtmlIEDirectiveEvent: public HtmlEvent {
 public:
  HtmlIEDirectiveEvent(const std::string& directive, int line_number)
      : HtmlEvent(line_number),
        directive_(directive) {
  }
  virtual void Run(HtmlFilter* filter) { filter->IEDirective(directive_); }
  virtual void ToString(std::string* str) {
    *str += "IEDirective ";
    *str += directive_;
  }
 private:
  std::string directive_;
};

class HtmlStartElementEvent: public HtmlEvent {
 public:
  HtmlStartElementEvent(HtmlElement* element, int line_number)
      : HtmlEvent(line_number),
        element_(element) {
  }
  virtual void Run(HtmlFilter* filter) { filter->StartElement(element_); }
  virtual void ToString(std::string* str) {
    *str += "StartElement ";
    *str += element_->tag().c_str();
  }
  virtual HtmlElement* GetStartElement() { return element_; }
  virtual HtmlElement* GetNode() { return element_; }
 private:
  HtmlElement* element_;
};

class HtmlEndElementEvent: public HtmlEvent {
 public:
  HtmlEndElementEvent(HtmlElement* element, int line_number)
      : HtmlEvent(line_number),
        element_(element) {
  }
  virtual void Run(HtmlFilter* filter) { filter->EndElement(element_); }
  virtual void ToString(std::string* str) {
    *str += "EndElement ";
    *str += element_->tag().c_str();
  }
  virtual HtmlElement* GetEndElement() { return element_; }
  virtual HtmlElement* GetNode() { return element_; }
 private:
  HtmlElement* element_;
};

class HtmlLeafNodeEvent: public HtmlEvent {
 public:
  explicit HtmlLeafNodeEvent(int line_number) : HtmlEvent(line_number) { }
  virtual HtmlNode* GetNode() { return GetLeafNode(); }
};

class HtmlCdataEvent: public HtmlLeafNodeEvent {
 public:
  HtmlCdataEvent(HtmlCdataNode* cdata, int line_number)
      : HtmlLeafNodeEvent(line_number),
        cdata_(cdata) {
  }
  virtual void Run(HtmlFilter* filter) { filter->Cdata(cdata_); }
  virtual void ToString(std::string* str) {
    *str += "Cdata ";
    *str += cdata_->contents();
  }
  virtual HtmlLeafNode* GetLeafNode() { return cdata_; }
 private:
  HtmlCdataNode* cdata_;
};

class HtmlCommentEvent: public HtmlLeafNodeEvent {
 public:
  HtmlCommentEvent(HtmlCommentNode* comment, int line_number)
      : HtmlLeafNodeEvent(line_number),
        comment_(comment) {
  }
  virtual void Run(HtmlFilter* filter) { filter->Comment(comment_); }
  virtual void ToString(std::string* str) {
    *str += "Comment ";
    *str += comment_->contents();
  }
  virtual HtmlLeafNode* GetLeafNode() { return comment_; }
 private:
  HtmlCommentNode* comment_;
};

class HtmlCharactersEvent: public HtmlLeafNodeEvent {
 public:
  HtmlCharactersEvent(HtmlCharactersNode* characters, int line_number)
      : HtmlLeafNodeEvent(line_number),
        characters_(characters) {
  }
  virtual void Run(HtmlFilter* filter) { filter->Characters(characters_); }
  virtual void ToString(std::string* str) {
    *str += "Characters ";
    *str += characters_->contents();
  }
  virtual HtmlLeafNode* GetLeafNode() { return characters_; }
  virtual HtmlCharactersNode* GetCharactersNode() { return characters_; }
 private:
  HtmlCharactersNode* characters_;
};

class HtmlDirectiveEvent: public HtmlLeafNodeEvent {
 public:
  HtmlDirectiveEvent(HtmlDirectiveNode* directive, int line_number)
      : HtmlLeafNodeEvent(line_number),
        directive_(directive) {
  }
  virtual void Run(HtmlFilter* filter) { filter->Directive(directive_); }
  virtual void ToString(std::string* str) {
    *str += "Directive: ";
    *str += directive_->contents();
  }
  virtual HtmlLeafNode* GetLeafNode() { return directive_; }
 private:
  HtmlDirectiveNode* directive_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_HTMLPARSE_HTML_EVENT_H_
