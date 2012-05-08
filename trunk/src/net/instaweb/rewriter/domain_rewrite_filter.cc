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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/rewriter/public/domain_rewrite_filter.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_hash.h"
#include "net/instaweb/util/public/string_util.h"

namespace {

// Names for Statistics variables.
const char kDomainRewrites[] = "domain_rewrites";

}  // namespace

namespace net_instaweb {

DomainRewriteFilter::DomainRewriteFilter(RewriteDriver* rewrite_driver,
                                         Statistics *stats)
    : CommonFilter(rewrite_driver),
      tag_scanner_(rewrite_driver),
      rewrite_count_(stats->GetVariable(kDomainRewrites)) {}

void DomainRewriteFilter::StartDocumentImpl() {
  bool rewrite_hyperlinks = driver_->options()->domain_rewrite_hyperlinks();
  tag_scanner_.set_find_a_tags(rewrite_hyperlinks);
  tag_scanner_.set_find_form_tags(rewrite_hyperlinks);

  if (rewrite_hyperlinks) {
    // TODO(nikhilmadan): Rewrite the domain for cookies.
    // Rewrite the Location header for redirects.
    ResponseHeaders* headers = driver_->mutable_response_headers();
    if (headers != NULL) {
      const char* location = headers->Lookup1(HttpAttributes::kLocation);
      if (location != NULL) {
        GoogleString new_location;
        Rewrite(location, driver_->base_url(), false /* !apply_sharding */,
                &new_location);
        headers->Replace(HttpAttributes::kLocation, new_location);
      }
    }
  }
}

DomainRewriteFilter::~DomainRewriteFilter() {}

void DomainRewriteFilter::Initialize(Statistics* statistics) {
  statistics->AddVariable(kDomainRewrites);
}

void DomainRewriteFilter::StartElementImpl(HtmlElement* element) {
  // Disable domain_rewrite for img if ModPagespeedDisableForBots is on
  // and the user-agent is a bot.
  if (element->keyword() == HtmlName::kImg &&
      driver_->ShouldNotRewriteImages()) {
    return;
  }
  bool is_hyperlink;
  HtmlElement::Attribute* attr = tag_scanner_.ScanElement(
      element, &is_hyperlink);
  if (attr != NULL) {
    StringPiece val(attr->DecodedValueOrNull());
    GoogleString rewritten_val;
    bool apply_sharding = !is_hyperlink;
    if (!val.empty() &&
        BaseUrlIsValid() &&
        (Rewrite(val, driver_->base_url(), apply_sharding, &rewritten_val) ==
         kRewroteDomain)) {
      attr->SetValue(rewritten_val);
      rewrite_count_->Add(1);
    }
  }
}

// Resolve the url we want to rewrite, and then shard as appropriate.
DomainRewriteFilter::RewriteResult DomainRewriteFilter::Rewrite(
    const StringPiece& url_to_rewrite, const GoogleUrl& base_url,
    bool apply_sharding, GoogleString* rewritten_url) {
  if (url_to_rewrite.empty()) {
    return kDomainUnchanged;
  }

  GoogleUrl orig_url(base_url, url_to_rewrite);
  if (!orig_url.is_valid()) {
    return kFail;
  }
  if (!orig_url.is_standard()) {
    // If the schemes are the same url_to_rewrite was -probably- relative,
    // so fail this rewrite since the absolute result can't be handled;
    // if they're different then it was definitely absolute and we should
    // just leave it as it was.
    if (orig_url.Scheme() == base_url.Scheme()) {
      return kFail;
    } else {
      orig_url.Spec().CopyToString(rewritten_url);
      return kDomainUnchanged;
    }
  }

  StringPiece orig_spec = orig_url.Spec();
  const RewriteOptions* options = driver_->options();

  if (!options->IsAllowed(orig_spec) ||
      // Don't rewrite a domain from an already-rewritten resource.
      resource_manager_->IsPagespeedResource(orig_url)) {
    // Even though domain is unchanged, we need to store absolute URL in
    // rewritten_url.
    orig_url.Spec().CopyToString(rewritten_url);
    return kDomainUnchanged;
  }

  // Apply any domain rewrites.
  //
  // TODO(jmarantz): There are two things going on: resolving URLs
  // against base and mapping them.  We should (a) factor those out
  // so they are distinct and (b) only do the resolution once, as it
  // is expensive.  I think the ResourceSlot system offers a good
  // framework to do this.
  const DomainLawyer* lawyer = options->domain_lawyer();
  GoogleString mapped_domain_name;
  GoogleUrl resolved_request;
  if (!lawyer->MapRequestToDomain(base_url, url_to_rewrite,
                                  &mapped_domain_name, &resolved_request,
                                  driver_->message_handler())) {
    // Even though domain is unchanged, we need to store absolute URL in
    // rewritten_url.
    orig_url.Spec().CopyToString(rewritten_url);
    return kDomainUnchanged;
  }

  // Next, apply any sharding.
  GoogleString sharded_domain;
  GoogleString domain = StrCat(resolved_request.Origin(), "/");
  resolved_request.Spec().CopyToString(rewritten_url);
  uint32 int_hash = HashString<CasePreserve, uint32>(
      rewritten_url->data(), rewritten_url->size());
  if (apply_sharding &&
      lawyer->ShardDomain(domain, int_hash, &sharded_domain)) {
    *rewritten_url = StrCat(sharded_domain,
                            resolved_request.PathAndLeaf().substr(1));
  }

  // Return true if really changed the url with this rewrite.
  if (orig_spec == *rewritten_url) {
    return kDomainUnchanged;
  } else {
    return kRewroteDomain;
  }
}

}  // namespace net_instaweb
