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

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/javascript_code_block.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_result.h"
#include "net/instaweb/rewriter/public/script_tag_scanner.h"
#include "net/instaweb/rewriter/public/single_rewrite_context.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

namespace {

void CleanupWhitespaceScriptBody(
    RewriteDriver* driver, RewriteContext* context, HtmlCharactersNode* node) {
  if (node != NULL) {
    // Note that an external script tag might contain body data.  We erase this
    // if it is just whitespace; otherwise we leave it alone.  The script body
    // is ignored by all browsers we know of.  However, various sources have
    // encouraged using the body of an external script element to store a
    // post-load callback.  As this technique is preferable to storing callbacks
    // in, say, html comments, we support it here.
    const GoogleString& contents = node->contents();
    for (size_t j = 0; j < contents.size(); ++j) {
      char c = contents[j];
      if (!isspace(c) && c != 0) {
        driver->InfoAt(context, "Retaining contents of script tag;"
                       " probably data for external script.");
        return;
      }
    }
    driver->DeleteElement(node);
  }
}

}  // namespace

JavascriptFilter::JavascriptFilter(RewriteDriver* driver)
    : RewriteFilter(driver),
      body_node_(NULL),
      script_in_progress_(NULL),
      script_src_(NULL),
      some_missing_scripts_(false),
      config_(NULL),
      script_tag_scanner_(driver_) { }

JavascriptFilter::~JavascriptFilter() { }

void JavascriptFilter::InitStats(Statistics* statistics) {
  JavascriptRewriteConfig::InitStats(statistics);
}

class JavascriptFilter::Context : public SingleRewriteContext {
 public:
  Context(RewriteDriver* driver, RewriteContext* parent,
          JavascriptRewriteConfig* config,
          HtmlCharactersNode* body_node)
      : SingleRewriteContext(driver, parent, NULL),
        config_(config),
        body_node_(body_node) {
  }

  RewriteResult RewriteJavascript(
      const ResourcePtr& input, const OutputResourcePtr& output) {
    ServerContext* server_context = FindServerContext();
    MessageHandler* message_handler = server_context->message_handler();
    JavascriptCodeBlock code_block(
        input->contents(), config_, input->url(), message_handler);
    // Check whether this code should, for various reasons, not be rewritten.
    if (PossiblyRewriteToLibrary(code_block, server_context, output)) {
      // Code was a library, so we will use the canonical url rather than create
      // an optimized version.
      // libraries_identified is incremented internally in
      // PossiblyRewriteToLibrary, so there's no specific failure metric here.
      return kRewriteFailed;
    }
    if (!config_->minify()) {
      config_->minification_disabled()->Add(1);
      return kRewriteFailed;
    }
    if (!code_block.ProfitableToRewrite()) {
      // Optimization happened but wasn't useful; the base class will remember
      // this for later so we don't attempt to rewrite twice.
      server_context->message_handler()->Message(
          kInfo, "Script %s didn't shrink.", code_block.message_id().c_str());
      config_->did_not_shrink()->Add(1);
      return kRewriteFailed;
    }
    // Code block was optimized, so write out the new version.
    if (!WriteExternalScriptTo(
            input, code_block.Rewritten(), server_context, output)) {
      config_->failed_to_write()->Add(1);
      return kRewriteFailed;
    }
    // We only check and rule out introspective javascript *after* writing the
    // minified script because we might be performing AJAX rewriting, in which
    // case we'll rewrite without changing the url and can ignore introspection.
    // TODO(jmaessen): Figure out how to distinguish AJAX rewrites so that we
    // don't need the special control flow (and url_relocatable field in
    // cached_result and its treatment in rewrite_context).
    if (Options()->avoid_renaming_introspective_javascript() &&
        JavascriptCodeBlock::UnsafeToRename(code_block.Rewritten())) {
      CachedResult* result = output->EnsureCachedResultCreated();
      result->set_url_relocatable(false);
      message_handler->Message(
          kInfo, "Script %s is unsafe to replace.", input->url().c_str());
    }
    return kRewriteOk;
  }

 protected:
  // Implements the asynchronous interface required by SingleRewriteContext.
  //
  // TODO(jmarantz): this should be done as a SimpleTextFilter.
  virtual void RewriteSingle(
      const ResourcePtr& input, const OutputResourcePtr& output) {
    RewriteDone(RewriteJavascript(input, output), 0);
  }

  virtual void Render() {
    CleanupWhitespaceScriptBody(Driver(), this, body_node_);
    if (num_output_partitions() != 1) {
      return;
    }
    CachedResult* result = output_partition(0);
    ResourceSlot* output_slot = slot(0).get();
    if (!result->optimizable()) {
      if (result->canonicalize_url() && output_slot->CanDirectSetUrl()) {
        // Use the canonical library url and disable the later render step.
        // This permits us to patch in a library url that doesn't correspond to
        // the OutputResource naming scheme.
        // Note that we can't direct set the url during AJAX rewriting, but we
        // have computed and cached the library match for any subsequent visit
        // to the page.
        output_slot->DirectSetUrl(result->url());
      }
      return;
    }
    // The url or script content is changing, so log that fact.
    config_->num_uses()->Add(1);
    if (Driver()->log_record() != NULL) {
      Driver()->log_record()->LogAppliedRewriter(id());
    }
  }

  virtual OutputResourceKind kind() const { return kRewrittenResource; }

  virtual const char* id() const { return RewriteOptions::kJavascriptMinId; }

 private:
  // Take script_out, which is derived from the script at script_url,
  // and write it to script_dest.
  // Returns true on success, reports failures itself.
  bool WriteExternalScriptTo(
      const ResourcePtr script_resource,
      const StringPiece& script_out, ServerContext* server_context,
      const OutputResourcePtr& script_dest) {
    bool ok = false;
    MessageHandler* message_handler = server_context->message_handler();
    server_context->MergeNonCachingResponseHeaders(
        script_resource, script_dest);
    // Try to preserve original content type to avoid breaking upstream proxies
    // and the like.
    const ContentType* content_type = script_resource->type();
    if (content_type == NULL ||
        content_type->type() != ContentType::kJavascript) {
      content_type = &kContentTypeJavascript;
    }
    if (server_context->Write(ResourceVector(1, script_resource),
                              script_out,
                              content_type,
                              script_resource->charset(),
                              script_dest.get(),
                              message_handler)) {
      ok = true;
      message_handler->Message(kInfo, "Rewrite script %s to %s",
                               script_resource->url().c_str(),
                               script_dest->url().c_str());
    }
    return ok;
  }

  // Decide if given code block is a JS library, and if so set up CachedResult
  // to reflect this fact.
  bool PossiblyRewriteToLibrary(
      const JavascriptCodeBlock& code_block, ServerContext* server_context,
      const OutputResourcePtr& output) {
    StringPiece library_url = code_block.ComputeJavascriptLibrary();
    if (library_url.empty()) {
      return false;
    }
    // We expect canonical urls to be protocol relative, and so we use the base
    // to provide a protocol when one is missing (while still permitting
    // absolute canonical urls when they are required).
    GoogleUrl library_gurl(Driver()->base_url(), library_url);
    server_context->message_handler()->Message(
        kInfo, "Script %s is %s", code_block.message_id().c_str(),
        library_gurl.UncheckedSpec().as_string().c_str());
    if (!library_gurl.is_valid()) {
      return false;
    }
    // We remember the canonical url in the CachedResult in the metadata cache,
    // but don't actually write any kind of resource corresponding to the
    // rewritten file (since we don't need it).  This means we'll end up with a
    // CachedResult with a url() set, but none of the output resource metadata
    // such as a hash().  We set canonicalize_url to signal the Render() method
    // below to handle this case.  If it's useful for another filter, the logic
    // here can move up to RewriteContext::Propagate(...), but this ought to be
    // sufficient for a single filter-specific path.
    CachedResult* cached = output->EnsureCachedResultCreated();
    cached->set_url(library_gurl.Spec().data(),
                    library_gurl.Spec().size());
    cached->set_canonicalize_url(true);
    ResourceSlotPtr output_slot = slot(0);
    output_slot->set_disable_further_processing(true);
    return true;
  }

  JavascriptRewriteConfig* config_;
  // The node containing the body of the script tag, or NULL.
  HtmlCharactersNode* body_node_;
};

void JavascriptFilter::StartElementImpl(HtmlElement* element) {
  // These ought to be invariants.  If they're not, we may leak
  // memory and/or fail to optimize, but it's not a disaster.
  DCHECK(script_in_progress_ == NULL);
  DCHECK(body_node_ == NULL);

  switch (script_tag_scanner_.ParseScriptElement(element, &script_src_)) {
    case ScriptTagScanner::kJavaScript:
      script_in_progress_ = element;
      if (script_src_ != NULL) {
        driver_->InfoHere("Found script with src %s",
                          script_src_->DecodedValueOrNull());
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
    // Save a reference to characters encountered in the script body.
    body_node_ = characters;
  }
}

// Set up config_ if it has not already been initialized.  We must do this
// lazily because at filter creation time many of the options have not yet been
// set up correctly.
void JavascriptFilter::InitializeConfig() {
  DCHECK(config_.get() == NULL);
  config_.reset(
      new JavascriptRewriteConfig(
          driver_->server_context()->statistics(),
          driver_->options()->Enabled(RewriteOptions::kRewriteJavascript),
          driver_->options()->javascript_library_identification()));
}

void JavascriptFilter::RewriteInlineScript() {
  if (body_node_ != NULL) {
    // First buffer up script data and minify it.
    GoogleString* script = body_node_->mutable_contents();
    MessageHandler* message_handler = driver_->message_handler();
    const JavascriptCodeBlock code_block(
        *script, config_.get(), driver_->UrlLine(), message_handler);
    StringPiece library_url = code_block.ComputeJavascriptLibrary();
    if (!library_url.empty()) {
      // TODO(jmaessen): outline and use canonical url.
      driver_->InfoHere("Script is inlined version of %s",
                        library_url.as_string().c_str());
    }
    if (code_block.ProfitableToRewrite()) {
      // Replace the old script string with the new, minified one.
      GoogleString* rewritten_script = code_block.RewrittenString();
      if ((driver_->MimeTypeXhtmlStatus() != RewriteDriver::kIsNotXhtml) &&
          (script->find("<![CDATA[") != StringPiece::npos) &&
          !StringPiece(*rewritten_script).starts_with(
              "<![CDATA")) {  // See Issue 542.
        // Minifier strips leading and trailing CDATA comments from scripts.
        // Restore them if necessary and safe according to the original script.
        script->clear();
        StrAppend(script, "//<![CDATA[\n", *rewritten_script, "\n//]]>");
      } else {
        // Swap in the minified code to replace the original code.
        script->swap(*rewritten_script);
      }
      config_->num_uses()->Add(1);
      LogFilterModifiedContent();
    }
  }
}

// External script; minify and replace with rewritten version (also external).
void JavascriptFilter::RewriteExternalScript() {
  const StringPiece script_url(script_src_->DecodedValueOrNull());
  ResourcePtr resource = CreateInputResource(script_url);
  if (resource.get() != NULL) {
    ResourceSlotPtr slot(
        driver_->GetSlot(resource, script_in_progress_, script_src_));
    Context* jrc = new Context(driver_, NULL, config_.get(), body_node_);
    jrc->AddSlot(slot);
    driver_->InitiateRewrite(jrc);
  }
}

// Reset state at end of script.
void JavascriptFilter::CompleteScriptInProgress() {
  body_node_ = NULL;
  script_in_progress_ = NULL;
  script_src_ = NULL;
}

void JavascriptFilter::EndElementImpl(HtmlElement* element) {
  if (script_in_progress_ != NULL &&
      driver_->IsRewritable(script_in_progress_) &&
      driver_->IsRewritable(element)) {
    if (element->keyword() == HtmlName::kScript) {
      if (element->close_style() == HtmlElement::BRIEF_CLOSE) {
        driver_->InfoHere("Brief close of script tag (non-portable)");
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

RewriteContext* JavascriptFilter::MakeRewriteContext() {
  InitializeConfigIfNecessary();
  // A resource fetch.  This means a client has requested minified content;
  // we'll fail the request (serving the existing content) if minification is
  // disabled for this resource (eg because we've recognized it as a library).
  // This usually happens because the underlying JS content or rewrite
  // configuration changed since the client fetched a rewritten page.
  return new Context(driver_, NULL, config_.get(), NULL /* no body node */);
}

RewriteContext* JavascriptFilter::MakeNestedRewriteContext(
    RewriteContext* parent, const ResourceSlotPtr& slot) {
  InitializeConfigIfNecessary();
  // A nested rewrite, should work just like an HTML rewrite does.
  Context* context = new Context(NULL /* driver */, parent, config_.get(),
                                 NULL /* no body node */);
  context->AddSlot(slot);
  return context;
}

}  // namespace net_instaweb
