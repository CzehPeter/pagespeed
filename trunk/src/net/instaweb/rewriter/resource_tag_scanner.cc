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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/rewriter/public/resource_tag_scanner.h"

#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/rewriter/public/css_tag_scanner.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

HtmlElement::Attribute* ResourceTagScanner::ScanElement(HtmlElement* element)
    const {
  HtmlName::Keyword keyword = element->keyword();
  HtmlElement::Attribute* attr = NULL;
  switch (keyword) {
    case HtmlName::kLink: {
      // See http://www.whatwg.org/specs/web-apps/current-work/multipage/
      // links.html#linkTypes
      HtmlElement::Attribute* rel_attr = element->FindAttribute(HtmlName::kRel);
      if ((rel_attr != NULL) &&
          StringCaseEqual(rel_attr->value(), CssTagScanner::kStylesheet)) {
        attr = element->FindAttribute(HtmlName::kHref);
      }
      break;
    }
    case HtmlName::kScript:
    case HtmlName::kImg:
      attr = element->FindAttribute(HtmlName::kSrc);
      break;
    case HtmlName::kA:
      if (find_a_tags_) {
        attr = element->FindAttribute(HtmlName::kHref);
      }
      break;
    case HtmlName::kForm:
      if (find_form_tags_) {
        attr = element->FindAttribute(HtmlName::kAction);
      }
      break;
    default:
      break;
  }
  return attr;
}

}  // namespace net_instaweb
