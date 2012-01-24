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

// Author: nforman@google.com (Naomi Forman)
//
// Implements the convert_meta_tags filter, which creates a
// response header for http-equiv meta tags.

#include "net/instaweb/rewriter/public/meta_tag_filter.h"

#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace {

const char kConvertedMetaTags[] = "converted_meta_tags";

}  // namespace

namespace net_instaweb {

MetaTagFilter::MetaTagFilter(RewriteDriver* rewrite_driver)
    : CommonFilter(rewrite_driver) {
  Statistics* stats = driver_->statistics();
  converted_meta_tag_count_ = stats->GetVariable(kConvertedMetaTags);
}

void MetaTagFilter::Initialize(Statistics* stats) {
  if (stats != NULL) {
    stats->AddVariable(kConvertedMetaTags);
  }
}

MetaTagFilter::~MetaTagFilter() {}

void MetaTagFilter::EndElementImpl(HtmlElement* element) {
  // If headers are null, they got reset due to a flush, so don't
  // try to convert any tags into headers (which were already finalized).
  // Also don't add meta tags to headers if they're inside a noscript tag.
  ResponseHeaders* headers = driver_->response_headers_ptr();
  if (headers != NULL && noscript_element() == NULL) {
    // Figure out if this is a meta tag.  If it is, move to headers.
    if (element->keyword() == HtmlName::kMeta) {
      // We want meta tags with http header equivalents.
      HtmlElement::Attribute* equiv = element->FindAttribute(
          HtmlName::kHttpEquiv);
      HtmlElement::Attribute* value = element->FindAttribute(
          HtmlName::kContent);

      // HTTP-EQUIV case.
      if (equiv != NULL && value != NULL) {
        StringPiece attribute = equiv->value();
        StringPiece content = value->value();
        // It doesn't make sense to have nothing in HttpEquiv, but that means
        // it is in fact lurking out there.
        TrimWhitespace(&attribute);

        if (StringCaseEqual(attribute, HttpAttributes::kContentType)) {
          if (!content.empty()) {
            // Check to see if we have this value already.  If we do,
            // there's no need to add it in again.
            ConstStringStarVector values;
            headers->Lookup(attribute, &values);
            for (int i = 0, n = values.size(); i < n; ++i) {
              StringPiece val(*values[i]);
              if (StringCaseEqual(val, content)) {
                return;
              }
            }
            GoogleString mime_type;
            GoogleString charset;
            if (ParseContentType(content, &mime_type, &charset)) {
              if (!mime_type.empty()) {
                const ContentType* type = MimeTypeToContentType(mime_type);
                if (type == NULL || !type->IsHtmlLike()) {
                  return;
                }
              }
            }
            if (headers->MergeContentType(content)) {
              converted_meta_tag_count_->Add(1);
            }
          }
        }
        return;
      } else {
        // Also handle the <meta charset=''> case.
        HtmlElement::Attribute* charset = element->FindAttribute(
            HtmlName::kCharset);
        if (charset != NULL) {
          GoogleString type = StrCat("; charset=", charset->value());
          if (headers->MergeContentType(type)) {
            converted_meta_tag_count_->Add(1);
          }
        }
      }
    }
  }
}

void MetaTagFilter::Flush() {
  driver_->set_response_headers_ptr(NULL);
}

}  // namespace net_instaweb
