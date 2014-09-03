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

// Author: rahulbansal@google.com (Rahul Bansal)

#include <vector>

#include "net/instaweb/rewriter/public/blink_util.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/strip_non_cacheable_filter.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_node.h"

namespace net_instaweb {

StripNonCacheableFilter::StripNonCacheableFilter(
    RewriteDriver* rewrite_driver)
    : rewrite_driver_(rewrite_driver),
      rewrite_options_(rewrite_driver->options()) {
}

StripNonCacheableFilter::~StripNonCacheableFilter() {}

void StripNonCacheableFilter::StartDocument() {
  BlinkUtil::PopulateAttributeToNonCacheableValuesMap(
      rewrite_options_, rewrite_driver_->google_url(),
      &attribute_non_cacheable_values_map_, &panel_number_num_instances_);
}

void StripNonCacheableFilter::StartElement(HtmlElement* element) {
  int panel_number = BlinkUtil::GetPanelNumberForNonCacheableElement(
      attribute_non_cacheable_values_map_, element);
  if (panel_number != -1) {
    GoogleString panel_id = BlinkUtil::GetPanelId(
        panel_number, panel_number_num_instances_[panel_number]);
    panel_number_num_instances_[panel_number]++;
    InsertPanelStub(element, panel_id);
    rewrite_driver_->DeleteNode(element);
  }
}

void StripNonCacheableFilter::InsertPanelStub(HtmlElement* element,
                                              const GoogleString& panel_id) {
  HtmlCommentNode* comment = rewrite_driver_->NewCommentNode(
      element->parent(),
      StrCat(RewriteOptions::kPanelCommentPrefix, " begin ", panel_id));
  rewrite_driver_->InsertNodeBeforeCurrent(comment);
  // Append end stub to json.
  comment = rewrite_driver_->NewCommentNode(
      element->parent(),
      StrCat(RewriteOptions::kPanelCommentPrefix, " end ", panel_id));
  rewrite_driver_->InsertNodeBeforeCurrent(comment);
}

}   // namespace net_instaweb
