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

#include "public/empty_html_filter.h"

namespace net_instaweb {

EmptyHtmlFilter::EmptyHtmlFilter() {
}

EmptyHtmlFilter::~EmptyHtmlFilter() {
}

void EmptyHtmlFilter::StartDocument() {
}

void EmptyHtmlFilter::EndDocument() {
}

void EmptyHtmlFilter::StartElement(HtmlElement* element) {
}

void EmptyHtmlFilter::EndElement(HtmlElement* element) {
}

void EmptyHtmlFilter::Cdata(HtmlCdataNode* cdata) {
}

void EmptyHtmlFilter::Comment(HtmlCommentNode* comment) {
}

void EmptyHtmlFilter::IEDirective(const std::string& directive) {
}

void EmptyHtmlFilter::Characters(HtmlCharactersNode* characters) {
}

void EmptyHtmlFilter::Directive(HtmlDirectiveNode* directive) {
}

void EmptyHtmlFilter::Flush() {
}

}  // namespace net_instaweb
