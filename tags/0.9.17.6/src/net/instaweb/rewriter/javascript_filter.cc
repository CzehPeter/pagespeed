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

// Author: jmaessen@google.com (Jan Maessen)

#include "net/instaweb/rewriter/public/javascript_filter.h"

#include <cctype>
#include <cstddef>
#include <vector>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/javascript_code_block.h"
#include "net/instaweb/rewriter/public/javascript_library_identification.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_single_resource_filter.h"
#include "net/instaweb/rewriter/public/script_tag_scanner.h"
#include "net/instaweb/rewriter/public/single_rewrite_context.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/content_type.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class Statistics;

JavascriptFilter::JavascriptFilter(RewriteDriver* driver,
                                   const StringPiece& path_prefix)
    : RewriteSingleResourceFilter(driver, path_prefix),
      script_in_progress_(NULL),
      script_src_(NULL),
      some_missing_scripts_(false),
      config_(driver->resource_manager()->statistics()),
      script_tag_scanner_(driver_) { }

JavascriptFilter::~JavascriptFilter() { }

void JavascriptFilter::Initialize(Statistics* statistics) {
  JavascriptRewriteConfig::Initialize(statistics);
}

class JavascriptRewriteContext : public SingleRewriteContext {
 public:
  JavascriptRewriteContext(RewriteDriver* driver,
                           const ResourceSlotPtr& slot,
                           JavascriptRewriteConfig* config)
      : SingleRewriteContext(driver, NULL),
        config_(config) {
    AddSlot(slot);
  }

  RewriteSingleResourceFilter::RewriteResult RewriteSingle(
      const ResourcePtr& input, const OutputResourcePtr& output) {
    MessageHandler* message_handler = resource_manager()->message_handler();
    StringPiece script = input->contents();
    JavascriptCodeBlock code_block(script, config_, input->url(),
                                   message_handler);
    JavascriptLibraryId library = code_block.ComputeJavascriptLibrary();
    if (library.recognized()) {
      message_handler->Message(kInfo, "Script %s is %s %s",
                               input->url().c_str(),
                               library.name(), library.version());
    }

    bool ok = code_block.ProfitableToRewrite();
    if (ok) {
      // Give the script a nice mimetype and extension.
      // (There is no harm in doing this, they're ignored anyway).
      output->SetType(&kContentTypeJavascript);
      ok = WriteExternalScriptTo(input.get(), code_block.Rewritten(),
                                 output.get());
    } else {
      // Rewriting happened but wasn't useful; as we return false base class
      // will remember this for later so we don't attempt to rewrite twice.
      message_handler->Message(kInfo, "Script %s didn't shrink",
                               input->url().c_str());
    }

    return ok ? RewriteSingleResourceFilter::kRewriteOk :
        RewriteSingleResourceFilter::kRewriteFailed;
  }

  // Take script_out, which is derived from the script at script_url,
  // and write it to script_dest.
  // Returns true on success, reports failures itself.
  bool WriteExternalScriptTo(
      const Resource* script_resource,
      const StringPiece& script_out, OutputResource* script_dest) {
    bool ok = false;
    ResourceManager* rm = resource_manager();
    MessageHandler* message_handler = rm->message_handler();
    int64 origin_expire_time_ms = script_resource->CacheExpirationTimeMs();
    if (rm->Write(HttpStatus::kOK, script_out, script_dest,
                  origin_expire_time_ms, message_handler)) {
      ok = true;
      message_handler->Message(kInfo, "Rewrite script %s to %s",
                               script_resource->url().c_str(),
                               script_dest->url().c_str());
    }
    return ok;
  }

  virtual OutputResourceKind kind() const { return kRewrittenResource; }

 protected:
  virtual const char* id() const { return RewriteDriver::kJavascriptMinId; }

 private:
  JavascriptRewriteConfig* config_;
};

void JavascriptFilter::StartElementImpl(HtmlElement* element) {
  CHECK(script_in_progress_ == NULL);

  switch (script_tag_scanner_.ParseScriptElement(element, &script_src_)) {
    case ScriptTagScanner::kJavaScript:
      script_in_progress_ = element;
      if (script_src_ != NULL) {
        driver_->InfoHere("Found script with src %s", script_src_->value());
      }
      break;
    case ScriptTagScanner::kUnknownScript: {
      GoogleString script_dump;
      element->ToString(&script_dump);
      driver_->InfoHere("Unrecognized script:'%s'", script_dump.c_str());
      break;
    }
    case ScriptTagScanner::kNonScript:
      break;
  }
}

void JavascriptFilter::Characters(HtmlCharactersNode* characters) {
  if (script_in_progress_ != NULL) {
    // Note that we're keeping a vector of nodes here,
    // and appending them lazily at the end.  This is
    // because there's usually only 1 HtmlCharactersNode involved,
    // and we end up not actually needing to copy the string.
    buffer_.push_back(characters);
  }
}

// Flatten script fragments in buffer_, using script_buffer to hold
// the data if necessary.  Return a StringPiece referring to the data.
const StringPiece JavascriptFilter::FlattenBuffer(GoogleString* script_buffer) {
  const int buffer_size = buffer_.size();
  if (buffer_.size() == 1) {
    StringPiece result(buffer_[0]->contents());
    return result;
  } else {
    for (int i = 0; i < buffer_size; i++) {
      script_buffer->append(buffer_[i]->contents());
    }
    StringPiece result(*script_buffer);
    return result;
  }
}

void JavascriptFilter::RewriteInlineScript() {
  const int buffer_size = buffer_.size();
  if (buffer_size > 0) {
    // First buffer up script data and minify it.
    GoogleString script_buffer;
    const StringPiece script = FlattenBuffer(&script_buffer);
    MessageHandler* message_handler = driver_->message_handler();
    JavascriptCodeBlock code_block(script, &config_, driver_->UrlLine(),
                                   message_handler);
    JavascriptLibraryId library = code_block.ComputeJavascriptLibrary();
    if (library.recognized()) {
      driver_->InfoHere("Script is %s %s",
                        library.name(), library.version());
    }
    if (code_block.ProfitableToRewrite()) {
      // Now replace all CharactersNodes with a single CharactersNode containing
      // the minified script.
      HtmlCharactersNode* new_script = driver_->NewCharactersNode(
          buffer_[0]->parent(), code_block.Rewritten());
      driver_->ReplaceNode(buffer_[0], new_script);
      for (int i = 1; i < buffer_size; i++) {
        driver_->DeleteElement(buffer_[i]);
      }
    }
  }
}

// External script; minify and replace with rewritten version (also external).
void JavascriptFilter::RewriteExternalScript() {
  const StringPiece script_url(script_src_->value());
  if (driver_->asynchronous_rewrites()) {
    ResourcePtr resource = CreateInputResource(script_url);
    if (resource.get() != NULL) {
      ResourceSlotPtr slot(
          driver_->GetSlot(resource, script_in_progress_, script_src_));
      JavascriptRewriteContext* jrc = new JavascriptRewriteContext(
          driver_, slot, &config_);
      driver_->InitiateRewrite(jrc);
    }
    return;
  }

  scoped_ptr<CachedResult> rewrite_info(RewriteWithCaching(script_url, NULL));

  if (rewrite_info.get() != NULL && rewrite_info->optimizable()) {
    script_src_->SetValue(rewrite_info->url());
  }

  // Finally, note that the script might contain body data.
  // We erase this if it is just whitespace; otherwise we leave it alone.
  // The script body is ignored by all browsers we know of.
  // However, various sources have encouraged using the body of an
  // external script element to store a post-load callback.
  // As this technique is preferable to storing callbacks in, say, html
  // comments, we support it for now.
  bool allSpaces = true;
  for (size_t i = 0; allSpaces && i < buffer_.size(); ++i) {
    const GoogleString& contents = buffer_[i]->contents();
    for (size_t j = 0; allSpaces && j < contents.size(); ++j) {
      char c = contents[j];
      if (!isspace(c) && c != 0) {
        driver_->WarningHere("Retaining contents of script tag"
                             " even though script is external.");
        allSpaces = false;
      }
    }
  }
  for (size_t i = 0; allSpaces && i < buffer_.size(); ++i) {
    driver_->DeleteElement(buffer_[i]);
  }
}

// Reset state at end of script.
void JavascriptFilter::CompleteScriptInProgress() {
  buffer_.clear();
  script_in_progress_ = NULL;
  script_src_ = NULL;
}

void JavascriptFilter::EndElementImpl(HtmlElement* element) {
  if (script_in_progress_ != NULL &&
      driver_->IsRewritable(script_in_progress_) &&
      driver_->IsRewritable(element)) {
    if (element->keyword() == HtmlName::kScript) {
      if (element->close_style() == HtmlElement::BRIEF_CLOSE) {
        driver_->ErrorHere("Brief close of script tag (non-portable)");
      }
      if (script_src_ == NULL) {
        RewriteInlineScript();
      } else {
        RewriteExternalScript();
      }
      CompleteScriptInProgress();
    } else {
      // Should not happen by construction (parser should not have tags here).
      // Note that if we get here, this test *Will* fail; it is written
      // out longhand to make diagnosis easier.
      CHECK(script_in_progress_ == NULL);
    }
  }
}

void JavascriptFilter::Flush() {
  // TODO(jmaessen): We can be smarter here if it turns out to be necessary (eg
  // by buffering an in-progress script across the flush boundary).
  if (script_in_progress_ != NULL) {
    // Not actually an error!
    driver_->InfoHere("Flush in mid-script; leaving script untouched.");
    CompleteScriptInProgress();
    some_missing_scripts_ = true;
  }
}

void JavascriptFilter::IEDirective(HtmlIEDirectiveNode* directive) {
  CHECK(script_in_progress_ == NULL);
  // We presume an IE directive is concealing some js code.
  some_missing_scripts_ = true;
}

bool JavascriptFilter::ReuseByContentHash() const {
  return true;
}

RewriteSingleResourceFilter::RewriteResult
JavascriptFilter::RewriteLoadedResource(
    const ResourcePtr& script_input,
    const OutputResourcePtr& output_resource) {
  // Temporary code so that we can share the rewriting implementation beteween
  // the old blocking rewrite model and the new async model.
  ResourceSlotPtr dummy_slot;
  JavascriptRewriteContext jrc(driver_, dummy_slot, &config_);
  return jrc.RewriteSingle(script_input, output_resource);
}

}  // namespace net_instaweb
