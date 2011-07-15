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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/rewriter/public/url_partnership.h"

#include <cstddef>
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/stl_util.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

UrlPartnership::UrlPartnership(const RewriteOptions* rewrite_options)
    : rewrite_options_(rewrite_options) {
}

UrlPartnership::UrlPartnership(const RewriteOptions* rewrite_options,
                               const GoogleUrl& original_request)
    : rewrite_options_(rewrite_options) {
  Reset(original_request);
}

UrlPartnership::~UrlPartnership() {
  STLDeleteElements(&url_vector_);
}

// Adds a URL to a combination.  If it can be legally added, consulting
// the DomainLawyer, then true is returned.  AddUrl cannot be called
// after Resolve (CHECK failure).
bool UrlPartnership::AddUrl(const StringPiece& untrimmed_resource_url,
                            MessageHandler* handler) {
  GoogleString resource_url, mapped_domain_name;
  bool ret = false;
  TrimWhitespace(untrimmed_resource_url, &resource_url);

  if (resource_url.empty()) {
    handler->Message(
        kInfo, "Cannot rewrite empty URL relative to %s",
        original_origin_and_path_.spec_c_str());
  } else if (!original_origin_and_path_.is_valid()) {
    handler->Message(
        kInfo, "Cannot rewrite %s relative to invalid url %s",
        resource_url.c_str(),
        original_origin_and_path_.spec_c_str());
  } else {
    // First resolve the original request to ensure that it is allowed by the
    // options.
    scoped_ptr<GoogleUrl> resolved_request(
        new GoogleUrl(original_origin_and_path_, resource_url));
    if (!resolved_request->is_valid()) {
      handler->Message(
          kInfo, "URL %s cannot be resolved relative to base URL %s",
          resource_url.c_str(),
          original_origin_and_path_.spec_c_str());
    } else if (!rewrite_options_->IsAllowed(resolved_request->Spec())) {
      handler->Message(kInfo,
                       "Rewriting URL %s is disallowed via configuration",
                       resolved_request->spec_c_str());
    } else if (rewrite_options_->domain_lawyer()->MapRequestToDomain(
        original_origin_and_path_, resource_url, &mapped_domain_name,
        resolved_request.get(), handler)) {
      if (url_vector_.empty()) {
        domain_.swap(mapped_domain_name);
        GoogleUrl domain_origin_gurl(domain_);
        GoogleUrl tmp(domain_origin_gurl,
                      original_origin_and_path_.PathAndLeaf());
        domain_gurl_.Swap(&tmp);

        ret = true;
      } else {
        ret = (domain_ == mapped_domain_name);
        if (ret && !rewrite_options_->combine_across_paths()) {
          ret = (ResolvedBase() == resolved_request->AllExceptLeaf());
        }
      }

      if (ret) {
        url_vector_.push_back(resolved_request.release());
        int index = url_vector_.size() - 1;
        IncrementalResolve(index);
      }
    }
  }
  return ret;
}

void UrlPartnership::RemoveLast() {
  CHECK(!url_vector_.empty());
  int last = url_vector_.size() - 1;
  delete url_vector_[last];
  url_vector_.resize(last);

  // Re-resolve the entire partnership in the absense of the influence of the
  // ex-partner, by re-adding the GURLs one at a time.
  common_components_.clear();
  for (int i = 0, n = url_vector_.size(); i < n; ++i) {
    IncrementalResolve(i);
  }
}

void UrlPartnership::Reset(const GoogleUrl& original_request) {
  STLDeleteElements(&url_vector_);
  url_vector_.clear();
  common_components_.clear();
  if (original_request.is_valid()) {
    GoogleUrl tmp(original_request.AllExceptLeaf());
    original_origin_and_path_.Swap(&tmp);
  }
}

void UrlPartnership::IncrementalResolve(int index) {
  CHECK_LE(0, index);
  CHECK_LT(index, static_cast<int>(url_vector_.size()));

  // When tokenizing a URL, we don't want to omit empty segments
  // because we need to avoid aliasing "http://x" with "/http:/x".
  bool omit_empty = false;
  StringPieceVector components;

  if (index == 0) {
    StringPiece base = url_vector_[0]->AllExceptLeaf();
    SplitStringPieceToVector(base, "/", &components, omit_empty);
    components.pop_back();            // base ends with "/"
    CHECK_LE(3U, components.size());  // expect {"http:", "", "x"...}
    for (size_t i = 0; i < components.size(); ++i) {
      const StringPiece& sp = components[i];
      common_components_.push_back(GoogleString(sp.data(), sp.size()));
    }
  } else {
    // Split each string on / boundaries, then compare these path elements
    // until one doesn't match, then shortening common_components.
    StringPiece all_but_leaf = url_vector_[index]->AllExceptLeaf();
    SplitStringPieceToVector(all_but_leaf, "/", &components, omit_empty);
    components.pop_back();            // base ends with "/"
    CHECK_LE(3U, components.size());  // expect {"http:", "", "x"...}

    if (components.size() < common_components_.size()) {
      common_components_.resize(components.size());
    }
    for (size_t c = 0; c < common_components_.size(); ++c) {
      if (common_components_[c] != components[c]) {
        common_components_.resize(c);
        break;
      }
    }
  }
}

GoogleString UrlPartnership::ResolvedBase() const {
  GoogleString ret;
  if (!common_components_.empty()) {
    for (size_t c = 0; c < common_components_.size(); ++c) {
      const GoogleString& component = common_components_[c];
      ret += component;
      ret += "/";  // initial segment is "http" with no leading /
    }
  }
  return ret;
}

// Returns the relative path of a particular URL that was added into
// the partnership.  This requires that Resolve() be called first.
GoogleString UrlPartnership::RelativePath(int index) const {
  GoogleString resolved_base = ResolvedBase();
  StringPiece spec = url_vector_[index]->Spec();
  CHECK_GE(spec.size(), resolved_base.size());
  CHECK_EQ(StringPiece(spec.data(), resolved_base.size()),
           StringPiece(resolved_base));
  return GoogleString(spec.data() + resolved_base.size(),
                      spec.size() - resolved_base.size());
}

}  // namespace net_instaweb
