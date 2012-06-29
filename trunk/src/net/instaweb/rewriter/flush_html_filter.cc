/*
 * Copyright 2011 Google Inc.
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

#include <cstddef>  // for NULL

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/rewriter/public/common_filter.h"
#include "net/instaweb/rewriter/public/flush_html_filter.h"
#include "net/instaweb/rewriter/public/resource_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"

namespace {

// Controls the number of resource references that will be scanned before a
// Flush is issued.
//
// TODO(jmarantz): Make these configurable via RewriteOptions.
// TODO(jmarantz): Consider gaps in realtime as justification to induce flushes
// as well.  That might be beyond the scope of this filter.
const int kFlushScoreThreshold = 80;
const int kFlushCssScore = 10;     // 8 CSS files induces a flush.
const int kFlushScriptScore = 10;  // 8 Scripts files induces a flush.
const int kFlushImageScore = 2;    // 40 images induces a flush.

}  // namespace

namespace net_instaweb {

FlushHtmlFilter::FlushHtmlFilter(RewriteDriver* driver)
    : CommonFilter(driver),
      score_(0) {
}

FlushHtmlFilter::~FlushHtmlFilter() {}

void FlushHtmlFilter::StartDocumentImpl() {
  score_ = 0;
}

void FlushHtmlFilter::Flush() {
  score_ = 0;
}

void FlushHtmlFilter::StartElementImpl(HtmlElement* element) {
  ContentType::Category category;
  HtmlElement::Attribute* href = resource_tag_scanner::ScanElement(
      element, driver_, &category);

  if (href == NULL) {
    return;
  }
  switch (category) {
    case ContentType::kStylesheet:
      score_ += kFlushCssScore;
      break;
    case ContentType::kScript:
      score_ += kFlushScriptScore;
      break;
    case ContentType::kImage:
      score_ += kFlushImageScore;
      break;
    default:
      break;
  }
}

void FlushHtmlFilter::EndElementImpl(HtmlElement* element) {
  ContentType::Category category;
  HtmlElement::Attribute* href = resource_tag_scanner::ScanElement(
      element, driver_, &category);

  if (href != NULL && score_ >= kFlushScoreThreshold) {
    score_ = 0;
    driver_->RequestFlush();
  }
}

}  // namespace net_instaweb
