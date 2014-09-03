/*
 * Copyright 2012 Google Inc.
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

// Author: guptaa@google.com (Ashish Gupta)

#include "net/instaweb/rewriter/public/compute_visible_text_filter.h"

#include "net/instaweb/rewriter/public/blink_util.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/writer.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/html/html_writer_filter.h"

namespace net_instaweb {

ComputeVisibleTextFilter::ComputeVisibleTextFilter(
    RewriteDriver* rewrite_driver)
    : HtmlWriterFilter(rewrite_driver),
      rewrite_driver_(rewrite_driver),
      writer_(&buffer_) {}

ComputeVisibleTextFilter::~ComputeVisibleTextFilter() {}

void ComputeVisibleTextFilter::StartDocument() {
  // Overridden to suppress emitting the bytes.
  set_writer(&writer_);
}

void ComputeVisibleTextFilter::StartElement(HtmlElement* element) {
  // Overridden to suppress emitting the bytes.
  //
  // We retain meta tags and src attribute of img tags.
  if (element->keyword() == HtmlName::kMeta) {
    HtmlWriterFilter::StartElement(element);
  } else if (element->keyword() == HtmlName::kImg) {
    const char* src_value = element->EscapedAttributeValue(HtmlName::kSrc);
    if (src_value != NULL) {
      writer_.Write(src_value,
                    rewrite_driver_->server_context()->message_handler());
    }
  }
}

void ComputeVisibleTextFilter::EndElement(HtmlElement* element) {
  // Overridden to suppress emitting the bytes.
  if (element->keyword() == HtmlName::kMeta) {
    HtmlWriterFilter::EndElement(element);
  }
}

void ComputeVisibleTextFilter::EndDocument() {
  rewrite_driver_->writer()->Write(
      buffer_, rewrite_driver_->server_context()->message_handler());
  rewrite_driver_->writer()->Write(
      BlinkUtil::kComputeVisibleTextFilterOutputEndMarker,
      rewrite_driver_->server_context()->message_handler());
}

void ComputeVisibleTextFilter::Cdata(HtmlCdataNode* cdata) {
  // Overridden to suppress emitting the bytes.
}

void ComputeVisibleTextFilter::Characters(HtmlCharactersNode* characters) {
  HtmlElement* parent = characters->parent();
  if (parent != NULL && parent->keyword() != HtmlName::kScript &&
      parent->keyword() != HtmlName::kStyle &&
      parent->keyword() != HtmlName::kNoscript) {
    HtmlWriterFilter::Characters(characters);
  }
}

void ComputeVisibleTextFilter::Comment(HtmlCommentNode* comment) {
  // Overridden to suppress emitting the bytes.
}

void ComputeVisibleTextFilter::IEDirective(HtmlIEDirectiveNode* directive) {
  // Overridden to suppress emitting the bytes.
}

void ComputeVisibleTextFilter::Directive(HtmlDirectiveNode* directive) {
  // Overridden to suppress emitting the bytes.
}

}  // namespace net_instaweb
