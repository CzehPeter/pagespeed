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

// Author: nikhilmadan@google.com (Nikhil Madan)

#include "net/instaweb/rewriter/public/ajax_rewrite_context.h"

#include <algorithm>

#include "base/logging.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/cache_url_async_fetcher.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_filter.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_result.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/proto_util.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/timer.h"

namespace net_instaweb {

class MessageHandler;

// Names for Statistics variables.
const char AjaxRewriteContext::kInPlaceOversizedOptStream[] =
    "in_place_oversized_opt_stream";

AjaxRewriteResourceSlot::AjaxRewriteResourceSlot(const ResourcePtr& resource)
    : ResourceSlot(resource) {}

AjaxRewriteResourceSlot::~AjaxRewriteResourceSlot() {}

void AjaxRewriteResourceSlot::Render() {
  // Do nothing.
}

RecordingFetch::RecordingFetch(AsyncFetch* async_fetch,
                               const ResourcePtr& resource,
                               AjaxRewriteContext* context,
                               MessageHandler* handler)
    : SharedAsyncFetch(async_fetch),
      handler_(handler),
      resource_(resource),
      context_(context),
      can_ajax_rewrite_(false),
      streaming_(true),
      cache_value_writer_(&cache_value_,
                          context_->FindServerContext()->http_cache()) {
  Statistics* stats = context->FindServerContext()->statistics();
  in_place_oversized_opt_stream_ =
      stats->GetVariable(AjaxRewriteContext::kInPlaceOversizedOptStream);
}

RecordingFetch::~RecordingFetch() {}

void RecordingFetch::HandleHeadersComplete() {
  can_ajax_rewrite_ = CanAjaxRewrite();
  streaming_ = ShouldStream();
  if (can_ajax_rewrite_) {
    // Save the headers, and wait to finalize them in HandleDone().
    saved_headers_.CopyFrom(*response_headers());
    if (streaming_) {
      base_fetch()->HeadersComplete();
    }
  } else {
    FreeDriver();
    base_fetch()->HeadersComplete();
  }
}

void RecordingFetch::FreeDriver() {
  // This cleans up the context and frees the driver. Leaving this context
  // around causes problems in the html flow in particular.
  context_->driver_->FetchComplete();
}

bool RecordingFetch::ShouldStream() {
  return !can_ajax_rewrite_
      || !context_->Options()->in_place_wait_for_optimized();
}

bool RecordingFetch::HandleWrite(const StringPiece& content,
                                 MessageHandler* handler) {
  bool result = true;
  if (streaming_) {
    result = base_fetch()->Write(content, handler);
  }
  if (can_ajax_rewrite_) {
    if (cache_value_writer_.CanCacheContent(content)) {
      result &= cache_value_writer_.Write(content, handler);
      DCHECK(cache_value_writer_.has_buffered());
    } else {
      // Cannot ajax rewrite a resource which is too big to fit in cache.
      // TODO(jkarlin): Do we make note that the resource was too big so that
      // we don't try to cache it later? Test and fix if not.
      can_ajax_rewrite_ = false;
      if (!streaming_) {
        // We need to start streaming now so write out what we've cached so far.
        streaming_ = true;
        in_place_oversized_opt_stream_->Add(1);
        base_fetch()->HeadersComplete();
        StringPiece cache_contents;
        cache_value_.ExtractContents(&cache_contents);
        base_fetch()->Write(cache_contents, handler);
        base_fetch()->Write(content, handler);
      }
      FreeDriver();
    }
  }
  return result;
}

bool RecordingFetch::HandleFlush(MessageHandler* handler) {
  if (streaming_) {
    return base_fetch()->Flush(handler);
  }
  return true;
}

void RecordingFetch::HandleDone(bool success) {
  if (success && can_ajax_rewrite_) {
    // Extract X-Original-Content-Length from the response headers, which may
    // have been added by the fetcher, and set it in the Resource. This will
    // be used to build the X-Original-Content-Length for rewrites.
    const char* original_content_length_hdr = extra_response_headers()->Lookup1(
        HttpAttributes::kXOriginalContentLength);
    int64 ocl;
    if (original_content_length_hdr != NULL &&
        StringToInt64(original_content_length_hdr, &ocl)) {
      saved_headers_.SetOriginalContentLength(ocl);
    }
    // Now finalize the headers.
    cache_value_writer_.SetHeaders(&saved_headers_);
  }

  if (streaming_) {
    base_fetch()->Done(success);
  }

  if (success && can_ajax_rewrite_) {
    resource_->Link(&cache_value_, handler_);
    if (streaming_) {
      context_->DetachFetch();
    }
    context_->StartFetchReconstructionParent();
    if (streaming_) {
      context_->driver_->FetchComplete();
    }
  }
  delete this;
}

bool RecordingFetch::CanAjaxRewrite() {
  const ContentType* type = response_headers()->DetermineContentType();
  if (type == NULL) {
    return false;
  }
  // Note that this only checks the length, not the caching headers; the
  // latter are checked in IsAlreadyExpired.
  if (!cache_value_writer_.CheckCanCacheElseClear(response_headers())) {
    return false;
  }
  if (type->type() == ContentType::kCss ||
      type->type() == ContentType::kJavascript ||
      type->IsImage()) {
    if (!context_->driver_->server_context()->http_cache()->IsAlreadyExpired(
        request_headers(), *response_headers())) {
      return true;
    }
  }
  return false;
}

AjaxRewriteContext::AjaxRewriteContext(RewriteDriver* driver,
                                       const StringPiece& url)
    : SingleRewriteContext(driver, NULL, NULL),
      driver_(driver),
      url_(url.data(), url.size()),
      is_rewritten_(true) {
  set_notify_driver_on_fetch_done(true);
}

AjaxRewriteContext::~AjaxRewriteContext() {}

void AjaxRewriteContext::InitStats(Statistics* statistics) {
  statistics->AddVariable(kInPlaceOversizedOptStream);
}

void AjaxRewriteContext::Harvest() {
  if (num_nested() == 1) {
    RewriteContext* nested_context = nested(0);
    if (nested_context->num_slots() == 1) {
      ResourcePtr nested_resource = nested_context->slot(0)->resource();
      if (nested_context->slot(0)->was_optimized() &&
          num_output_partitions() == 1) {
        CachedResult* partition = output_partition(0);
        VLOG(1) << "Ajax rewrite succeeded for " << url_
                << " and the rewritten resource is "
                << nested_resource->url();
        partition->set_url(nested_resource->url());
        partition->set_optimizable(true);
        if (partitions()->other_dependency_size() == 1) {
          // If there is only one other dependency, then the InputInfo is
          // already covered in the first partition. We're clearing this here
          // since freshens only update the partitions and not the other
          // dependencies.
          partitions()->clear_other_dependency();
        }
        if (!FetchContextDetached() &&
             Options()->in_place_wait_for_optimized()) {
          // If we're waiting for the optimized version before responding,
          // prepare the output here. Most of this is translated from
          // RewriteContext::FetchContext::FetchDone
          output_resource_->response_headers()->CopyFrom(
              *(input_resource_->response_headers()));
          Writer* writer = output_resource_->BeginWrite(
              driver_->message_handler());
          writer->Write(nested_resource->contents(),
                        driver_->message_handler());
          output_resource_->EndWrite(driver_->message_handler());

          is_rewritten_ = true;
          // EndWrite updated the hash in output_resource_.
          output_resource_->full_name().hash().CopyToString(&rewritten_hash_);
          FixFetchFallbackHeaders(output_resource_->response_headers());

          // Use the most conservative Cache-Control considering the input.
          // TODO(jkarlin): Is ApplyInputCacheControl needed here?
          ResourceVector rv(1, input_resource_);
          FindServerContext()->ApplyInputCacheControl(
              rv, output_resource_->response_headers());
        }
        RewriteDone(kRewriteOk, 0);
        return;
      }
    }
  }
  VLOG(1) << "Ajax rewrite failed for " << url_;
  RewriteDone(kRewriteFailed, 0);
}

void AjaxRewriteContext::FetchTryFallback(const GoogleString& url,
                                          const StringPiece& hash) {
  const char* request_etag = async_fetch()->request_headers()->Lookup1(
      HttpAttributes::kIfNoneMatch);
  if (request_etag != NULL && !hash.empty() &&
      (StringPrintf(HTTPCache::kEtagFormat,
                    StrCat(id(), "-",  hash).c_str()) == request_etag)) {
    // Serve out a 304.
    async_fetch()->response_headers()->Clear();
    async_fetch()->response_headers()->SetStatusAndReason(
        HttpStatus::kNotModified);
    async_fetch()->Done(true);
    driver_->FetchComplete();
  } else {
    if (url == url_) {
      // If the fallback url is the same as the original url, no rewriting is
      // happening.
      is_rewritten_ = false;
      // TODO(nikhilmadan): RewriteContext::FetchTryFallback is going to look up
      // the cache. The fetcher may also do so. Should we just call
      // StartFetchReconstruction() here instead?
    } else {
      // Save the hash of the resource.
      rewritten_hash_ = hash.as_string();
    }
    RewriteContext::FetchTryFallback(url, hash);
  }
}

void AjaxRewriteContext::FixFetchFallbackHeaders(ResponseHeaders* headers) {
  if (is_rewritten_) {
    if (!rewritten_hash_.empty()) {
      headers->Replace(HttpAttributes::kEtag, StringPrintf(
          HTTPCache::kEtagFormat,
          StrCat(id(), "-", rewritten_hash_).c_str()));
    }

    headers->ComputeCaching();
    int64 expire_at_ms = kint64max;
    int64 date_ms = kint64max;
    if (partitions()->other_dependency_size() > 0) {
      UpdateDateAndExpiry(partitions()->other_dependency(), &date_ms,
                          &expire_at_ms);
    } else {
      UpdateDateAndExpiry(output_partition(0)->input(), &date_ms,
                          &expire_at_ms);
    }
    int64 now_ms = FindServerContext()->timer()->NowMs();
    if (expire_at_ms == kint64max) {
      // If expire_at_ms is not set, set the cache ttl to the implicit ttl value
      // specified in the response headers.
      expire_at_ms = now_ms + headers->implicit_cache_ttl_ms();
    } else if (stale_rewrite()) {
      // If we are serving a stale rewrite, set the cache ttl to the minimum of
      // kImplicitCacheTtlMs and the original ttl.
      expire_at_ms = now_ms + std::min(ResponseHeaders::kImplicitCacheTtlMs,
                                       expire_at_ms - date_ms);
    }
    headers->SetDateAndCaching(now_ms, expire_at_ms - now_ms);
  }
}

void AjaxRewriteContext::UpdateDateAndExpiry(
    const protobuf::RepeatedPtrField<InputInfo>& inputs,
    int64* date_ms,
    int64* expire_at_ms) {
  for (int j = 0, m = inputs.size(); j < m; ++j) {
    const InputInfo& dependency = inputs.Get(j);
    if (dependency.has_expiration_time_ms() && dependency.has_date_ms()) {
      *date_ms = std::min(*date_ms, dependency.date_ms());
      *expire_at_ms = std::min(*expire_at_ms, dependency.expiration_time_ms());
    }
  }
}

void AjaxRewriteContext::FetchCallbackDone(bool success) {
  if (is_rewritten_ && num_output_partitions() == 1) {
    // Ajax rewrites always have a single output partition.
    // Freshen the resource if possible. Note that since is_rewritten_ is true,
    // we got a metadata cache hit and a hit on the rewritten resource in cache.
    // TODO(nikhilmadan): Freshening is broken for ajax rewrites on css, since
    // we don't update the other dependencies.
    Freshen();
  }
  RewriteContext::FetchCallbackDone(success);
}

RewriteFilter* AjaxRewriteContext::GetRewriteFilter(
    const ContentType& type) {
  const RewriteOptions* options = driver_->options();
  if (type.type() == ContentType::kCss &&
      options->Enabled(RewriteOptions::kRewriteCss)) {
    return driver_->FindFilter(RewriteOptions::kCssFilterId);
  }
  if (type.type() == ContentType::kJavascript &&
      options->Enabled(RewriteOptions::kRewriteJavascript)) {
    return driver_->FindFilter(RewriteOptions::kJavascriptMinId);
  }
  if (type.IsImage() && options->ImageOptimizationEnabled()) {
    // TODO(nikhilmadan): This converts one image format to another. We
    // shouldn't do inter-conversion since we can't change the file extension.
    return driver_->FindFilter(RewriteOptions::kImageCompressionId);
  }
  return NULL;
}

void AjaxRewriteContext::RewriteSingle(const ResourcePtr& input,
                                       const OutputResourcePtr& output) {
  input_resource_ = input;
  output_resource_ = output;
  input->DetermineContentType();
  if (input->IsValidAndCacheable() && input->type() != NULL) {
    const ContentType* type = input->type();
    RewriteFilter* filter = GetRewriteFilter(*type);
    if (filter != NULL) {
      ResourceSlotPtr ajax_slot(
          new AjaxRewriteResourceSlot(slot(0)->resource()));
      RewriteContext* context = filter->MakeNestedRewriteContext(
          this, ajax_slot);
      if (context != NULL) {
        AddNestedContext(context);
        if (!is_rewritten_ && !rewritten_hash_.empty()) {
          // The ajax metadata was found but the rewritten resource is not.
          // Hence, make the nested rewrite skip the metadata and force a
          // rewrite.
          context->set_force_rewrite(true);
        }
        StartNestedTasks();
        return;
      } else {
        LOG(ERROR) << "Filter (" << filter->id() << ") does not support "
                   << "nested contexts.";
        ajax_slot.clear();
      }
    }
  }
  // Give up on the rewrite.
  RewriteDone(kRewriteFailed, 0);
  // TODO(nikhilmadan): If the resource is not cacheable, cache this in the
  // metadata so that the fetcher can skip reading from the cache.
}

bool AjaxRewriteContext::DecodeFetchUrls(
    const OutputResourcePtr& output_resource,
    MessageHandler* message_handler,
    GoogleUrlStarVector* url_vector) {
  GoogleUrl* url = new GoogleUrl(url_);
  url_vector->push_back(url);
  return true;
}

void AjaxRewriteContext::StartFetchReconstruction() {
  // The ajax metdata or the rewritten resource was not found in cache. Fetch
  // the original resource and trigger an asynchronous rewrite.
  if (num_slots() == 1) {
    ResourcePtr resource(slot(0)->resource());
    // If we get here, the resource must not have been rewritten.
    is_rewritten_ = false;
    RecordingFetch* fetch = new RecordingFetch(
        async_fetch(), resource, this, fetch_message_handler());
    cache_fetcher_.reset(driver_->CreateCacheFetcher());
    cache_fetcher_->Fetch(url_, fetch_message_handler(), fetch);
  } else {
    LOG(ERROR) << "Expected one resource slot, but found " << num_slots()
               << ".";
    delete this;
  }
}

void AjaxRewriteContext::StartFetchReconstructionParent() {
  RewriteContext::StartFetchReconstruction();
}
}  // namespace net_instaweb
