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

// Author: abliss@google.com (Joshua Marantz)

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_RESOURCE_COMBINER_TEMPLATE_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_RESOURCE_COMBINER_TEMPLATE_H_

#include <vector>

#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_combiner.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class MessageHandler;
class RewriteDriver;
class RewriteFilter;

// A templatized extension of ResourceCombiner that can track elements of a
// custom type.
template<typename T>
class ResourceCombinerTemplate : public ResourceCombiner {
 public:
  ResourceCombinerTemplate(RewriteDriver* rewrite_driver,
                           const StringPiece& extension,
                           RewriteFilter* filter)
      : ResourceCombiner(rewrite_driver, extension, filter) {
  }
  virtual ~ResourceCombinerTemplate() {
    // Note that the superclass's dtor will not call our overridden Clear.
    // Fortunately there's no harm in calling Clear() several times.
    Clear();
  }

  TimedBool AddElementNoFetch(T element, const ResourcePtr& resource,
                              MessageHandler* handler) {
    TimedBool result = AddResourceNoFetch(resource, handler);
    if (result.value) {
      elements_.push_back(element);
    }
    return result;
  }

  // Removes the last element that was added to this combiner, and the
  // corresponding resource.
  void RemoveLastElement() {
    RemoveLastResource();
    elements_.pop_back();
  }

  T element(int i) const { return elements_[i]; }

 protected:
  int num_elements() const { return elements_.size(); }

  virtual void Clear() {
    elements_.clear();
    ResourceCombiner::Clear();
  }

 private:
  friend class ResourceCombinerTest;

  // This is visible for ResourceCombinerTest, which uses this to
  // emulate an old blocking flow using the now-extinct ReadIfCached.
  // That test-suite is still useful, however, for pathname limitation
  // tests.
  void push_back_element(T element) { elements_.push_back(element); }

  std::vector<T> elements_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_RESOURCE_COMBINER_TEMPLATE_H_
