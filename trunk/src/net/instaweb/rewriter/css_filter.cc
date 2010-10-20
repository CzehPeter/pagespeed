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

#include "net/instaweb/rewriter/public/css_filter.h"

#include "base/at_exit.h"

#include "base/scoped_ptr.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/rewriter/public/css_minify.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/util/public/content_type.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/url_escaper.h"
#include "net/instaweb/util/public/writer.h"
#include "webutil/css/parser.h"

namespace {

base::AtExitManager* at_exit_manager = NULL;

}

namespace net_instaweb {

namespace {

const char kStylesheet[] = "stylesheet";

}  // namespace

CssFilter::CssFilter(RewriteDriver* driver, const StringPiece& path_prefix)
    : RewriteFilter(driver, path_prefix),
      html_parse_(driver->html_parse()),
      resource_manager_(driver->resource_manager()),
      in_style_element_(false),
      s_style_(html_parse_->Intern("style")),
      s_link_(html_parse_->Intern("link")),
      s_rel_(html_parse_->Intern("rel")),
      s_href_(html_parse_->Intern("href")) {
}

void CssFilter::StartDocument() {
  in_style_element_ = false;
}

void CssFilter::StartElement(HtmlElement* element) {
  // HtmlParse should not pass us elements inside a style element.
  CHECK(!in_style_element_);
  if (element->tag() == s_style_) {
    in_style_element_ = true;
    style_element_ = element;
    style_char_node_ = NULL;
  }
  // We deal with <link> elements in EndElement.
}

void CssFilter::Characters(HtmlCharactersNode* characters_node) {
  if (in_style_element_) {
    if (style_char_node_ == NULL) {
      style_char_node_ = characters_node;
    } else {
      html_parse_->ErrorHere("Multiple character nodes in style.");
      in_style_element_ = false;
    }
  }
}

void CssFilter::EndElement(HtmlElement* element) {
  // Rewrite an inline style.
  if (in_style_element_) {
    CHECK(style_element_ == element);  // HtmlParse should not pass unmatching.

    if (html_parse_->IsRewritable(element) && style_char_node_ != NULL) {
      CHECK(element == style_char_node_->parent());  // Sanity check.
      std::string new_content;
      if (RewriteCssText(style_char_node_->contents(), &new_content,
                         html_parse_->message_handler())) {
        // Note: Copy of new_content here.
        HtmlCharactersNode* new_style_char_node =
            html_parse_->NewCharactersNode(element, new_content);
        html_parse_->ReplaceNode(style_char_node_, new_style_char_node);
      }
    }
    in_style_element_ = false;

  // Rewrite an external style.
  } else if (element->tag() == s_link_ && html_parse_->IsRewritable(element)) {
    StringPiece relation(element->AttributeValue(s_rel_));
    if (relation == kStylesheet) {
      HtmlElement::Attribute* element_href = element->FindAttribute(s_href_);
      if (element_href != NULL) {  // If it has a href= attribute
        StringPiece old_url(element_href->value());
        std::string new_url;
        if (RewriteExternalCss(old_url, &new_url)) {
          element_href->SetValue(new_url);  // Update the href= attribute.
        }
      } else {
        html_parse_->ErrorHere("Link element with no href.");
      }
    }
  }
}

bool CssFilter::RewriteCssText(const StringPiece& in_text,
                               std::string* out_text,
                               MessageHandler* handler) {
  // Load stylesheet w/o expanding background attributes.
  // TODO(sligocki): Figure out how we know if this failed.
  Css::Stylesheet* stylesheet = Css::Parser(in_text).ParseRawStylesheet();

  // TODO(sligocki): Edit stylesheet.

  // Re-serialize stylesheet.
  StringWriter writer(out_text);
  CssMinify::Stylesheet(*stylesheet, &writer, handler);

  // TODO(sligocki): Do we want to save the AST somewhere? Deleting for now.
  delete stylesheet;
  return true;
}


// Combine all 'original_stylesheets' (and all their sub stylescripts) into a
// single returned stylesheet which has no @imports or returns NULL if we fail
// to load some sub-resources.
//
// Note: we must cannibalize input stylesheets or we will have ownership
// problems or a lot of deep-copying.
Css::Stylesheet* CssFilter::CombineStylesheets(
    std::vector<Css::Stylesheet*>* original_stylesheets) {
  // Load all sub-stylesheets to assure that we can do the combination.
  std::vector<Css::Stylesheet*> stylesheets;
  std::vector<Css::Stylesheet*>::const_iterator iter;
  for (iter = original_stylesheets->begin();
       iter < original_stylesheets->end(); ++iter) {
    Css::Stylesheet* stylesheet = *iter;
    if (!LoadAllSubStylesheets(stylesheet, &stylesheets)) {
      return NULL;
    }
  }

  // Once all sub-stylesheets are loaded in memory, combine them.
  Css::Stylesheet* combination = new Css::Stylesheet;
  // TODO(sligocki): combination->rulesets().reserve(...);
  for (std::vector<Css::Stylesheet*>::const_iterator iter = stylesheets.begin();
       iter < stylesheets.end(); ++iter) {
    Css::Stylesheet* stylesheet = *iter;
    // Append all rulesets from 'stylesheet' to 'combination' ...
    combination->mutable_rulesets().insert(
        combination->mutable_rulesets().end(),
        stylesheet->rulesets().begin(),
        stylesheet->rulesets().end());
    // ... and then clear rules from 'stylesheet' to avoid double ownership.
    stylesheet->mutable_rulesets().clear();
  }
  return combination;
}

// Collect a list of all stylesheets @imported by base_stylesheet directly or
// indirectly in the order that they will be dealt with by a CSS parser and
// append them to vector 'all_stylesheets'.
bool CssFilter::LoadAllSubStylesheets(
    Css::Stylesheet* base_stylesheet,
    std::vector<Css::Stylesheet*>* all_stylesheets) {
  const Css::Imports& imports = base_stylesheet->imports();
  for (Css::Imports::const_iterator iter = imports.begin();
       iter < imports.end(); ++iter) {
    Css::Import* import = *iter;
    StringPiece url(import->link.utf8_data(), import->link.utf8_length());

    // Fetch external stylesheet from url ...
    Css::Stylesheet* sub_stylesheet = LoadStylesheet(url);
    if (sub_stylesheet == NULL) {
      html_parse_->ErrorHere("Failed to load sub-resource %s",
                             url.as_string().c_str());
      return false;
    }

    // ... and recursively add all its sub-stylesheets (and it) to vector.
    if (!LoadAllSubStylesheets(sub_stylesheet, all_stylesheets)) {
      return false;
    }
  }
  // Add base stylesheet after all imports have been added.
  all_stylesheets->push_back(base_stylesheet);
  return true;
}


Resource* CssFilter::GetInputResource(const StringPiece& url) {
  // TODO(sligocki): Use base_url() not document->url().
  return resource_manager_->CreateInputResource(html_parse_->url(), url,
                                                html_parse_->message_handler());
}

// Create an output resource based on url of input resource.
OutputResource* CssFilter::CreateCssOutputResource(const StringPiece& in_url) {
  std::string name;  // UrlEscaper encoded url.
  resource_manager_->url_escaper()->EncodeToUrlSegment(in_url, &name);
  return resource_manager_->CreateNamedOutputResource(
      filter_prefix_, name, &kContentTypeCss, html_parse_->message_handler());
}

// Read an external CSS file, rewrite it and write a new external CSS file.
bool CssFilter::RewriteExternalCss(const StringPiece& in_url,
                                   std::string* out_url) {
  OutputResource* output_resource = CreateCssOutputResource(in_url);
  bool ret = false;
  if (output_resource != NULL) {
    ret = RewriteExternalCssToResource(in_url, output_resource);
    if (ret) {
      *out_url = output_resource->url();
    }
  }
  return ret;
}

bool CssFilter::RewriteExternalCssToResource(const StringPiece& in_url,
                                             OutputResource* output_resource) {
  // If this OutputResource has not already been created, create it.
  if (!output_resource->IsWritten()) {
    // Load input stylesheet.
    scoped_ptr<Resource> input_resource(GetInputResource(in_url));
    MessageHandler* handler = html_parse_->message_handler();
    if (input_resource == NULL ||
        !resource_manager_->ReadIfCached(input_resource.get(), handler) ||
        !input_resource->ContentsValid()) {
      // TODO(sligocki): Should these really be HtmlParse errors?
      html_parse_->ErrorHere("Failed to load resource %s",
                             in_url.as_string().c_str());
      return false;
    }

    // Rewrite stylesheet.
    StringPiece in_contents = input_resource->contents();
    std::string out_contents;
    if (!RewriteCssText(in_contents, &out_contents,
                        html_parse_->message_handler())) {
      html_parse_->ErrorHere("Failed to rewrite resource %s",
                             in_url.as_string().c_str());
      return false;
    }

    // Write new stylesheet.
    // TODO(sligocki): Set expire time.
    if (!resource_manager_->Write(HttpStatus::kOK, out_contents,
                                  output_resource, -1, handler)) {
      return false;
    }
  }

  return output_resource->IsWritten();
}

bool CssFilter::Fetch(OutputResource* output_resource,
                      Writer* writer,
                      const MetaData& request_header,
                      MetaData* response_headers,
                      UrlAsyncFetcher* fetcher,
                      MessageHandler* message_handler,
                      UrlAsyncFetcher::Callback* callback) {
  // TODO(sligocki): We do not use writer, *_headers or fetcher ... should we?
  // It looks like nobody is using the fetcher, I'll let someone else get this
  // right first.
  bool ret = false;
  std::string in_url;
  if (resource_manager_->url_escaper()->DecodeFromUrlSegment(
          output_resource->name(), &in_url)) {
    // TODO(sligocki): If this doesn't work, we need to wait for it to finish
    // fetching and then rewrite.
    ret = RewriteExternalCssToResource(in_url, output_resource);
  } else {
    message_handler->Message(kError, "Could not decode original CSS url %s",
                             output_resource->name().as_string().c_str());
  }
  // For some reason we only call the callback if we succeed.
  if (ret) {
    callback->Done(ret);
  }
  return ret;
}

void CssFilter::Initialize(Statistics* statistics) {
  // TODO(jmarantz): Add statistics for CSS rewrites.

  // Note: This is not thread-safe, but I don't believe we need it to be.
  if (at_exit_manager == NULL) {
    at_exit_manager = new base::AtExitManager;
  }
}

}  // namespace net_instaweb
