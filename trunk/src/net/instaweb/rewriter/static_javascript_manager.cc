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

// Author: guptaa@google.com (Ashish Gupta)

#include "net/instaweb/rewriter/public/static_javascript_manager.h"

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/doctype.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

extern const char* JS_add_instrumentation;
extern const char* JS_add_instrumentation_opt;
extern const char* JS_client_domain_rewriter;
extern const char* JS_client_domain_rewriter_opt;
extern const char* JS_critical_images_beacon;
extern const char* JS_critical_images_beacon_opt;
extern const char* JS_defer_iframe;
extern const char* JS_defer_iframe_opt;
extern const char* JS_delay_images;
extern const char* JS_delay_images_opt;
extern const char* JS_delay_images_inline;
extern const char* JS_delay_images_inline_opt;
extern const char* JS_js_defer;
extern const char* JS_js_defer_opt;
extern const char* JS_lazyload_images;
extern const char* JS_lazyload_images_opt;
extern const char* JS_deterministic;
extern const char* JS_deterministic_opt;
extern const char* JS_detect_reflow;
extern const char* JS_detect_reflow_opt;
extern const char* JS_local_storage_cache;
extern const char* JS_local_storage_cache_opt;

// The generated files(blink.js, js_defer.js) are named in "<hash>-<fileName>"
// format.
const char StaticJavascriptManager::kGStaticBase[] =
    "http://www.gstatic.com/psa/static/";
const char StaticJavascriptManager::kBlinkGstaticSuffix[] = "-blink.js";

// Following are file names when gstatic is not used.
// The file name for debug version is appended with '_debug'.
// Eg: <fileName>[_debug].<md5>.js
const char StaticJavascriptManager::kDefaultLibraryUrlPrefix[] = "/psajs/";
const char StaticJavascriptManager::kBlinkJsFileName[] = "blink";
const char StaticJavascriptManager::kDeferJsFileName[] = "js_defer";
const char StaticJavascriptManager::kDeferJsDebugFileName[] = "js_defer_debug";
const char StaticJavascriptManager::kJsExtension[] = ".js";

StaticJavascriptManager::StaticJavascriptManager(
    UrlNamer* url_namer,
    Hasher* hasher,
    MessageHandler* message_handler)
    : url_namer_(url_namer),
      hasher_(hasher),
      message_handler_(message_handler),
      serve_js_from_gstatic_(false),
      library_url_prefix_(kDefaultLibraryUrlPrefix) {
  InitializeFileNameToJsStringMap();
  InitializeJsStrings();
  InitBlink();
  InitDeferJs();

  ResponseHeaders header;
  // TODO(ksimbili): Define a new constant kShortCacheTtlForMismatchedContentMs
  // in ServerContext for 5min.
  header.SetDateAndCaching(0, ResponseHeaders::kImplicitCacheTtlMs);
  cache_header_with_private_ttl_ = StrCat(
      header.Lookup1(HttpAttributes::kCacheControl),
      ",private");

  header.Clear();
  header.SetDateAndCaching(0, ServerContext::kGeneratedMaxAgeMs);
  cache_header_with_long_ttl_ = header.Lookup1(HttpAttributes::kCacheControl);
}

StaticJavascriptManager::~StaticJavascriptManager() {
}

const GoogleString& StaticJavascriptManager::GetBlinkJsUrl(
    const RewriteOptions* options) const {
  if (serve_js_from_gstatic_ && !options->Enabled(RewriteOptions::kDebug)) {
    return blink_javascript_gstatic_url_;
  }
  return blink_javascript_handler_url_;
}

void StaticJavascriptManager::InitBlink() {
  // TODO(ksimbili): Make blink.js to have hash and serve it through
  // static_javascript_manager
  blink_javascript_handler_url_ =
      StrCat(url_namer_->get_proxy_domain(),
             library_url_prefix_,
             kBlinkJsFileName, kJsExtension);
}

void StaticJavascriptManager::set_gstatic_blink_hash(const GoogleString& hash) {
  if (serve_js_from_gstatic_) {
    CHECK(!hash.empty());
    blink_javascript_gstatic_url_ =
        StrCat(kGStaticBase, hash, kBlinkGstaticSuffix);
  }
}

const GoogleString& StaticJavascriptManager::GetDeferJsUrl(
    const RewriteOptions* options) const {
  if (options->Enabled(RewriteOptions::kDebug)) {
    return defer_javascript_debug_url_;
  }
  return defer_javascript_url_;
}

void StaticJavascriptManager::InitDeferJs() {
  defer_javascript_url_ =
      StrCat(url_namer_->get_proxy_domain(),
             library_url_prefix_,
             kDeferJsFileName,
             ".", file_name_to_js_map_[kDeferJsFileName].second,  // hash
             kJsExtension);

  defer_javascript_debug_url_ =
      StrCat(url_namer_->get_proxy_domain(),
             library_url_prefix_,
             kDeferJsDebugFileName,
             ".", file_name_to_js_map_[kDeferJsDebugFileName].second,
             kJsExtension);
}

void StaticJavascriptManager::set_gstatic_defer_js_hash(
    const GoogleString& hash) {
  if (serve_js_from_gstatic_) {
    CHECK(!hash.empty());
    // TODO(ksimbili): Modify the Gstatic Urls to confirm with the url naming
    // pattern as in non-GStatic case.
    defer_javascript_url_ =
        StrCat(kGStaticBase, hash, "-", kDeferJsFileName, kJsExtension);
  }
}

void StaticJavascriptManager::InitializeFileNameToJsStringMap() {
  file_name_to_js_map_[kDeferJsFileName] =
      std::make_pair(JS_js_defer_opt, hasher_->Hash(JS_js_defer_opt));
  file_name_to_js_map_[kDeferJsDebugFileName] =
      std::make_pair(JS_js_defer, hasher_->Hash(JS_js_defer));
}

void StaticJavascriptManager::InitializeJsStrings() {
  // Initialize compiled javascript strings.
  opt_js_vector_.resize(static_cast<int>(kEndOfModules));
  opt_js_vector_[static_cast<int>(kAddInstrumentationJs)] =
      JS_add_instrumentation_opt;
  opt_js_vector_[static_cast<int>(kClientDomainRewriter)] =
      JS_client_domain_rewriter_opt;
  opt_js_vector_[static_cast<int>(kCriticalImagesBeaconJs)] =
      JS_critical_images_beacon_opt;
  opt_js_vector_[static_cast<int>(kDeferIframe)] = JS_defer_iframe_opt;
  opt_js_vector_[static_cast<int>(kDeferJs)] = JS_js_defer_opt;
  opt_js_vector_[static_cast<int>(kDelayImagesJs)] =
      JS_delay_images_opt;
  opt_js_vector_[static_cast<int>(kDelayImagesInlineJs)] =
      JS_delay_images_inline_opt;
  opt_js_vector_[static_cast<int>(kLazyloadImagesJs)] =
      JS_lazyload_images_opt;
  opt_js_vector_[static_cast<int>(kDetectReflowJs)] =
      JS_detect_reflow_opt;
  opt_js_vector_[static_cast<int>(kDeterministicJs)] =
      JS_deterministic_opt;
  opt_js_vector_[static_cast<int>(kLocalStorageCacheJs)] =
      JS_local_storage_cache_opt;
  // Initialize cleartext javascript strings.
  debug_js_vector_.resize(static_cast<int>(kEndOfModules));
  debug_js_vector_[static_cast<int>(kAddInstrumentationJs)] =
      JS_add_instrumentation;
  debug_js_vector_[static_cast<int>(kClientDomainRewriter)] =
      JS_client_domain_rewriter;
  debug_js_vector_[static_cast<int>(kCriticalImagesBeaconJs)] =
      JS_critical_images_beacon;
  debug_js_vector_[static_cast<int>(kDeferIframe)] = JS_defer_iframe;
  debug_js_vector_[static_cast<int>(kDeferJs)] = JS_js_defer;
  debug_js_vector_[static_cast<int>(kDelayImagesJs)] =
      JS_delay_images;
  debug_js_vector_[static_cast<int>(kDelayImagesInlineJs)] =
      JS_delay_images_inline;
  debug_js_vector_[static_cast<int>(kLazyloadImagesJs)] =
      JS_lazyload_images;
  debug_js_vector_[static_cast<int>(kDetectReflowJs)] =
      JS_detect_reflow;
  debug_js_vector_[static_cast<int>(kDeterministicJs)] =
      JS_deterministic;
  debug_js_vector_[static_cast<int>(kLocalStorageCacheJs)] =
      JS_local_storage_cache;
}

const char* StaticJavascriptManager::GetJsSnippet(
    StaticJavascriptManager::JsModule js_module,
    const RewriteOptions* options) {
  CHECK(js_module != kEndOfModules);
  int module = js_module;
  return options->Enabled(RewriteOptions::kDebug) ?
      debug_js_vector_[module] : opt_js_vector_[module];
}

void StaticJavascriptManager::AddJsToElement(
    StringPiece js, HtmlElement* script, RewriteDriver* driver) {
  DCHECK(script->keyword() == HtmlName::kScript);
  // CDATA tags are required for inlined JS in XHTML pages to prevent
  // interpretation of certain characters (like &). In apache, something
  // downstream of mod_pagespeed could modify the content type of the response.
  // So CDATA tags are added conservatively if we are not sure that it is safe
  // to exclude them.
  GoogleString js_str;

  if (!(driver->server_context()->response_headers_finalized() &&
        driver->MimeTypeXhtmlStatus() == RewriteDriver::kIsNotXhtml)) {
    StrAppend(&js_str, "//<![CDATA[\n", js, "\n//]]>");
    js = js_str;
  }

  if (!driver->doctype().IsVersion5()) {
    driver->AddAttribute(script, HtmlName::kType, "text/javascript");
  }
  HtmlCharactersNode* script_content = driver->NewCharactersNode(script, js);
  driver->AppendChild(script, script_content);
}

bool StaticJavascriptManager::GetJsSnippet(StringPiece file_name,
                                           StringPiece* content,
                                           StringPiece* cache_header) {
  StringPieceVector names;
  SplitStringPieceToVector(file_name, ".", &names, true);
  // Expected file_name format is <name>[_debug].<HASH>.js
  // If file names doesn't contain hash in it, just return, because they may be
  // spurious request.
  if (names.size() != 3) {
    message_handler_->Message(kError, "Invalid url requested: %s.",
                              file_name.as_string().c_str());
    return false;
  }
  GoogleString plain_file_name;
  names[0].CopyToString(&plain_file_name);;

  FileNameToStringsMap::const_iterator p =
      file_name_to_js_map_.find(plain_file_name);
  if (p != file_name_to_js_map_.end()) {
    const JsSnippetHashPair& value = p->second;
    *content = value.first;
    if (cache_header) {
      if (value.second == names[1]) {  // compare hash
        *cache_header = cache_header_with_long_ttl_;
      } else {
        *cache_header = cache_header_with_private_ttl_;
      }
    }
    return true;
  }
  return false;
}

}  // namespace net_instaweb
