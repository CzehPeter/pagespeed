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

#include "net/instaweb/rewriter/public/css_filter.h"

#include "base/at_exit.h"

#include "base/scoped_ptr.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/rewriter/public/css_minify.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/util/public/content_type.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/url_escaper.h"
#include "net/instaweb/util/public/writer.h"
#include "webutil/css/parser.h"

namespace {

base::AtExitManager* at_exit_manager = NULL;

}  // namespace

namespace net_instaweb {

namespace {

const char kStylesheet[] = "stylesheet";

}  // namespace

// Statistics variable names.
const char CssFilter::kFilesMinified[] = "css_filter_files_minified";
const char CssFilter::kMinifiedBytesSaved[] = "css_filter_minified_bytes_saved";
const char CssFilter::kParseFailures[] = "css_filter_parse_failures";

CssFilter::CssFilter(RewriteDriver* driver, const StringPiece& path_prefix,
                     bool rewrite_images_from_css,
                     CacheExtender* cache_extender,
                     ImgRewriteFilter* image_rewriter)
    : RewriteSingleResourceFilter(driver, path_prefix),
      in_style_element_(false),
      rewrite_images_(rewrite_images_from_css),
      image_rewriter_(driver, cache_extender, image_rewriter),
      s_style_(html_parse_->Intern("style")),
      s_link_(html_parse_->Intern("link")),
      s_rel_(html_parse_->Intern("rel")),
      s_href_(html_parse_->Intern("href")),
      num_files_minified_(NULL),
      minified_bytes_saved_(NULL),
      num_parse_failures_(NULL) {
  Statistics* stats = resource_manager_->statistics();
  if (stats != NULL) {
    num_files_minified_ = stats->GetVariable(CssFilter::kFilesMinified);
    minified_bytes_saved_ = stats->GetVariable(CssFilter::kMinifiedBytesSaved);
    num_parse_failures_ = stats->GetVariable(CssFilter::kParseFailures);
  }
}

void CssFilter::Initialize(Statistics* statistics) {
  if (statistics != NULL) {
    statistics->AddVariable(CssFilter::kFilesMinified);
    statistics->AddVariable(CssFilter::kMinifiedBytesSaved);
    statistics->AddVariable(CssFilter::kParseFailures);
    CssImageRewriter::Initialize(statistics);
  }

  InitializeAtExitManager();
}

void CssFilter::Terminate() {
  // Note: This is not thread-safe, but I don't believe we need it to be.
  if (at_exit_manager != NULL) {
    delete at_exit_manager;
    at_exit_manager = NULL;
  }
}

void CssFilter::InitializeAtExitManager() {
  // Note: This is not thread-safe, but I don't believe we need it to be.
  if (at_exit_manager == NULL) {
    at_exit_manager = new base::AtExitManager;
  }
}

void CssFilter::StartDocumentImpl() {
  in_style_element_ = false;
}

void CssFilter::StartElementImpl(HtmlElement* element) {
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

void CssFilter::EndElementImpl(HtmlElement* element) {
  // Rewrite an inline style.
  if (in_style_element_) {
    CHECK(style_element_ == element);  // HtmlParse should not pass unmatching.

    if (html_parse_->IsRewritable(element) && style_char_node_ != NULL) {
      CHECK(element == style_char_node_->parent());  // Sanity check.
      std::string new_content;
      if (RewriteCssText(style_char_node_->contents(), &new_content,
                         base_gurl(), html_parse_->message_handler())) {
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
      if (element_href != NULL) {
        // If it has a href= attribute
        std::string new_url;
        if (RewriteExternalCss(element_href->value(), &new_url)) {
          element_href->SetValue(new_url);  // Update the href= attribute.
        }
      } else {
        html_parse_->ErrorHere("Link element with no href.");
      }
    }
  }
}

// Return value answers the question: May we rewrite?
// If return false, out_text is undefined.
// css_gurl is the URL used to resolve relative URLs in the CSS.
// Specifically, it should be the address of the CSS document itself.
bool CssFilter::RewriteCssText(const StringPiece& in_text,
                               std::string* out_text,
                               const GURL& css_gurl,
                               MessageHandler* handler) {
  // Load stylesheet w/o expanding background attributes.
  Css::Parser parser(in_text);
  scoped_ptr<Css::Stylesheet> stylesheet(parser.ParseRawStylesheet());

  bool ret = true;
  if (parser.errors_seen_mask() != Css::Parser::kNoError) {
    ret = false;
    html_parse_->InfoHere("CSS parsing error in %s", css_gurl.spec().c_str());
    if (num_parse_failures_ != NULL) {
      num_parse_failures_->Add(1);
    }
  } else {
    // Edit stylesheet.
    bool editted_css = false;
    // TODO(sligocki): Use separate flags for whether to rewrite images or
    // cache extend.
    if (rewrite_images_) {
      editted_css =
          image_rewriter_.RewriteCssImages(css_gurl, stylesheet.get(), handler);
    }

    // Re-serialize stylesheet.
    StringWriter writer(out_text);
    CssMinify::Stylesheet(*stylesheet, &writer, handler);

    // Get signed versions so that we can subtract them.
    int64 out_text_size = static_cast<int64>(out_text->size());
    int64 in_text_size = static_cast<int64>(in_text.size());
    int64 bytes_saved = in_text_size - out_text_size;

    // Don't rewrite if we didn't edit it or make it any smaller.
    if (!editted_css && bytes_saved <= 0) {
      ret = false;
      html_parse_->InfoHere("CSS parser increased size of CSS file %s by %lld "
                            "bytes.", css_gurl.spec().c_str(),
                            static_cast<long long int>(-bytes_saved));
    }

    // Don't rewrite if we blanked the CSS file! (This is a parse error)
    // TODO(sligocki): Don't error if in_text is all whitespace.
    if (out_text_size == 0 && in_text_size != 0) {
      ret = false;
      html_parse_->InfoHere("CSS parsing error in %s", css_gurl.spec().c_str());
      if (num_parse_failures_ != NULL) {
        num_parse_failures_->Add(1);
      }
    }

    // Statistics
    if (ret) {
      html_parse_->InfoHere("Successfully rewrote CSS file %s saving %lld "
                            "bytes.", css_gurl.spec().c_str(),
                            static_cast<long long int>(bytes_saved));
      if (num_files_minified_ != NULL) {
        num_files_minified_->Add(1);
        minified_bytes_saved_->Add(bytes_saved);
      }
    }
    // TODO(sligocki): Do we want to save the AST 'stylesheet' somewhere?
    // It currently, deletes itself at the end of the function.
  }

  return ret;
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


// Read an external CSS file, rewrite it and write a new external CSS file.
bool CssFilter::RewriteExternalCss(const StringPiece& in_url,
                                   std::string* out_url) {
  scoped_ptr<OutputResource::CachedResult> rewrite_info(
      RewriteWithCaching(in_url, resource_manager_->url_escaper()));

  if (rewrite_info.get() != NULL && rewrite_info->optimizable()) {
    *out_url = rewrite_info->url();
    return true;
  }
  return false;
}

bool CssFilter::RewriteLoadedResource(const Resource* input_resource,
                                      OutputResource* output_resource) {
  CHECK(input_resource->loaded());
  bool ret = false;
  if (input_resource->ContentsValid()) {
    // Rewrite stylesheet.
    StringPiece in_contents = input_resource->contents();
    std::string out_contents;
    // TODO(sligocki): Store the GURL in the input_resource.
    GURL css_gurl = GoogleUrl::Create(input_resource->url());
    if (css_gurl.is_valid() &&
        RewriteCssText(in_contents, &out_contents, css_gurl,
                       html_parse_->message_handler())) {
      // Write new stylesheet.
      output_resource->SetType(&kContentTypeCss);
      if (resource_manager_->Write(HttpStatus::kOK,
                                   out_contents,
                                   output_resource,
                                   input_resource->CacheExpirationTimeMs(),
                                   html_parse_->message_handler())) {
        ret = output_resource->IsWritten();
      }
    }
  }
  return ret;
}

}  // namespace net_instaweb
