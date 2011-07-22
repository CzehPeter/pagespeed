// Copyright 2010 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Author: mdsteele@google.com (Matthew D. Steele)

#include "net/instaweb/rewriter/public/js_inline_filter.h"

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/doctype.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/rewriter/public/inline_rewrite_context.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/script_tag_scanner.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class JsInlineFilter::Context : public InlineRewriteContext {
 public:
  Context(JsInlineFilter* filter, HtmlElement* element,
          HtmlElement::Attribute* src)
      : InlineRewriteContext(filter, element, src), filter_(filter) {}

  virtual bool ShouldInline(const StringPiece& input) const {
    return filter_->ShouldInline(input);
  }

  virtual void RenderInline(
      const ResourcePtr& resource, const StringPiece& text,
      HtmlElement* element) {
    filter_->RenderInline(resource, text, element);
  }

  virtual const char* id() const {
    // Unlike filters with output resources, which use their ID as part of URLs
    // they make, we are not constrained to 2 characters, so we make our
    // name (used for our cache key) nice and long so at not to worry about
    // someone else using it.
    return "js_inline";
  }

 private:
  JsInlineFilter* filter_;
  DISALLOW_COPY_AND_ASSIGN(Context);
};

JsInlineFilter::JsInlineFilter(RewriteDriver* driver)
    : CommonFilter(driver),
      size_threshold_bytes_(driver->options()->js_inline_max_bytes()),
      script_tag_scanner_(driver_),
      should_inline_(false) {}

JsInlineFilter::~JsInlineFilter() {}

void JsInlineFilter::StartDocumentImpl() {
  should_inline_ = false;
}

void JsInlineFilter::EndDocument() {
}

void JsInlineFilter::StartElementImpl(HtmlElement* element) {
  DCHECK(!should_inline_);

  HtmlElement::Attribute* src;
  if (script_tag_scanner_.ParseScriptElement(element, &src) ==
      ScriptTagScanner::kJavaScript) {
    should_inline_ = (src != NULL) && (src->value() != NULL);
  }
}

void JsInlineFilter::EndElementImpl(HtmlElement* element) {
  if (should_inline_ && driver_->IsRewritable(element)) {
    DCHECK(element->keyword() == HtmlName::kScript);
    HtmlElement::Attribute* attr = element->FindAttribute(HtmlName::kSrc);
    CHECK(attr != NULL);
    const char* src = attr->value();
    DCHECK(src != NULL);
    should_inline_ = false;

    // TODO(morlovich): Consider async/defer here; it may not be a good
    // idea to inline async scripts in particular.

    if (HasAsyncFlow()) {
      (new Context(this, element, attr))->Initiate();
    } else {
      ResourcePtr resource(CreateInputResourceAndReadIfCached(src));
      // TODO(jmaessen): Is the domain lawyer policy the appropriate one here?
      // Or do we still have to check for strict domain equivalence?
      // If so, add an inline-in-page policy to domainlawyer in some form,
      // as we make a similar policy decision in css_inline_filter.
      if ((resource.get() != NULL) && resource->ContentsValid()) {
        StringPiece contents = resource->contents();
        if (ShouldInline(contents)) {
          RenderInline(resource, contents, element);
        }
      }
    }
  }
}

bool JsInlineFilter::ShouldInline(const StringPiece& contents) const {
  // Only inline if it's small enough, and if it doesn't contain
  // "</script>" anywhere.  If we inline an external script containing
  // "</script>", the <script> tag will be ended early.
  // See http://code.google.com/p/modpagespeed/issues/detail?id=106
  // TODO(mdsteele): We should consider rewriting "</script>" to
  //   "<\/script>" instead of just bailing.  But we can't blindly search
  //   and replace because that would break legal (if contrived) code such
  //   as "if(x</script>/){...}", which is comparing x to a regex literal.
  return (contents.size() <= size_threshold_bytes_ &&
          contents.find("</script>") == StringPiece::npos);
}

void JsInlineFilter::RenderInline(
    const ResourcePtr& resource, const StringPiece& contents,
    HtmlElement* element) {
  // If we're in XHTML, we should wrap the script in a <!CDATA[...]]>
  // block to ensure that we don't break well-formedness.  Since XHTML is
  // sometimes interpreted as HTML (which will ignore CDATA delimiters),
  // we have to hide the CDATA delimiters behind Javascript comments.
  // See http://lachy.id.au/log/2006/11/xhtml-script
  // and http://code.google.com/p/modpagespeed/issues/detail?id=125
  if (driver_->doctype().IsXhtml()) {
    // CDATA sections cannot be nested because they end with the first
    // occurance of "]]>", so if the script contains that string
    // anywhere (and we're in XHTML) we can't inline.
    // TODO(mdsteele): Again, we should consider escaping somehow.
    if (contents.find("]]>") == StringPiece::npos) {
      HtmlCharactersNode* node =
          driver_->NewCharactersNode(element, "//<![CDATA[\n");
      node->Append(contents);
      node->Append("\n//]]>");
      driver_->AppendChild(element, node);
      element->DeleteAttribute(HtmlName::kSrc);
    }
  } else {
    // If we're not in XHTML, we can simply paste in the external script
    // verbatim.
    driver_->AppendChild(
        element, driver_->NewCharactersNode(element, contents));
    element->DeleteAttribute(HtmlName::kSrc);
  }
}

void JsInlineFilter::Characters(HtmlCharactersNode* characters) {
  if (should_inline_) {
    DCHECK(characters->parent() != NULL);
    DCHECK(characters->parent()->keyword() == HtmlName::kScript);
    if (OnlyWhitespace(characters->contents())) {
      // If it's just whitespace inside the script tag, it's (probably) safe to
      // just remove it.
      driver_->DeleteElement(characters);
    } else {
      // This script tag isn't empty, despite having a src field.  The contents
      // won't be executed by the browser, but will still be in the DOM; some
      // external scripts like to use this as a place to store data.  So, we'd
      // better not try to inline in this case.
      should_inline_ = false;
    }
  }
}

bool JsInlineFilter::HasAsyncFlow() const {
  return driver_->asynchronous_rewrites();
}

}  // namespace net_instaweb
