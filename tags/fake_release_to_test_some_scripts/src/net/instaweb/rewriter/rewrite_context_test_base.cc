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

// Base-class & helper classes for testing RewriteContext and its
// interaction with various subsystems.

#include "net/instaweb/rewriter/public/rewrite_context_test_base.h"
#include "net/instaweb/util/public/stl_util.h"

namespace net_instaweb {

const char TrimWhitespaceRewriter::kFilterId[] = "tw";
const char TrimWhitespaceSyncFilter::kFilterId[] = "ts";
const char UpperCaseRewriter::kFilterId[] = "uc";
const char NestedFilter::kFilterId[] = "nf";
const char CombiningFilter::kFilterId[] = "cr";

TrimWhitespaceRewriter::~TrimWhitespaceRewriter() {
}

bool TrimWhitespaceRewriter::RewriteText(const StringPiece& url,
                                         const StringPiece& in,
                                         GoogleString* out,
                                         ServerContext* resource_manager) {
  LOG(INFO) << "Trimming whitespace.";
  ++num_rewrites_;
  TrimWhitespace(in, out);
  return in != *out;
}

HtmlElement::Attribute* TrimWhitespaceRewriter::FindResourceAttribute(
    HtmlElement* element) {
  if (element->keyword() == HtmlName::kLink) {
    return element->FindAttribute(HtmlName::kHref);
  }
  return NULL;
}

TrimWhitespaceSyncFilter::~TrimWhitespaceSyncFilter() {
}

void TrimWhitespaceSyncFilter::StartElementImpl(HtmlElement* element) {
  if (element->keyword() == HtmlName::kLink) {
    HtmlElement::Attribute* href = element->FindAttribute(HtmlName::kHref);
    if (href != NULL) {
      GoogleUrl gurl(driver()->google_url(), href->DecodedValueOrNull());
      href->SetValue(StrCat(gurl.Spec(), ".pagespeed.ts.0.css"));
    }
  }
}

UpperCaseRewriter::~UpperCaseRewriter() {
}

NestedFilter::~NestedFilter() {
}

NestedFilter::Context::~Context() {
  STLDeleteElements(&strings_);
}

void NestedFilter::Context::RewriteSingle(
    const ResourcePtr& input, const OutputResourcePtr& output) {
  ++filter_->num_top_rewrites_;
  // Assume that this file just has nested CSS URLs one per line,
  // which we will rewrite.
  StringPieceVector pieces;
  SplitStringPieceToVector(input->contents(), "\n", &pieces, true);

  GoogleUrl base(input->url());
  if (base.is_valid()) {
    // Add a new nested multi-slot context.
    for (int i = 0, n = pieces.size(); i < n; ++i) {
      GoogleUrl url(base, pieces[i]);
      if (url.is_valid()) {
        ResourcePtr resource(Driver()->CreateInputResource(url));
        if (resource.get() != NULL) {
          ResourceSlotPtr slot(new NestedSlot(resource));
          RewriteContext* nested_context =
              filter_->upper_filter()->MakeNestedRewriteContext(this, slot);
          AddNestedContext(nested_context);
          nested_slots_.push_back(slot);

          // Test chaining of a 2nd rewrite on the same slot, if asked.
          if (chain_) {
            RewriteContext* nested_context2 =
                filter_->upper_filter()->MakeNestedRewriteContext(this,
                                                                  slot);
            AddNestedContext(nested_context2);
          }
        }
      }
    }
    // TODO(jmarantz): start this automatically.  This will be easier
    // to do once the states are kept more explicitly via a refactor.
    StartNestedTasks();
  }
}

void NestedFilter::Context::Harvest() {
  RewriteResult result = kRewriteFailed;
  GoogleString new_content;

  if (filter_->check_nested_rewrite_result_) {
    for (int i = 0, n = nested_slots_.size(); i < n; ++i) {
      EXPECT_EQ(filter_->expected_nested_rewrite_result(),
                nested_slots_[i]->was_optimized());
    }
  }

  CHECK_EQ(1, num_slots());
  for (int i = 0, n = num_nested(); i < n; ++i) {
    CHECK_EQ(1, nested(i)->num_slots());
    ResourceSlotPtr slot(nested(i)->slot(0));
    ResourcePtr resource(slot->resource());
    StrAppend(&new_content, resource->url(), "\n");
  }
  ServerContext* resource_manager = FindServerContext();
  MessageHandler* message_handler = resource_manager->message_handler();
  // Warning: this uses input's content-type for simplicity, but real
  // filters should not do that --- see comments in
  // CacheExtender::RewriteLoadedResource as to why.
  if (resource_manager->Write(ResourceVector(1, slot(0)->resource()),
                              new_content,
                              slot(0)->resource()->type(),
                              slot(0)->resource()->charset(),
                              output(0).get(),
                              message_handler)) {
    result = kRewriteOk;
  }
  RewriteDone(result, 0);
}

void NestedFilter::StartElementImpl(HtmlElement* element) {
  HtmlElement::Attribute* attr = element->FindAttribute(HtmlName::kHref);
  if (attr != NULL) {
    ResourcePtr resource = CreateInputResource(attr->DecodedValueOrNull());
    if (resource.get() != NULL) {
      ResourceSlotPtr slot(driver_->GetSlot(resource, element, attr));

      // This 'new' is paired with a delete in RewriteContext::FinishFetch()
      Context* context = new Context(driver_, this, chain_);
      context->AddSlot(slot);
      driver_->InitiateRewrite(context);
    }
  }
}

CombiningFilter::CombiningFilter(RewriteDriver* driver,
                                 MockScheduler* scheduler,
                                 int64 rewrite_delay_ms)
    : RewriteFilter(driver),
      scheduler_(scheduler),
      rewrite_delay_ms_(rewrite_delay_ms),
      rewrite_block_on_(NULL),
      rewrite_signal_on_(NULL),
      on_the_fly_(false),
      optimization_only_(true),
      disable_successors_(false) {
  ClearStats();
}

CombiningFilter::~CombiningFilter() {
}

CombiningFilter::Context::Context(RewriteDriver* driver,
                                  CombiningFilter* filter,
                                  MockScheduler* scheduler)
    : RewriteContext(driver, NULL, NULL),
      combiner_(driver, filter),
      scheduler_(scheduler),
      time_at_start_of_rewrite_us_(scheduler_->timer()->NowUs()),
      filter_(filter) {
  combiner_.set_prefix(filter_->prefix_);
}

bool CombiningFilter::Context::Partition(OutputPartitions* partitions,
                                         OutputResourceVector* outputs) {
  MessageHandler* handler = Driver()->message_handler();
  CachedResult* partition = partitions->add_partition();
  for (int i = 0, n = num_slots(); i < n; ++i) {
    slot(i)->resource()->AddInputInfoToPartition(
        Resource::kIncludeInputHash, i, partition);
    if (!combiner_.AddResourceNoFetch(slot(i)->resource(), handler).value) {
      return false;
    }
  }
  OutputResourcePtr combination(combiner_.MakeOutput());

  // ResourceCombiner provides us with a pre-populated CachedResult,
  // so we need to copy it over to our CachedResult.  This is
  // less efficient than having ResourceCombiner work with our
  // cached_result directly but this allows code-sharing as we
  // transition to the async flow.
  combination->UpdateCachedResultPreservingInputInfo(partition);
  DisableRemovedSlots(partition);
  outputs->push_back(combination);
  return true;
}

void CombiningFilter::Context::Rewrite(int partition_index,
                                       CachedResult* partition,
                                       const OutputResourcePtr& output) {
  if (filter_->rewrite_signal_on_ != NULL) {
    filter_->rewrite_signal_on_->Notify();
  }
  if (filter_->rewrite_block_on_ != NULL) {
    filter_->rewrite_block_on_->Wait();
  }
  if (filter_->rewrite_delay_ms() == 0) {
    DoRewrite(partition_index, partition, output);
  } else {
    int64 wakeup_us = time_at_start_of_rewrite_us_ +
        1000 * filter_->rewrite_delay_ms();
    Function* closure = MakeFunction(
        this, &Context::DoRewrite, partition_index, partition, output);
    scheduler_->AddAlarm(wakeup_us, closure);
  }
}

void CombiningFilter::Context::DoRewrite(int partition_index,
                                         CachedResult* partition,
                                         OutputResourcePtr output) {
  ++filter_->num_rewrites_;
  // resource_combiner.cc takes calls WriteCombination as part
  // of Combine.  But if we are being called on behalf of a
  // fetch then the resource still needs to be written.
  RewriteResult result = kRewriteOk;
  if (!output->IsWritten()) {
    ResourceVector resources;
    for (int i = 0, n = num_slots(); i < n; ++i) {
      ResourcePtr resource(slot(i)->resource());
      resources.push_back(resource);
    }
    if (!combiner_.Write(resources, output)) {
      result = kRewriteFailed;
    }
  }
  RewriteDone(result, partition_index);
}

void CombiningFilter::Context::Render() {
  // Slot 0 will be replaced by the combined resource as part of
  // rewrite_context.cc.  But we still need to delete slots 1-N.
  for (int p = 0, np = num_output_partitions(); p < np; ++p) {
    DisableRemovedSlots(output_partition(p));
  }
}

void CombiningFilter::Context::DisableRemovedSlots(CachedResult* partition) {
  if (filter_->disable_successors_) {
    slot(0)->set_disable_further_processing(true);
  }
  for (int i = 1; i < partition->input_size(); ++i) {
    int slot_index = partition->input(i).index();
    slot(slot_index)->RequestDeleteElement();
  }
}

void CombiningFilter::StartElementImpl(HtmlElement* element) {
  if (element->keyword() == HtmlName::kLink) {
    HtmlElement::Attribute* href = element->FindAttribute(HtmlName::kHref);
    if (href != NULL) {
      ResourcePtr resource(CreateInputResource(href->DecodedValueOrNull()));
      if (resource.get() != NULL) {
        if (context_.get() == NULL) {
          context_.reset(new Context(driver_, this, scheduler_));
        }
        context_->AddElement(element, href, resource);
      }
    }
  }
}


RewriteContextTestBase::~RewriteContextTestBase() {
}

void RewriteContextTestBase::SetUp() {
  trim_filter_ = NULL;
  other_trim_filter_ = NULL;
  combining_filter_ = NULL;
  nested_filter_ = NULL;
  logging_info_ = log_record_.logging_info();

  RewriteTestBase::SetUp();

  // The default deadline set in RewriteDriver is dependent on whether
  // the system was compiled for debug, or is being run under valgrind.
  // However, the unit-tests here use mock-time so we want to set the
  // deadline explicitly.
  rewrite_driver()->set_rewrite_deadline_ms(kRewriteDeadlineMs);
  other_rewrite_driver()->set_rewrite_deadline_ms(kRewriteDeadlineMs);
}

void RewriteContextTestBase::TearDown() {
  rewrite_driver()->WaitForShutDown();
  RewriteTestBase::TearDown();
}

void RewriteContextTestBase::InitResourcesToDomain(const char* domain) {
  ResponseHeaders default_css_header;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
  int64 now_ms = http_cache()->timer()->NowMs();
  default_css_header.SetDateAndCaching(now_ms, kOriginTtlMs);
  default_css_header.ComputeCaching();

  // trimmable
  SetFetchResponse(StrCat(domain, "a.css"), default_css_header, " a ");

  // not trimmable
  SetFetchResponse(StrCat(domain, "b.css"), default_css_header, "b");
  SetFetchResponse(StrCat(domain, "c.css"), default_css_header,
                   "a.css\nb.css\n");

  // trimmable, with charset.
  ResponseHeaders encoded_css_header;
  server_context()->SetDefaultLongCacheHeadersWithCharset(
      &kContentTypeCss, "koi8-r", &encoded_css_header);
  SetFetchResponse(StrCat(domain, "a_ru.css"), encoded_css_header,
                   " a = \xc1 ");

  // trimmable, private
  ResponseHeaders private_css_header;
  now_ms = http_cache()->timer()->NowMs();
  private_css_header.set_major_version(1);
  private_css_header.set_minor_version(1);
  private_css_header.SetStatusAndReason(HttpStatus::kOK);
  private_css_header.SetDateAndCaching(now_ms, kOriginTtlMs, ",private");
  private_css_header.ComputeCaching();

  SetFetchResponse(StrCat(domain, "a_private.css"),
                   private_css_header,
                   " a ");

  // trimmable, no-cache
  ResponseHeaders no_cache_css_header;
  now_ms = http_cache()->timer()->NowMs();
  no_cache_css_header.set_major_version(1);
  no_cache_css_header.set_minor_version(1);
  no_cache_css_header.SetStatusAndReason(HttpStatus::kOK);
  no_cache_css_header.SetDateAndCaching(now_ms, 0, ",no-cache");
  no_cache_css_header.ComputeCaching();

  SetFetchResponse(StrCat(domain, "a_no_cache.css"),
                   no_cache_css_header,
                   " a ");

  // trimmable, no-cache, no-store
  ResponseHeaders no_store_css_header;
  now_ms = http_cache()->timer()->NowMs();
  no_store_css_header.set_major_version(1);
  no_store_css_header.set_minor_version(1);
  no_store_css_header.SetStatusAndReason(HttpStatus::kOK);
  no_store_css_header.SetDateAndCaching(now_ms, 0, ",no-cache,no-store");
  no_store_css_header.ComputeCaching();

  SetFetchResponse(StrCat(domain, "a_no_store.css"),
                   no_store_css_header,
                   " a ");
}

void RewriteContextTestBase::InitUpperFilter(OutputResourceKind kind,
                                             RewriteDriver* rewrite_driver) {
  UpperCaseRewriter* rewriter;
  rewrite_driver->AppendRewriteFilter(
      UpperCaseRewriter::MakeFilter(kind, rewrite_driver, &rewriter));
}

void RewriteContextTestBase::InitCombiningFilter(int64 rewrite_delay_ms) {
  RewriteDriver* driver = rewrite_driver();
  combining_filter_ = new CombiningFilter(driver, mock_scheduler(),
                                          rewrite_delay_ms);
  driver->AppendRewriteFilter(combining_filter_);
  driver->AddFilters();
}

void RewriteContextTestBase::InitNestedFilter(
    bool expected_nested_rewrite_result) {
  RewriteDriver* driver = rewrite_driver();

  // Note that we only register this instance for rewrites, not HTML
  // handling, so that uppercasing doesn't end up messing things up before
  // NestedFilter gets to them.
  UpperCaseRewriter* upper_rewriter;
  SimpleTextFilter* upper_filter =
      UpperCaseRewriter::MakeFilter(kOnTheFlyResource, driver,
                                    &upper_rewriter);
  AddFetchOnlyRewriteFilter(upper_filter);
  nested_filter_ = new NestedFilter(driver, upper_filter, upper_rewriter,
                                    expected_nested_rewrite_result);
  driver->AppendRewriteFilter(nested_filter_);
  driver->AddFilters();
}

void RewriteContextTestBase::InitTrimFilters(OutputResourceKind kind) {
  trim_filter_ = new TrimWhitespaceRewriter(kind);
  rewrite_driver()->AppendRewriteFilter(
      new SimpleTextFilter(trim_filter_, rewrite_driver()));
  rewrite_driver()->AddFilters();

  other_trim_filter_ = new TrimWhitespaceRewriter(kind);
  other_rewrite_driver()->AppendRewriteFilter(
      new SimpleTextFilter(other_trim_filter_, other_rewrite_driver()));
  other_rewrite_driver()->AddFilters();
}

void RewriteContextTestBase::ClearStats() {
  RewriteTestBase::ClearStats();
  if (trim_filter_ != NULL) {
    trim_filter_->ClearStats();
  }
  if (other_trim_filter_ != NULL) {
    other_trim_filter_->ClearStats();
  }
  if (combining_filter_ != NULL) {
    combining_filter_->ClearStats();
  }
  if (nested_filter_ != NULL) {
    nested_filter_->ClearStats();
  }
  log_record_.logging_info()->Clear();
}

}  // namespace net_instaweb
