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

// Author: pulkitg@google.com (Pulkit Goyal)
//
// Contains implementation of DelayImagesFilter, which delays all the high
// quality images whose low quality inlined data url are available within their
// respective image tag.

#include "net/instaweb/rewriter/public/delay_images_filter.h"

#include <map>
#include <utility>

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/http/public/semantic_type.h"
#include "net/instaweb/http/public/device_properties.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/resource_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/util/enums.pb.h"
#include "net/instaweb/util/public/string.h"

namespace net_instaweb {

const char DelayImagesFilter::kDelayImagesSuffix[] =
    "\npagespeed.delayImagesInit();";

const char DelayImagesFilter::kDelayImagesInlineSuffix[] =
    "\npagespeed.delayImagesInlineInit();";

const char DelayImagesFilter::kOnloadFunction[] =
    "var elem=this;"
    "setTimeout(function(){elem.onload = null;"
    "elem.src=elem.getAttribute('pagespeed_high_res_src');}, 0);";

DelayImagesFilter::DelayImagesFilter(RewriteDriver* driver)
    : driver_(driver),
      static_asset_manager_(
          driver->server_context()->static_asset_manager()),
      num_low_res_inlined_images_(0),
      insert_low_res_images_inplace_(false),
      lazyload_highres_images_(false),
      is_script_inserted_(false) {
}

DelayImagesFilter::~DelayImagesFilter() {}

void DelayImagesFilter::StartDocument() {
  num_low_res_inlined_images_ = 0;
  // Low res images will be placed inside the respective image tag if the user
  // agent is not a mobile, or if mobile aggressive rewriters are turned off.
  // Otherwise, the low res images are inserted at the end of the flush window.
  insert_low_res_images_inplace_ = ShouldRewriteInplace();
  lazyload_highres_images_ = driver_->options()->lazyload_highres_images() &&
      driver_->device_properties()->IsMobile();
  is_script_inserted_ = false;
}

void DelayImagesFilter::EndDocument() {
  low_res_data_map_.clear();
}

void DelayImagesFilter::EndElement(HtmlElement* element) {
  if (element->keyword() == HtmlName::kBody) {
    InsertLowResImagesAndJs(element, /* insert_after_element */ false);
    InsertHighResJs(element);
  } else if (driver_->IsRewritable(element) &&
             (element->keyword() == HtmlName::kImg ||
              element->keyword() == HtmlName::kInput)) {
    // We only handle img and input tag images.  Note that delay_images.js and
    // delay_images_inline.js must be modified to handle other possible tags.
    // We should probably specifically *not* include low res images for link
    // tags of various sorts (favicons, mobile desktop icons, etc.). Use of low
    // res for explicit background images is a more interesting case, but the
    // current DOM walk in the above js files would need to be modified to
    // handle the large number of tags that we can identify in
    // resource_tag_scanner::ScanElement.
    semantic_type::Category category;
    HtmlElement::Attribute* src = resource_tag_scanner::ScanElement(
        element, driver_, &category);

    if (src == NULL || src->DecodedValueOrNull() == NULL ||
        category != semantic_type::kImage) {
      return;
    }
    HtmlElement::Attribute* low_res_src =
        element->FindAttribute(HtmlName::kPagespeedLowResSrc);
    if (low_res_src == NULL || low_res_src->DecodedValueOrNull() == NULL) {
      return;
    }
    ++num_low_res_inlined_images_;
    if (element->FindAttribute(HtmlName::kOnload) == NULL) {
      driver_->log_record()->SetRewriterLoggingStatus(
          RewriteOptions::FilterId(RewriteOptions::kDelayImages),
          RewriterApplication::APPLIED_OK);
      // High res src is added and original img src attribute is removed
      // from img tag.
      driver_->SetAttributeName(src, HtmlName::kPagespeedHighResSrc);
      if (insert_low_res_images_inplace_) {
        // Set the src as the low resolution image.
        driver_->AddAttribute(element, HtmlName::kSrc,
                              low_res_src->DecodedValueOrNull());
        // Add an onload function to set the high resolution image.
        driver_->AddEscapedAttribute(
            element, HtmlName::kOnload, kOnloadFunction);
      } else {
        // Low res image data is collected in low_res_data_map_ map. This
        // low_res_src will be moved just after last low res image in the flush
        // window.
        // It is better to move inlined low resolution data later in the DOM,
        // otherwise they will block further parsing and rendering of the html
        // page.
        // Note that the high resolution images are loaded at end of body.
        const GoogleString& src_content = src->DecodedValueOrNull();
        low_res_data_map_[src_content] = low_res_src->DecodedValueOrNull();
      }
    }
    if (num_low_res_inlined_images_ == driver_->num_inline_preview_images()) {
      if (!insert_low_res_images_inplace_) {
        InsertLowResImagesAndJs(element, /* insert_after_element */ true);
      }
    }
  }
  element->DeleteAttribute(HtmlName::kPagespeedLowResSrc);
}

void DelayImagesFilter::InsertLowResImagesAndJs(HtmlElement* element,
                                                bool insert_after_element) {
  if (low_res_data_map_.empty()) {
    return;
  }
  GoogleString inline_script;
  HtmlElement* current_element = element;
  // Check script for changing src to low res data url is inserted once.
  if (!is_script_inserted_) {
    inline_script = StrCat(
        static_asset_manager_->GetAsset(
            StaticAssetManager::kDelayImagesInlineJs,
            driver_->options()),
        kDelayImagesInlineSuffix,
        static_asset_manager_->GetAsset(
            StaticAssetManager::kDelayImagesJs,
            driver_->options()),
        kDelayImagesSuffix);
    HtmlElement* script_element =
        driver_->NewElement(element, HtmlName::kScript);
    driver_->AddAttribute(script_element, HtmlName::kPagespeedNoDefer, "");
    if (insert_after_element) {
      DCHECK(element->keyword() == HtmlName::kImg ||
             element->keyword() == HtmlName::kInput);
      driver_->InsertElementAfterElement(current_element, script_element);
      current_element = script_element;
    } else {
      DCHECK(element->keyword() == HtmlName::kBody);
      driver_->AppendChild(element, script_element);
    }
    static_asset_manager_->AddJsToElement(
        inline_script, script_element, driver_);
    is_script_inserted_ = true;
  }

  // Generate javascript map for inline data urls where key is url and
  // base64 encoded data url as its value. This map is added to the
  // html at the end of last low res image.
  GoogleString inline_data_script;
  for (StringStringMap::iterator it = low_res_data_map_.begin();
       it != low_res_data_map_.end(); ++it) {
    inline_data_script = StrCat(
        "\npagespeed.delayImagesInline.addLowResImages('",
        it->first, "', '", it->second, "');");
    StrAppend(&inline_data_script,
              "\npagespeed.delayImagesInline.replaceWithLowRes();\n");
    HtmlElement* low_res_element =
        driver_->NewElement(current_element, HtmlName::kScript);
    driver_->AddAttribute(low_res_element, HtmlName::kPagespeedNoDefer, "");
    if (insert_after_element) {
      driver_->InsertElementAfterElement(current_element, low_res_element);
      current_element = low_res_element;
    } else {
      driver_->AppendChild(element, low_res_element);
    }
    static_asset_manager_->AddJsToElement(
        inline_data_script, low_res_element, driver_);
  }
  low_res_data_map_.clear();
}

void DelayImagesFilter::InsertHighResJs(HtmlElement* body_element) {
  if (insert_low_res_images_inplace_ || !is_script_inserted_) {
    return;
  }
  GoogleString js;
  if (lazyload_highres_images_) {
    StrAppend(&js,
              "\npagespeed.delayImages.registerLazyLoadHighRes();\n");
  } else {
    StrAppend(&js,
              "\npagespeed.delayImages.replaceWithHighRes();\n");
  }
  HtmlElement* script = driver_->NewElement(body_element, HtmlName::kScript);
  driver_->AddAttribute(script, HtmlName::kPagespeedNoDefer, "");
  driver_->AppendChild(body_element, script);
  static_asset_manager_->AddJsToElement(js, script, driver_);
}

bool DelayImagesFilter::ShouldRewriteInplace() const {
  const RewriteOptions* options = driver_->options();
  return !(options->enable_aggressive_rewriters_for_mobile() &&
           driver_->device_properties()->IsMobile());
}

void DelayImagesFilter::DetermineEnabled() {
  AbstractLogRecord* log_record = driver_->log_record();
  if (!driver_->device_properties()->SupportsImageInlining()) {
    log_record->LogRewriterHtmlStatus(
        RewriteOptions::FilterId(RewriteOptions::kDelayImages),
        RewriterHtmlApplication::USER_AGENT_NOT_SUPPORTED);
    set_is_enabled(false);
    return;
  }
  log_record->LogRewriterHtmlStatus(
      RewriteOptions::FilterId(RewriteOptions::kDelayImages),
      RewriterHtmlApplication::ACTIVE);
  set_is_enabled(true);
}

}  // namespace net_instaweb
