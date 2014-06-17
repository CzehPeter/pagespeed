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

#include "net/instaweb/rewriter/public/css_outline_filter.h"

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string_writer.h"

namespace net_instaweb {

class MessageHandler;

const char kStylesheet[] = "stylesheet";

const char CssOutlineFilter::kFilterId[] = "co";

CssOutlineFilter::CssOutlineFilter(RewriteDriver* driver)
    : CommonFilter(driver),
      inline_element_(NULL),
      inline_chars_(NULL),
      size_threshold_bytes_(driver->options()->css_outline_min_bytes()) {
}

CssOutlineFilter::~CssOutlineFilter() {}

void CssOutlineFilter::StartDocumentImpl() {
  inline_element_ = NULL;
  inline_chars_ = NULL;
}

void CssOutlineFilter::StartElementImpl(HtmlElement* element) {
  // No tags allowed inside style element.
  if (inline_element_ != NULL) {
    // TODO(sligocki): Add negative unit tests to hit these errors.
    driver_->ErrorHere("Tag '%s' found inside style.",
                       CEscape(element->name_str()).c_str());
    inline_element_ = NULL;  // Don't outline what we don't understand.
    inline_chars_ = NULL;
  }
  if (element->keyword() == HtmlName::kStyle) {
    inline_element_ = element;
    inline_chars_ = NULL;
  }
}

void CssOutlineFilter::EndElementImpl(HtmlElement* element) {
  if (inline_element_ != NULL) {
    CHECK(element == inline_element_);
    if (inline_chars_ != NULL &&
        inline_chars_->contents().size() >= size_threshold_bytes_) {
      OutlineStyle(inline_element_, inline_chars_->contents());
    }
    inline_element_ = NULL;
    inline_chars_ = NULL;
  }
}

void CssOutlineFilter::Flush() {
  // If we were flushed in a style element, we cannot outline it.
  inline_element_ = NULL;
  inline_chars_ = NULL;
}

void CssOutlineFilter::Characters(HtmlCharactersNode* characters) {
  if (inline_element_ != NULL) {
    CHECK(inline_chars_ == NULL) << "Multiple character blocks in style.";
    inline_chars_ = characters;
  }
}

// Try to write content and possibly header to resource.
bool CssOutlineFilter::WriteResource(const StringPiece& content,
                                     OutputResource* resource,
                                     MessageHandler* handler) {
  // We don't provide charset here since in general we can just inherit
  // from the page.
  // TODO(morlovich) check for proper behavior in case of embedded BOM.
  // TODO(matterbury) but AFAICT you cannot have a BOM in a <style> tag.
  return driver_->Write(
      ResourceVector(), content, &kContentTypeCss, StringPiece(), resource);
}

// Create file with style content and remove that element from DOM.
void CssOutlineFilter::OutlineStyle(HtmlElement* style_element,
                                    const GoogleString& content_str) {
  StringPiece content(content_str);
  if (driver_->IsRewritable(style_element)) {
    // Create style file from content.
    const char* type = style_element->AttributeValue(HtmlName::kType);
    // We only deal with CSS styles.  If no type specified, CSS is assumed.
    // See http://www.w3.org/TR/html5/semantics.html#the-style-element
    if (type == NULL || strcmp(type, kContentTypeCss.mime_type()) == 0) {
      MessageHandler* handler = driver_->message_handler();
      // Create outline resource at the document location,
      // not base URL location.
      OutputResourcePtr output_resource(
          driver_->CreateOutputResourceWithUnmappedUrl(
              driver_->google_url(), kFilterId, "_", kOutlinedResource));

      if (output_resource.get() != NULL) {
        // Rewrite URLs in content.
        GoogleString transformed_content;
        StringWriter writer(&transformed_content);
        bool content_valid = true;
        switch (driver_->ResolveCssUrls(base_url(),
                                        output_resource->resolved_base(),
                                        content,
                                        &writer, handler)) {
          case RewriteDriver::kNoResolutionNeeded:
            break;
          case RewriteDriver::kWriteFailed:
            content_valid = false;
            break;
          case RewriteDriver::kSuccess:
            content = transformed_content;
            break;
        }
        if (content_valid &&
            WriteResource(content, output_resource.get(), handler)) {
          HtmlElement* link_element = driver_->NewElement(
              style_element->parent(), HtmlName::kLink);
          driver_->AddAttribute(link_element, HtmlName::kRel, kStylesheet);
          driver_->AddAttribute(link_element, HtmlName::kHref,
                                output_resource->url());
          // Add all style attributes to link.
          const HtmlElement::AttributeList& attrs = style_element->attributes();
          for (HtmlElement::AttributeConstIterator i(attrs.begin());
               i != attrs.end(); ++i) {
            const HtmlElement::Attribute& attr = *i;
            link_element->AddAttribute(attr);
          }
          // Add link to DOM.
          driver_->InsertNodeAfterNode(style_element, link_element);
          // Remove style element from DOM.
          if (!driver_->DeleteNode(style_element)) {
            driver_->FatalErrorHere("Failed to delete inline style element");
          }
        }
      }
    } else {
      GoogleString element_string;
      style_element->ToString(&element_string);
      driver_->InfoHere("Cannot outline non-css stylesheet %s",
                        element_string.c_str());
    }
  }
}

}  // namespace net_instaweb
