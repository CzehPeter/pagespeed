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

// Author: morlovich@google.com (Maksim Orlovich)

#include "net/instaweb/rewriter/public/inline_rewrite_context.h"

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/common_filter.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_result.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

InlineRewriteContext::InlineRewriteContext(CommonFilter* filter,
                                           HtmlElement* element,
                                           HtmlElement::Attribute* src)
    : RewriteContext(filter->driver(), NULL, NULL),
      filter_(filter),
      element_(element),
      src_(src) {
}

InlineRewriteContext::~InlineRewriteContext() {
}

bool InlineRewriteContext::StartInlining() {
  RewriteDriver* driver = filter_->driver();
  ResourcePtr input_resource(filter_->CreateInputResource(
      src_->DecodedValueOrNull()));
  if (input_resource.get() != NULL) {
    ResourceSlotPtr slot(driver->GetSlot(input_resource, element_, src_));
    AddSlot(slot);
    driver->InitiateRewrite(this);
    return true;
  } else {
    delete this;
    return false;
  }
}

bool InlineRewriteContext::Partition(OutputPartitions* partitions,
                                     OutputResourceVector* outputs) {
  CHECK_EQ(1, num_slots()) << "InlineRewriteContext only handles one slot";
  ResourcePtr resource(slot(0)->resource());
  if (resource->IsValidAndCacheable() && ShouldInline(resource)) {
    CachedResult* partition = partitions->add_partition();
    resource->AddInputInfoToPartition(Resource::kOmitInputHash, 0, partition);
    partition->set_inlined_data(resource->contents().as_string());
    outputs->push_back(OutputResourcePtr(NULL));
  }
  // If we don't inline, or resource is invalid, we write out an empty partition
  // table, making us do nothing.
  return true;
}

void InlineRewriteContext::Rewrite(int partition_index,
                                   CachedResult* partition,
                                   const OutputResourcePtr& output_resource) {
  CHECK(output_resource.get() == NULL);
  CHECK_EQ(0, partition_index);

  // We signal as rewrite failed, as we do not create an output resource.
  RewriteDone(kRewriteFailed, 0);
}

void InlineRewriteContext::Render() {
  if (num_output_partitions() == 1 && !slot(0)->should_delete_element()) {
    // We've decided to inline...
    slot(0)->set_disable_rendering(true);
    ResourceSlotPtr our_slot = slot(0);
    RenderInline(
        our_slot->resource(), output_partition(0)->inlined_data(), element_);
  }
}

// We never create output resources, so methods related to them are stubbed.
OutputResourceKind InlineRewriteContext::kind() const {
  LOG(FATAL);
  return kRewrittenResource;
}

}  // namespace net_instaweb
