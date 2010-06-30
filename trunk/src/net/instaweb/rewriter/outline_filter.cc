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

// Author: sligocki@google.com (Shawn Ligocki)

#include "net/instaweb/rewriter/public/outline_filter.h"

#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/util/public/content_type.h"
#include "net/instaweb/util/public/simple_meta_data.h"
#include <string>
#include "net/instaweb/util/public/string_writer.h"

namespace net_instaweb {

const char kTextCss[] = "text/css";
const char kTextJavascript[] = "text/javascript";
const char kStylesheet[] = "stylesheet";

OutlineFilter::OutlineFilter(HtmlParse* html_parse,
                             ResourceManager* resource_manager,
                             bool outline_styles,
                             bool outline_scripts)
    : inline_element_(NULL),
      html_parse_(html_parse),
      resource_manager_(resource_manager),
      outline_styles_(outline_styles),
      outline_scripts_(outline_scripts) {
  s_link_ = html_parse_->Intern("link");
  s_script_ = html_parse_->Intern("script");
  s_style_ = html_parse_->Intern("style");
  s_rel_ = html_parse_->Intern("rel");
  s_href_ = html_parse_->Intern("href");
  s_src_ = html_parse_->Intern("src");
  s_type_ = html_parse_->Intern("type");
}

void OutlineFilter::StartDocument() {
  inline_element_ = NULL;
  buffer_.clear();
}

void OutlineFilter::StartElement(HtmlElement* element) {
  // No tags allowed inside style or script element.
  if (inline_element_ != NULL) {
    // TODO(sligocki): Add negative unit tests to hit these errors.
    html_parse_->ErrorHere("Tag '%s' found inside style/script.",
                           element->tag().c_str());
    inline_element_ = NULL;  // Don't outline what we don't understand.
    buffer_.clear();
  }
  if (outline_styles_ && element->tag() == s_style_) {
    inline_element_ = element;
    buffer_.clear();

  } else if (outline_scripts_ && element->tag() == s_script_) {
    inline_element_ = element;
    buffer_.clear();
    // script elements which already have a src should not be outlined.
    for (int i = 0; i < element->attribute_size(); ++i) {
      if (element->attribute(i).name() == s_src_) {
        inline_element_ = NULL;
      }
    }
  }
}

void OutlineFilter::EndElement(HtmlElement* element) {
  if (inline_element_ != NULL) {
    if (element != inline_element_) {
      // No other tags allowed inside style or script element.
      html_parse_->ErrorHere("Tag '%s' found inside style/script.",
                             element->tag().c_str());

    } else if (inline_element_->tag() == s_style_) {
      OutlineStyle(inline_element_, buffer_);

    } else if (inline_element_->tag() == s_script_) {
      OutlineScript(inline_element_, buffer_);

    } else {
      html_parse_->ErrorHere("OutlineFilter::inline_element_ "
                             "Expected: 'style' or 'script', Actual: '%s'",
                             inline_element_->tag().c_str());
    }
    inline_element_ = NULL;
    buffer_.clear();
  }
}

void OutlineFilter::Flush() {
  // If we were flushed in a style/script element, we cannot outline it.
  inline_element_ = NULL;
  buffer_.clear();
}

void OutlineFilter::Characters(HtmlCharactersNode* characters) {
  if (inline_element_ != NULL) {
    buffer_ += characters->contents();
  }
}

void OutlineFilter::Comment(HtmlCommentNode* comment) {
  if (inline_element_ != NULL) {
    html_parse_->ErrorHere("Comment found inside style/script.");
    inline_element_ = NULL;  // Don't outline what we don't understand.
    buffer_.clear();
  }
}

void OutlineFilter::Cdata(HtmlCdataNode* cdata) {
  if (inline_element_ != NULL) {
    html_parse_->ErrorHere("CDATA found inside style/script.");
    inline_element_ = NULL;  // Don't outline what we don't understand.
    buffer_.clear();
  }
}

void OutlineFilter::IEDirective(const std::string& directive) {
  if (inline_element_ != NULL) {
    html_parse_->ErrorHere("IE Directive found inside style/script.");
    inline_element_ = NULL;  // Don't outline what we don't understand.
    buffer_.clear();
  }
}

// Try to write content and possibly header to resource.
bool OutlineFilter::WriteResource(const std::string& content,
                                  OutputResource* resource,
                                  MessageHandler* handler) {
  bool write_worked = resource->StartWrite(handler);
  write_worked &= resource->WriteChunk(content, handler);
  write_worked &= resource->EndWrite(handler);
  return write_worked;
}

// Create file with style content and remove that element from DOM.
void OutlineFilter::OutlineStyle(HtmlElement* style_element,
                                 const std::string& content) {
  if (html_parse_->IsRewritable(style_element)) {
    // Create style file from content.
    const char* type = style_element->AttributeValue(s_type_);
    // We only deal with CSS styles.  If no type specified, CSS is assumed.
    // TODO(sligocki): Is this assumption appropriate?
    if (type == NULL || strcmp(type, kTextCss) == 0) {
      OutputResource* resource =
          resource_manager_->GenerateOutputResource("of", kContentTypeCss);
      MessageHandler* handler = html_parse_->message_handler();
      if (WriteResource(content, resource, handler)) {
        HtmlElement* link_element = html_parse_->NewElement(
            style_element->parent(), s_link_);
        link_element->AddAttribute(s_rel_, kStylesheet, "'");
        link_element->AddAttribute(s_href_, resource->url(), "'");
        // Add all style atrributes to link.
        for (int i = 0; i < style_element->attribute_size(); ++i) {
          const HtmlElement::Attribute& attr = style_element->attribute(i);
          link_element->AddAttribute(attr.name(), attr.value(), attr.quote());
        }
        // Add link to DOM.
        html_parse_->InsertElementAfterElement(style_element, link_element);
        // Remove style element from DOM.
        if (!html_parse_->DeleteElement(style_element)) {
          html_parse_->FatalErrorHere("Failed to delete inline sytle element");
        }
      } else {
        html_parse_->ErrorHere("Failed to write outlined style resource.");
      }
    } else {
      std::string element_string;
      style_element->ToString(&element_string);
      html_parse_->InfoHere("Cannot outline non-css stylesheet %s",
                            element_string.c_str());
    }
  }
}

// Create file with script content and remove that element from DOM.
// TODO(sligocki): Combine similar code from OutlineStyle.
void OutlineFilter::OutlineScript(HtmlElement* inline_element,
                                  const std::string& content) {
  if (html_parse_->IsRewritable(inline_element)) {
    // Create script file from content.
    const char* type = inline_element->AttributeValue(s_type_);
    // We only deal with javascript styles. If no type specified, JS is assumed.
    // TODO(sligocki): Is this assumption appropriate?
    if (type == NULL || strcmp(type, kTextJavascript) == 0) {
      OutputResource* resource = resource_manager_->GenerateOutputResource(
          "of", kContentTypeJavascript);
      MessageHandler* handler = html_parse_->message_handler();
      if (WriteResource(content, resource, handler)) {
        HtmlElement* outline_element = html_parse_->NewElement(
            inline_element->parent(), s_script_);
        outline_element->AddAttribute(s_src_, resource->url(), "'");
        // Add all atrributes from old script element to new script src element.
        for (int i = 0; i < inline_element->attribute_size(); ++i) {
          const HtmlElement::Attribute& attr = inline_element->attribute(i);
          outline_element->AddAttribute(attr.name(), attr.value(),
                                        attr.quote());
        }
        // Add <script src=...> element to DOM.
        html_parse_->InsertElementBeforeElement(inline_element,
                                                outline_element);
        // Remove original script element from DOM.
        if (!html_parse_->DeleteElement(inline_element)) {
          html_parse_->FatalErrorHere("Failed to delete inline script element");
        }
      } else {
        html_parse_->ErrorHere("Failed to write outlined script resource.");
      }
    } else {
      std::string element_string;
      inline_element->ToString(&element_string);
      html_parse_->InfoHere("Cannot outline non-javascript script %s",
                            element_string.c_str());
    }
  }
}

}  // namespace net_instaweb
