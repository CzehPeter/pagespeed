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

#include "net/instaweb/rewriter/public/simple_text_filter.h"

#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_single_resource_filter.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/string.h"

namespace net_instaweb {

class MessageHandler;

SimpleTextFilter::Rewriter::~Rewriter() {
}

SimpleTextFilter::SimpleTextFilter(Rewriter* rewriter, RewriteDriver* driver)
    : RewriteFilter(driver),
      rewriter_(rewriter) {
}

SimpleTextFilter::~SimpleTextFilter() {
}

SimpleTextFilter::Context::Context(const RewriterPtr& rewriter,
                                   RewriteDriver* driver,
                                   RewriteContext* parent)
    : SingleRewriteContext(driver, parent, NULL),
      rewriter_(rewriter) {
}

SimpleTextFilter::Context::~Context() {
}

void SimpleTextFilter::Context::RewriteSingle(const ResourcePtr& input,
                                              const OutputResourcePtr& output) {
  RewriteSingleResourceFilter::RewriteResult result =
      RewriteSingleResourceFilter::kRewriteFailed;
  GoogleString rewritten;
  ResourceManager* resource_manager = Manager();
  if (rewriter_->RewriteText(input->url(), input->contents(), &rewritten,
                             resource_manager))  {
    MessageHandler* message_handler = resource_manager->message_handler();
    int64 origin_expire_time_ms = input->CacheExpirationTimeMs();
    if (resource_manager->Write(HttpStatus::kOK, rewritten, output.get(),
                                origin_expire_time_ms, message_handler)) {
      result = RewriteSingleResourceFilter::kRewriteOk;
    }
  }
  RewriteDone(result, 0);
}

void SimpleTextFilter::StartElementImpl(HtmlElement* element) {
  HtmlElement::Attribute* attr = rewriter_->FindResourceAttribute(element);
  if (attr != NULL) {
    ResourcePtr resource = CreateInputResource(attr->value());
    if (resource.get() != NULL) {
      ResourceSlotPtr slot(driver_->GetSlot(resource, element, attr));

      // This 'new' is paired with a delete in RewriteContext::FinishFetch()
      Context* context = new Context(rewriter_, driver_, NULL);
      context->AddSlot(slot);
      driver_->InitiateRewrite(context);
    }
  }
}

RewriteContext* SimpleTextFilter::MakeRewriteContext() {
  return new Context(rewriter_, driver_, NULL);
}

RewriteContext* SimpleTextFilter::MakeNestedRewriteContext(
    RewriteContext* parent, const ResourceSlotPtr& slot) {
  RewriteContext* context = new Context(rewriter_, NULL, parent);
  context->AddSlot(slot);
  return context;
}

}  // namespace net_instaweb
