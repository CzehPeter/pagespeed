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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_CSS_IMAGE_REWRITER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_CSS_IMAGE_REWRITER_H_

#include <string>
#include "net/instaweb/util/public/string_util.h"

class GURL;

namespace Css {

class Stylesheet;

}  // namespace Css

namespace net_instaweb {

class CacheExtender;
class ImgRewriteFilter;
class MessageHandler;
class RewriteDriver;
class RewriteOptions;
class Statistics;
class Variable;

class CssImageRewriter {
 public:
  CssImageRewriter(RewriteDriver* driver,
                   CacheExtender* cache_extender,
                   ImgRewriteFilter* image_rewriter);
  ~CssImageRewriter();

  static void Initialize(Statistics* statistics);

  // Attempts to rewrite all images in stylesheet. If successful, it mutates
  // stylesheet to point to new images.
  //
  // Returns whether or not it made any changes.
  bool RewriteCssImages(const GURL& base_url, Css::Stylesheet* stylesheet,
                        MessageHandler* handler);

  // Statistics names.
  static const char kImageRewrites[];
  static const char kCacheExtends[];
  static const char kNoRewrite[];

 private:
  bool RewriteImageUrl(const GURL& base_url,
                       const StringPiece& old_rel_url,
                       std::string* new_url,
                       MessageHandler* handler);

  // Needed for resource_manager and options.
  RewriteDriver* driver_;

  // Pointers to other HTML filters used to rewrite images.
  // TODO(sligocki): morlovich suggests separating this out as some
  // centralized API call like rewrite_driver_->RewriteImage().
  CacheExtender* cache_extender_;
  ImgRewriteFilter* image_rewriter_;

  // Statistics
  Variable* image_rewrites_;
  Variable* cache_extends_;
  Variable* no_rewrite_;

  DISALLOW_COPY_AND_ASSIGN(CssImageRewriter);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_CSS_IMAGE_REWRITER_H_
