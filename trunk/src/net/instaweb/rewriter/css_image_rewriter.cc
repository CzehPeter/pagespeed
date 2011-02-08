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

// Author: sligocki@google.com (Shawn Ligocki)

#include "net/instaweb/rewriter/public/css_image_rewriter.h"

#include "net/instaweb/rewriter/public/cache_extender.h"
#include "net/instaweb/rewriter/public/img_rewrite_filter.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/statistics.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"
#include "webutil/css/parser.h"

namespace net_instaweb {

// Statistics names.
const char CssImageRewriter::kImageRewrites[] = "css_image_rewrites";
const char CssImageRewriter::kCacheExtends[] = "css_image_cache_extends";
const char CssImageRewriter::kNoRewrite[] = "css_image_no_rewrite";

CssImageRewriter::CssImageRewriter(RewriteDriver* driver,
                                   CacheExtender* cache_extender,
                                   ImgRewriteFilter* image_rewriter)
    : driver_(driver),
      cache_extender_(cache_extender),
      image_rewriter_(image_rewriter),
      image_rewrites_(NULL),
      cache_extends_(NULL),
      no_rewrite_(NULL) {
  Statistics* stats = driver_->resource_manager()->statistics();
  if (stats != NULL) {
    image_rewrites_ = stats->GetVariable(kImageRewrites);
    // TODO(sligocki): Should this be shared with CacheExtender or kept
    // separately? I think it's useful to know how many images were optimized
    // from CSS files, but people probably also want to know how many total
    // images were cache-extended.
    cache_extends_ = stats->GetVariable(kCacheExtends);
    no_rewrite_ = stats->GetVariable(kNoRewrite);
  }
}

CssImageRewriter::~CssImageRewriter() {}

void CssImageRewriter::Initialize(Statistics* statistics) {
  statistics->AddVariable(kImageRewrites);
  statistics->AddVariable(kCacheExtends);
  statistics->AddVariable(kNoRewrite);
}

bool CssImageRewriter::RewriteImageUrl(const GURL& base_url,
                                       const StringPiece& old_rel_url,
                                       std::string* new_url,
                                       MessageHandler* handler) {
  bool ret = false;
  const char* old_rel_url_cstr = old_rel_url.as_string().c_str();
  scoped_ptr<Resource> input_resource(
      driver_->resource_manager()->CreateInputResource(
          base_url, old_rel_url, driver_->options(), handler));
  if (input_resource.get() != NULL) {
    scoped_ptr<OutputResource::CachedResult> rewrite_info;
    // TODO(sligocki): Try image rewriting.
    //handler->Message(kInfo, "Attempting to rewrite image %s",
    //                 old_rel_url_cstr);
    //rewrite_info.reset(img_rewriter_->RewriteWithCaching(original_url_string));
    //if (rewrite_info.get() != NULL && rewrite_info->optimizable()) {
    //  if (image_rewrites_ != NULL) {
    //    image_rewrites_->Add(1);
    //  }
    //  *new_url = rewrite_info->url();
    //  ret = true;
    //} else
    {
      // Try cache extending.
      handler->Message(kInfo, "Attempting to cache extend image %s",
                       old_rel_url_cstr);
      rewrite_info.reset(
          cache_extender_->RewriteResourceWithCaching(input_resource.get()));
      if (rewrite_info.get() != NULL && rewrite_info->optimizable()) {
        if (cache_extends_ != NULL) {
          cache_extends_->Add(1);
        }
        *new_url = rewrite_info->url();
        ret = true;
      }
    }
  }
  return ret;
}

bool CssImageRewriter::RewriteCssImages(const GURL& base_url,
                                        Css::Stylesheet* stylesheet,
                                        MessageHandler* handler) {
  bool editted = false;
  handler->Message(kInfo, "Starting to rewrite images in CSS in %s",
                   base_url.spec().c_str());
  Css::Rulesets& rulesets = stylesheet->mutable_rulesets();
  for (Css::Rulesets::iterator ruleset_iter = rulesets.begin();
       ruleset_iter != rulesets.end(); ++ruleset_iter) {
    Css::Ruleset* ruleset = *ruleset_iter;
    Css::Declarations& decls = ruleset->mutable_declarations();
    for (Css::Declarations::iterator decl_iter = decls.begin();
         decl_iter != decls.end(); ++decl_iter) {
      Css::Declaration* decl = *decl_iter;
      // Only edit image declarations.
      // TODO(sligocki): Find out where else images can be.
      switch (decl->prop()) {
        case Css::Property::BACKGROUND:
        case Css::Property::BACKGROUND_IMAGE: {
          // Rewrite all URLs. Technically, background-image should only
          // have a single value which is a URL, but background could have
          // more values.
          Css::Values* values = decl->mutable_values();
          for (Css::Values::iterator value_iter = values->begin();
               value_iter != values->end(); ++value_iter) {
            Css::Value* value = *value_iter;
            if (value->GetLexicalUnitType() == Css::Value::URI) {
              std::string rel_url =
                  UnicodeTextToUTF8(value->GetStringValue());
              handler->Message(kInfo, "Found image URL %s", rel_url.c_str());
              std::string new_url;
              if (RewriteImageUrl(base_url, rel_url, &new_url, handler)) {
                // Replace the URL.
                *value_iter = new Css::Value(Css::Value::URI,
                                             UTF8ToUnicodeText(new_url));
                delete value;
                editted = true;
                handler->Message(kInfo, "Successfully rewrote %s to %s",
                                 rel_url.c_str(), new_url.c_str());
              } else {
                if (no_rewrite_ != NULL) {
                  no_rewrite_->Add(1);
                }
                handler->Message(kInfo, "Could not rewrite %s "
                                 "(Perhaps it is being fetched).",
                                 rel_url.c_str());
              }
            }
          }
          break;
        }
        default:
          break;
      }
    }
  }
  return editted;
}

}  // namespace net_instaweb
