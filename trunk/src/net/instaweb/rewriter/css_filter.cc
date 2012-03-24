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

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/htmlparse/public/doctype.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/css_flatten_imports_context.h"
#include "net/instaweb/rewriter/public/css_hierarchy.h"
#include "net/instaweb/rewriter/public/css_image_rewriter_async.h"
#include "net/instaweb/rewriter/public/css_minify.h"
#include "net/instaweb/rewriter/public/css_tag_scanner.h"
#include "net/instaweb/rewriter/public/css_util.h"
#include "net/instaweb/rewriter/public/data_url_input_resource.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_combiner.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_result.h"
#include "net/instaweb/rewriter/public/single_rewrite_context.h"
#include "net/instaweb/rewriter/public/usage_data_reporter.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/data_url.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/rewriter/public/rewrite_context.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/writer.h"
#include "webutil/css/parser.h"

#include "base/at_exit.h"

namespace {

base::AtExitManager* at_exit_manager = NULL;

}  // namespace

namespace net_instaweb {

class CacheExtender;
class ImageCombineFilter;
class ImageRewriteFilter;
class MessageHandler;
class UrlSegmentEncoder;

namespace {

const char kStylesheet[] = "stylesheet";

// A slot we use when rewriting inline CSS --- there is no place or need
// to write out an output URL, so it has a no-op Render().
class InlineCssSlot : public ResourceSlot {
 public:
  InlineCssSlot(const ResourcePtr& resource, const GoogleString& location)
      : ResourceSlot(resource), location_(location) {}
  virtual ~InlineCssSlot() {}
  virtual void Render() {}
  virtual GoogleString LocationString() { return location_; }

 private:
  GoogleString location_;
  DISALLOW_COPY_AND_ASSIGN(InlineCssSlot);
};

}  // namespace

// Statistics variable names.
const char CssFilter::kBlocksRewritten[] = "css_filter_blocks_rewritten";
const char CssFilter::kParseFailures[] = "css_filter_parse_failures";
const char CssFilter::kRewritesDropped[] = "css_filter_rewrites_dropped";
const char CssFilter::kTotalBytesSaved[] = "css_filter_total_bytes_saved";
const char CssFilter::kTotalOriginalBytes[] = "css_filter_total_original_bytes";
const char CssFilter::kUses[] = "css_filter_uses";

CssFilter::Context::Context(CssFilter* filter, RewriteDriver* driver,
                            RewriteContext* parent,
                            CacheExtender* cache_extender,
                            ImageRewriteFilter* image_rewriter,
                            ImageCombineFilter* image_combiner,
                            ResourceContext* context)
    : SingleRewriteContext(driver, parent, context),
      filter_(filter),
      driver_(driver),
      image_rewriter_(
          new CssImageRewriterAsync(this, filter, filter->driver_,
                                    cache_extender, image_rewriter,
                                    image_combiner)),
      rewrite_inline_element_(NULL),
      rewrite_inline_char_node_(NULL),
      rewrite_inline_attribute_(NULL),
      in_text_size_(-1) {
  css_base_gurl_.Reset(filter_->decoded_base_url());
  DCHECK(css_base_gurl_.is_valid());
  css_trim_gurl_.Reset(css_base_gurl_);

  if (parent != NULL) {
    // If the context is nested.
    DCHECK(driver_ == NULL);
    driver_ = filter_->driver_;
  }
}

CssFilter::Context::~Context() {
}

bool CssFilter::Context::AbsolutifyIfNeeded(
    const StringPiece& input_contents, Writer* writer,
    MessageHandler* handler) {
  bool ret = false;
  switch (driver_->ResolveCssUrls(css_base_gurl_, css_trim_gurl_.Spec(),
                                  input_contents, writer, handler)) {
    case RewriteDriver::kNoResolutionNeeded:
    case RewriteDriver::kWriteFailed:
      // If kNoResolutionNeeded, we just write out the input_contents, because
      // nothing needed to be changed.
      //
      // If kWriteFailed, this means that the URLs couldn't be transformed
      // (or that writer->Write() actually failed ... I think this shouldn't
      // generally happen). So, we just push out the unedited original,
      // figuring that must be better than nothing.
      //
      // TODO(sligocki): In the fetch path ResolveCssUrls should never fail
      // to transform URLs. We should just absolutify all the ones we can.
      ret = writer->Write(input_contents, handler);
      break;
    case RewriteDriver::kSuccess:
      ret = true;
      break;
  }
  return ret;
}

void CssFilter::Context::Render() {
  if (num_output_partitions() == 0) {
    return;
  }

  const CachedResult& result = *output_partition(0);
  if (result.optimizable()) {
    if (rewrite_inline_char_node_ != NULL) {
      HtmlCharactersNode* new_style_char_node =
          driver_->NewCharactersNode(rewrite_inline_element_,
                                     result.inlined_data());
      driver_->ReplaceNode(rewrite_inline_char_node_, new_style_char_node);
    } else if (rewrite_inline_attribute_ != NULL) {
      rewrite_inline_attribute_->SetValue(result.inlined_data());
    }
    filter_->num_uses_->Add(1);
  }
}

void CssFilter::Context::SetupInlineRewrite(HtmlElement* style_element,
                                            HtmlCharactersNode* text) {
  // To handle nested rewrites of inline CSS, we internally handle it
  // as a rewrite of a data: URL.
  rewrite_inline_element_ = style_element;
  rewrite_inline_char_node_ = text;
}

void CssFilter::Context::SetupAttributeRewrite(HtmlElement* element,
                                               HtmlElement::Attribute* src) {
  rewrite_inline_element_ = element;
  rewrite_inline_attribute_ = src;
}

void CssFilter::Context::SetupExternalRewrite(const GoogleUrl& base_gurl,
                                              const GoogleUrl& trim_gurl) {
  css_base_gurl_.Reset(base_gurl);
  css_trim_gurl_.Reset(trim_gurl);
}

void CssFilter::Context::RewriteSingle(
    const ResourcePtr& input_resource,
    const OutputResourcePtr& output_resource) {
  input_resource_ = input_resource;
  output_resource_ = output_resource;
  StringPiece input_contents = input_resource_->contents();
  // The base URL used when absolutifying sub-resources must be the input
  // URL of this rewrite.
  //
  // The only exception is the case of inline CSS, where we define the
  // input URL to be a data: URL. In this case the base URL is the URL of
  // the HTML page set in the constructor.
  //
  // When our input is the output of CssCombiner, the css_base_gurl_ here
  // is stale (it's the first input to the combination). It ought to be
  // the URL of the output of the combination. Similary the css_trim_gurl_
  // needs to be set from the ultimate output resource.
  if (!StringPiece(input_resource_->url()).starts_with("data:")) {
    css_base_gurl_.Reset(input_resource_->url());
    css_trim_gurl_.Reset(output_resource_->UrlEvenIfHashNotSet());
  }
  in_text_size_ = input_contents.size();
  TimedBool result = filter_->RewriteCssText(
      this, css_base_gurl_, css_trim_gurl_, input_contents, in_text_size_,
      IsInlineAttribute() /* text_is_declarations */,
      driver_->message_handler());

  if (result.value) {
    if (num_nested() > 0) {
      StartNestedTasks();
    } else {
      // We call Harvest() ourselves so we can centralize all the output there.
      Harvest();
    }
  } else {
    RewriteDone(kRewriteFailed, 0);
  }
}

void CssFilter::Context::RewriteCssFromRoot(const StringPiece& contents,
                                            int64 in_text_size,
                                            Css::Stylesheet* stylesheet) {
  in_text_size_ = in_text_size;

  hierarchy_.InitializeRoot(css_base_gurl_, css_trim_gurl_, contents,
                            driver_->doctype().IsXhtml(), stylesheet,
                            driver_->message_handler());

  image_rewriter_->RewriteCss(ImageInlineMaxBytes(), this, &hierarchy_,
                              driver_->message_handler());
}

void CssFilter::Context::RewriteCssFromNested(RewriteContext* parent,
                                              CssHierarchy* hierarchy) {
  image_rewriter_->RewriteCss(ImageInlineMaxBytes(), parent, hierarchy,
                              driver_->message_handler());
}

void CssFilter::Context::Harvest() {
  GoogleString out_text;

  hierarchy_.RollUpStylesheets();

  bool previously_optimized = false;
  for (int i = 0; !previously_optimized && i < num_nested(); ++i) {
    RewriteContext* nested_context = nested(i);
    for (int j = 0; j < nested_context->num_slots(); ++j) {
      if (nested_context->slot(j)->was_optimized()) {
        previously_optimized = true;
        break;
      }
    }
  }

  // May need to absolutify @imports.
  bool absolutified_imports = false;
  if (driver_->ShouldAbsolutifyUrl(css_base_gurl_, css_trim_gurl_, NULL)) {
    absolutified_imports =
        CssMinify::AbsolutifyImports(hierarchy_.mutable_stylesheet(),
                                     css_base_gurl_);
  }

  bool ok = filter_->SerializeCss(
      this, in_text_size_, hierarchy_.mutable_stylesheet(), css_base_gurl_,
      css_trim_gurl_, previously_optimized || absolutified_imports,
      IsInlineAttribute() /* stylesheet_is_declarations */,
      &out_text, driver_->message_handler());
  if (ok) {
    if (rewrite_inline_element_ == NULL) {
      output_resource_->SetType(&kContentTypeCss);
      ResourceManager* manager = Manager();
      manager->MergeNonCachingResponseHeaders(input_resource_,
                                              output_resource_);
      ok = manager->Write(ResourceVector(1, input_resource_),
                          out_text,
                          output_resource_.get(),
                          driver_->message_handler());
    } else {
      output_partition(0)->set_inlined_data(out_text);
    }
  }

  if (ok) {
    RewriteDone(kRewriteOk, 0);
  } else {
    RewriteDone(kRewriteFailed, 0);
  }
}

bool CssFilter::Context::Partition(OutputPartitions* partitions,
                                   OutputResourceVector* outputs) {
  if (rewrite_inline_element_ == NULL) {
    return SingleRewriteContext::Partition(partitions, outputs);
  } else {
    // In case where we're rewriting inline CSS, we don't want an output
    // resource but still want a non-trivial partition.
    // We use kOmitInputHash here as this is for inline content.
    CachedResult* partition = partitions->add_partition();
    slot(0)->resource()->AddInputInfoToPartition(
        Resource::kOmitInputHash, 0, partition);
    outputs->push_back(OutputResourcePtr(NULL));
    return true;
  }
}

GoogleString CssFilter::Context::CacheKeySuffix() const {
  // TODO(morlovich): Make the quirks bit part of the actual output resource
  // name; as ignoring it on the fetch path is unsafe.
  // TODO(nikhilmadan): For ajax rewrites, be conservative and assume its XHTML.
  // Is this right?
  GoogleString suffix = (has_parent() || driver_->doctype().IsXhtml())
      ? "X" : "h";

  if (rewrite_inline_element_ != NULL) {
    // Incorporate the base path of the HTML as part of the key --- it
    // matters for inline CSS since resources are resolved against
    // that (while it doesn't for external CSS, since that uses the
    // stylesheet as the base).
    const Hasher* hasher = Manager()->lock_hasher();
    StrAppend(&suffix, "_@", hasher->Hash(css_base_gurl_.AllExceptLeaf()));
  }

  return suffix;
}

CssFilter::CssFilter(RewriteDriver* driver,
                     CacheExtender* cache_extender,
                     ImageRewriteFilter* image_rewriter,
                     ImageCombineFilter* image_combiner)
    : RewriteFilter(driver),
      in_style_element_(false),
      cache_extender_(cache_extender),
      image_rewrite_filter_(image_rewriter),
      image_combiner_(image_combiner) {
  Statistics* stats = resource_manager_->statistics();
  num_blocks_rewritten_ = stats->GetVariable(CssFilter::kBlocksRewritten);
  num_parse_failures_ = stats->GetVariable(CssFilter::kParseFailures);
  num_rewrites_dropped_ = stats->GetVariable(CssFilter::kRewritesDropped);
  total_bytes_saved_ = stats->GetVariable(CssFilter::kTotalBytesSaved);
  total_original_bytes_ = stats->GetVariable(CssFilter::kTotalOriginalBytes);
  num_uses_ = stats->GetVariable(CssFilter::kUses);
}

CssFilter::~CssFilter() {}

int CssFilter::FilterCacheFormatVersion() const {
  return 1;
}

void CssFilter::Initialize(Statistics* statistics) {
  statistics->AddVariable(CssFilter::kBlocksRewritten);
  statistics->AddVariable(CssFilter::kParseFailures);
  statistics->AddVariable(CssFilter::kRewritesDropped);
  statistics->AddVariable(CssFilter::kTotalBytesSaved);
  statistics->AddVariable(CssFilter::kTotalOriginalBytes);
  statistics->AddVariable(CssFilter::kUses);
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
  meta_tag_charset_.clear();
}

void CssFilter::StartElementImpl(HtmlElement* element) {
  // HtmlParse should not pass us elements inside a style element.
  CHECK(!in_style_element_);
  if (element->keyword() == HtmlName::kStyle) {
    in_style_element_ = true;
    style_element_ = element;
    style_char_node_ = NULL;
  } else {
    bool do_rewrite = false;
    bool check_for_url = false;
    if (driver_->options()->Enabled(RewriteOptions::kRewriteStyleAttributes)) {
      do_rewrite = true;
    } else if (driver_->options()->Enabled(
        RewriteOptions::kRewriteStyleAttributesWithUrl)) {
      check_for_url = true;
    }

    // Rewrite style attribute, if any, and iff enabled.
    if (do_rewrite || check_for_url) {
      // Per http://www.w3.org/TR/CSS21/syndata.html#uri s4.3.4 URLs and URIs:
      // "The format of a URI value is 'url(' followed by ..."
      HtmlElement::Attribute* element_style = element->FindAttribute(
          HtmlName::kStyle);
      if (element_style != NULL &&
          (!check_for_url || CssTagScanner::HasUrl(element_style->value()))) {
        StartAttributeRewrite(element, element_style);
      }
    }
  }
  // We deal with <link> elements in EndElement.
}

void CssFilter::Characters(HtmlCharactersNode* characters_node) {
  if (in_style_element_) {
    if (style_char_node_ == NULL) {
      style_char_node_ = characters_node;
    } else {
      driver_->ErrorHere("Multiple character nodes in style.");
      in_style_element_ = false;
    }
  }
}

void CssFilter::EndElementImpl(HtmlElement* element) {
  // Rewrite an inline style.
  if (in_style_element_) {
    CHECK(style_element_ == element);  // HtmlParse should not pass unmatching.

    if (driver_->IsRewritable(element) && style_char_node_ != NULL) {
      CHECK(element == style_char_node_->parent());  // Sanity check.
      GoogleString new_content;

      StartInlineRewrite(element, style_char_node_);
    }
    in_style_element_ = false;

  // Rewrite an external style.
  } else if (element->keyword() == HtmlName::kLink &&
             driver_->IsRewritable(element)) {
    StringPiece relation(element->AttributeValue(HtmlName::kRel));
    if (StringCaseEqual(relation, kStylesheet)) {
      HtmlElement::Attribute* element_href = element->FindAttribute(
          HtmlName::kHref);
      if (element_href != NULL) {
        // If it has a href= attribute
        StartExternalRewrite(element, element_href);
      } else {
        driver_->ErrorHere("Link element with no href.");
      }
    }
  // Note any meta tag charset specifier.
  } else if (meta_tag_charset_.empty() &&
             element->keyword() == HtmlName::kMeta) {
    GoogleString content, mime_type, charset;
    if (ExtractMetaTagDetails(*element, NULL, &content, &mime_type, &charset)) {
      meta_tag_charset_ = charset;
    }
  }
}

void CssFilter::StartInlineRewrite(HtmlElement* element,
                                   HtmlCharactersNode* text) {
  ResourceSlotPtr slot(MakeSlotForInlineCss(text->contents()));
  CssFilter::Context* rewriter = StartRewriting(slot);
  rewriter->SetupInlineRewrite(element, text);

  // Get the applicable media and charset. If the charset on the link doesn't
  // agree with that of the source page, we can't flatten.
  CssHierarchy* hierarchy = rewriter->mutable_hierarchy();
  GetApplicableMedia(element, hierarchy->mutable_media());
  hierarchy->set_flattening_succeeded(
      GetApplicableCharset(element, hierarchy->mutable_charset()));
}

void CssFilter::StartAttributeRewrite(HtmlElement* element,
                                      HtmlElement::Attribute* style) {
  ResourceSlotPtr slot(MakeSlotForInlineCss(style->value()));
  CssFilter::Context* rewriter = StartRewriting(slot);
  rewriter->SetupAttributeRewrite(element, style);

  // @import is not allowed (nor handled) in attribute CSS, which must be
  // declarations only, so disable flattening from the get-go.
  rewriter->mutable_hierarchy()->set_flattening_succeeded(false);
}

void CssFilter::StartExternalRewrite(HtmlElement* link,
                                     HtmlElement::Attribute* src) {
  // Create the input resource for the slot.
  ResourcePtr input_resource(CreateInputResource(src->value()));
  if (input_resource.get() == NULL) {
    return;
  }
  ResourceSlotPtr slot(driver_->GetSlot(input_resource, link, src));
  CssFilter::Context* rewriter = StartRewriting(slot);
  GoogleUrl input_resource_gurl(input_resource->url());
  rewriter->SetupExternalRewrite(input_resource_gurl, decoded_base_url());

  // Get the applicable media and charset. If the charset on the link doesn't
  // agree with that of the source page, we can't flatten.
  CssHierarchy* hierarchy = rewriter->mutable_hierarchy();
  GetApplicableMedia(link, hierarchy->mutable_media());
  hierarchy->set_flattening_succeeded(
      GetApplicableCharset(link, hierarchy->mutable_charset()));
}

ResourceSlot* CssFilter::MakeSlotForInlineCss(const StringPiece& content) {
  // Create the input resource for the slot.
  GoogleString data_url;
  // TODO(morlovich): This does a lot of useless conversions and
  // copying. Get rid of them.
  DataUrl(kContentTypeCss, PLAIN, content, &data_url);
  ResourcePtr input_resource(DataUrlInputResource::Make(data_url,
                                                        resource_manager_));
  return new InlineCssSlot(input_resource, driver_->UrlLine());
}

CssFilter::Context* CssFilter::StartRewriting(const ResourceSlotPtr& slot) {
  // Create the context add it to the slot, then kick everything off.
  CssFilter::Context* rewriter = MakeContext(driver_, NULL);
  rewriter->AddSlot(slot);
  driver_->InitiateRewrite(rewriter);
  return rewriter;
}

bool CssFilter::GetApplicableCharset(const HtmlElement* element,
                                     GoogleString* charset) const {
  // HTTP1.1 says the default charset is ISO-8859-1 but as the W3C says (in
  // http://www.w3.org/International/O-HTTP-charset.en.php) not many browsers
  // actually do this so a default of "" might be better. Starting from that
  // base, if the headers specify a charset that is used, otherwise if a meta
  // tag specifies a charset that is used.
  StringPiece our_charset("iso-8859-1");
  GoogleString headers_charset;
  ResponseHeaders* headers = driver_->response_headers_ptr();
  if (headers != NULL) {
    headers_charset = headers->DetermineCharset();
    if (!headers_charset.empty()) {
      our_charset = headers_charset;
    }
  }
  if (headers_charset.empty() && !meta_tag_charset_.empty()) {
    our_charset = meta_tag_charset_;
  }
  if (element != NULL) {
    const HtmlElement::Attribute* charset_attribute =
        element->FindAttribute(HtmlName::kCharset);
    if (charset_attribute != NULL) {
      if (our_charset != charset_attribute->value()) {
        return false;  // early return!
      }
    }
  }
  our_charset.CopyToString(charset);
  return true;
}

bool CssFilter::GetApplicableMedia(const HtmlElement* element,
                                   StringVector* media) const {
  bool result = false;
  if (element != NULL) {
    const HtmlElement::Attribute* media_attribute =
        element->FindAttribute(HtmlName::kMedia);
    if (media_attribute != NULL) {
      css_util::VectorizeMediaAttribute(media_attribute->value(), media);
      result = true;
    }
  }
  return result;
}

// Return value answers the question: May we rewrite?
// css_base_gurl is the URL used to resolve relative URLs in the CSS.
// css_trim_gurl is the URL used to trim absolute URLs to relative URLs.
// Specifically, it should be the address of the CSS document itself for
// external CSS or the HTML document that the CSS is in for inline CSS.
// The expiry of the answer is the minimum of the expiries of all subresources
// in the stylesheet, or kint64max if there are none or the sheet is invalid.
TimedBool CssFilter::RewriteCssText(Context* context,
                                    const GoogleUrl& css_base_gurl,
                                    const GoogleUrl& css_trim_gurl,
                                    const StringPiece& in_text,
                                    int64 in_text_size,
                                    bool text_is_declarations,
                                    MessageHandler* handler) {
  // Load stylesheet w/o expanding background attributes and preserving as
  // much content as possible from the original document.
  Css::Parser parser(in_text);
  parser.set_preservation_mode(true);
  // If we think this is XHTML, turn off quirks-mode so that we don't "fix"
  // things we shouldn't.
  // TODO(sligocki): We might need to do this in other cases too.
  // TODO(nikhilmadan): For ajax rewrites, be conservative and assume its XHTML.
  // Is this right?
  if ((context != NULL && context->has_parent()) ||
      driver_->doctype().IsXhtml()) {
    parser.set_quirks_mode(false);
  }
  // Create a stylesheet even if given declarations so that we don't need
  // two versions of everything, though they do need to handle a stylesheet
  // with no selectors in it, which they currently do.
  scoped_ptr<Css::Stylesheet> stylesheet(NULL);
  if (text_is_declarations) {
    Css::Declarations* declarations = parser.ParseRawDeclarations();
    if (declarations != NULL) {
      stylesheet.reset(new Css::Stylesheet());
      Css::Ruleset* ruleset = new Css::Ruleset();
      stylesheet->mutable_rulesets().push_back(ruleset);
      ruleset->set_declarations(declarations);
    }
  } else {
    stylesheet.reset(parser.ParseRawStylesheet());
  }

  TimedBool ret = {kint64max, true};
  if (stylesheet.get() == NULL ||
      parser.errors_seen_mask() != Css::Parser::kNoError) {
    ret.value = false;
    driver_->InfoAt(context, "CSS parsing error in %s",
                    css_base_gurl.spec_c_str());
    num_parse_failures_->Add(1);
    // TODO(sligocki): Pass in some meaningful error message and pass an
    // error for each component of the error mask (not just the mask).
    // NOTE: We cast int64->int32 for the warning code. This should be safe
    // for now since errors_seen_mask maxes out at 1 << 16, but we should
    // translate the error bit into a number (1 << 13 -> 13) when we report
    // each error as it happens.
    driver_->resource_manager()->usage_data_reporter()->ReportWarning(
        css_base_gurl, static_cast<int32>(parser.errors_seen_mask()), "");
  } else {
    // Edit stylesheet.
    // Any problem with an @import results in the error mask bit kImportError
    // being set, so if we get here we know that any @import rules were parsed
    // successfully, thus, flattening is safe.
    context->RewriteCssFromRoot(in_text, in_text_size, stylesheet.release());
    // Rewrite OK thus far.
    ret.value = true;
  }
  return ret;
}

bool CssFilter::SerializeCss(RewriteContext* context,
                             int64 in_text_size,
                             const Css::Stylesheet* stylesheet,
                             const GoogleUrl& css_base_gurl,
                             const GoogleUrl& css_trim_gurl,
                             bool previously_optimized,
                             bool stylesheet_is_declarations,
                             GoogleString* out_text,
                             MessageHandler* handler) {
  bool ret = true;

  // Re-serialize stylesheet.
  StringWriter writer(out_text);
  if (stylesheet_is_declarations) {
    CssMinify::Declarations(stylesheet->ruleset(0).declarations(),
                            &writer, handler);
  } else {
    CssMinify::Stylesheet(*stylesheet, &writer, handler);
  }

  // Get signed versions so that we can subtract them.
  int64 out_text_size = static_cast<int64>(out_text->size());
  int64 bytes_saved = in_text_size - out_text_size;

  if (!driver_->options()->always_rewrite_css()) {
    // Don't rewrite if we didn't edit it or make it any smaller.
    if (!previously_optimized && bytes_saved <= 0) {
      ret = false;
      driver_->InfoAt(context, "CSS parser increased size of CSS file %s by %s "
                      "bytes.", css_base_gurl.spec_c_str(),
                      Integer64ToString(-bytes_saved).c_str());
      num_rewrites_dropped_->Add(1);
    }
    // Don't rewrite if we blanked the CSS file. This is likely to be a parse
    // error unless the input was also blank.
    // TODO(sligocki): Don't error if in_text is all whitespace.
    if (out_text_size == 0 && in_text_size != 0) {
      ret = false;
      driver_->InfoAt(context, "CSS parsing error in %s",
                      css_base_gurl.spec_c_str());
      num_parse_failures_->Add(1);
    }
  }

  // Statistics
  if (ret) {
    driver_->InfoAt(context, "Successfully rewrote CSS file %s saving %s "
                    "bytes.", css_base_gurl.spec_c_str(),
                    Integer64ToString(bytes_saved).c_str());
    num_blocks_rewritten_->Add(1);
    total_bytes_saved_->Add(bytes_saved);
    // TODO(sligocki): Will this be misleading if we flatten @imports?
    total_original_bytes_->Add(in_text_size);
  }
  return ret;
}

CssFilter::Context* CssFilter::MakeContext(RewriteDriver* driver,
                                           RewriteContext* parent) {
  ResourceContext* resource_context = new ResourceContext;
  resource_context->set_inline_images(
      driver_->UserAgentSupportsImageInlining());
  resource_context->set_attempt_webp(driver_->UserAgentSupportsWebp());
  return new Context(this, driver, parent, cache_extender_,
                     image_rewrite_filter_, image_combiner_, resource_context);
}

RewriteContext* CssFilter::MakeRewriteContext() {
  return MakeContext(driver_, NULL);
}

const UrlSegmentEncoder* CssFilter::encoder() const {
  return &encoder_;
}

const UrlSegmentEncoder* CssFilter::Context::encoder() const {
  return filter_->encoder();
}

RewriteContext* CssFilter::MakeNestedRewriteContext(
    RewriteContext* parent, const ResourceSlotPtr& slot) {
  RewriteContext* context = MakeContext(NULL, parent);
  context->AddSlot(slot);
  return context;
}

RewriteContext* CssFilter::MakeNestedFlatteningContextInNewSlot(
    const ResourcePtr& resource, const GoogleString& location,
    CssFilter::Context* rewriter, RewriteContext* parent,
    CssHierarchy* hierarchy) {
  ResourceSlotPtr slot(new InlineCssSlot(resource, location));
  RewriteContext* context = new CssFlattenImportsContext(NULL /* driver */,
                                                         parent, rewriter,
                                                         hierarchy);
  context->AddSlot(slot);
  return context;
}

}  // namespace net_instaweb
