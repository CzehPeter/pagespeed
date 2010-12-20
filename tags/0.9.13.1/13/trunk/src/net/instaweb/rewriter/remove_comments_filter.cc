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

// Author: mdsteele@google.com (Matthew D. Steele)

#include "net/instaweb/rewriter/public/remove_comments_filter.h"

#include "net/instaweb/htmlparse/public/html_parse.h"

namespace net_instaweb {

RemoveCommentsFilter::RemoveCommentsFilter(HtmlParse* html_parse)
    : html_parse_(html_parse) {}

void RemoveCommentsFilter::Comment(HtmlCommentNode* comment) {
  html_parse_->DeleteElement(comment);
}

}  // namespace net_instaweb
