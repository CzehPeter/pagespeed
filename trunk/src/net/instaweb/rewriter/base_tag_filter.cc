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

#include "public/base_tag_filter.h"

#include "public/add_head_filter.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include <string>

namespace net_instaweb {
BaseTagFilter::BaseTagFilter(HtmlParse* html_parse) {
  s_head_ = html_parse->Intern("head");
  s_base_ = html_parse->Intern("base");
  s_href_ = html_parse->Intern("href");
  s_head_element_ = NULL;
  found_base_tag_ = false;
  html_parse_ = html_parse;
}

void BaseTagFilter::StartDocument() {
  s_head_element_ = NULL;
  found_base_tag_ = false;
}

// In a proxy server, we will want to set a base tag according to the current
// URL being processed.  But we need to add the BaseTagFilter upstream of
// the HtmlWriterFilter, so we'll need to establish it at init time before
// we know a URL.  So in that mode, where we've installed the filter but
// have no specific URL to set the base tag to, then we should avoid
// adding an empty base tag.
void BaseTagFilter::StartElement(HtmlElement* element) {
  if (element->tag() == s_head_) {
    s_head_element_ = element;
  } else if ((s_head_element_ != NULL) && !base_url_.empty()) {
    if (element->tag() == s_base_) {
      // There is already a base tag.  See if it's specified an href.
      for (int i = 0; i < element->attribute_size(); ++i) {
        HtmlElement::Attribute& attribute = element->attribute(i);
        if (attribute.name() == s_href_) {
          // For now let's assume that the explicit base-tag in
          // the source should left alone if it has an href.
          found_base_tag_ = true;
          break;
        }
      }
    }
  }
}

void BaseTagFilter::EndElement(HtmlElement* element) {
  if ((element == s_head_element_) && !base_url_.empty()) {
    s_head_element_ = NULL;
    if (!found_base_tag_) {
      found_base_tag_ = true;
      std::vector<std::string> head_atts;
      HtmlElement* new_element = html_parse_->NewElement(element, s_base_);
      new_element->set_close_style(HtmlElement::IMPLICIT_CLOSE);
      new_element->AddAttribute(s_href_, base_url_.c_str(), "\"");
      html_parse_->InsertElementBeforeCurrent(new_element);
      found_base_tag_ = true;
    }
  }
}

}  // namespace net_instaweb
