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

#include "net/instaweb/rewriter/public/common_filter.h"

#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_parse.h"

namespace net_instaweb {

CommonFilter::CommonFilter(RewriteDriver* driver)
    : driver_(driver),
      resource_manager_(driver->resource_manager()),
      rewrite_options_(driver->options()),
      seen_base_(false) {
}

CommonFilter::~CommonFilter() {}

void CommonFilter::StartDocument() {
  // Base URL starts as document URL.
  noscript_element_ = NULL;
  // Reset whether or not we've seen the base tag yet, because we're starting
  // back at the top of the document.
  seen_base_ = false;
  // Run the actual filter's StartDocumentImpl.
  StartDocumentImpl();
}

void CommonFilter::StartElement(HtmlElement* element) {
  if (element->keyword() == HtmlName::kNoscript) {
    if (noscript_element_ == NULL) {
      noscript_element_ = element;  // Record top-level <noscript>
    }
  }
  // If this is a base tag with an href attribute, then we've seen the base, and
  // any url references after this point are relative to that base.
  if (element->keyword() == HtmlName::kBase &&
      element->FindAttribute(HtmlName::kHref) != NULL) {
    seen_base_ = true;
  }

  // Run actual filter's StartElementImpl
  StartElementImpl(element);
}

void CommonFilter::EndElement(HtmlElement* element) {
  if (element == noscript_element_) {
    noscript_element_ = NULL;  // We are exitting the top-level <noscript>
  }

  // Run actual filter's EndElementImpl
  EndElementImpl(element);
}

// Returns whether or not we can resolve against the base tag.  References
// that occur before the base tag can not be resolved against it.
// Different browsers deal with such refs differently, but we shouldn't
// change their behavior.
bool CommonFilter::BaseUrlIsValid() const {
  // If there are no href or src attributes before the base, it's
  // always valid.
  if (!driver_->refs_before_base()) {
    return true;
  }
  // If the filter has already seen the base url, then it's now valid
  // even if there were urls before it.
  return seen_base_;
}

// TODO(jmarantz): Remove these methods -- they used to serve an
// important contextual purpose but now that the resource creation
// methods were moved to RewriteDriver they won't add much value.
Resource* CommonFilter::CreateInputResource(const StringPiece& url) {
  return driver_->CreateInputResource(base_url(), url);
}

Resource* CommonFilter::CreateInputResourceAndReadIfCached(
    const StringPiece& url) {
  return driver_->CreateInputResourceAndReadIfCached(base_url(), url);
}

void CommonFilter::ScanStartDocument() {
}

void CommonFilter::ScanEndDocument() {
}

void CommonFilter::ScanStartElement(HtmlElement* element) {
}

void CommonFilter::ScanEndElement(HtmlElement* element) {
}

void CommonFilter::ScanComment(HtmlCommentNode* comment) {
}

void CommonFilter::ScanIEDirective(HtmlIEDirectiveNode* directive) {
}

void CommonFilter::ScanCharacters(HtmlCharactersNode* characters) {
}

void CommonFilter::ScanDirective(HtmlDirectiveNode* directive) {
}

void CommonFilter::ScanCdata(HtmlCdataNode* cdata) {
}

}  // namespace net_instaweb
