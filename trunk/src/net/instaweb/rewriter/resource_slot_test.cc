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

// Unit-test the resource slot comparator.

#include "net/instaweb/rewriter/public/resource_slot.h"

#include <set>
#include <utility>  // for std::pair

#include "base/scoped_ptr.h"            // for scoped_ptr

#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_writer_filter.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/rewriter/public/data_url_input_resource.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_manager_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"               // for StrCat

namespace {

static const char kHtmlUrl[] = "http://html.parse.test/event_list_test.html";
static const char kUpdatedUrl[] = "http://html.parse.test/new_css.css";

}  // namespace

namespace net_instaweb {

class ResourceSlotTest : public ResourceManagerTestBase {
 protected:
  typedef std::set<HtmlResourceSlotPtr, HtmlResourceSlotComparator> SlotSet;

  virtual bool AddBody() const { return false; }

  virtual void SetUp() {
    ResourceManagerTestBase::SetUp();

    // Set up 4 slots for testing.
    RewriteDriver* driver = rewrite_driver();
    ASSERT_TRUE(driver->StartParseId(kHtmlUrl, "resource_slot_test",
                                     kContentTypeHtml));
    elements_[0] = driver->NewElement(NULL, HtmlName::kLink);
    driver->AddAttribute(elements_[0], HtmlName::kHref, "v1");
    driver->AddAttribute(elements_[0], HtmlName::kSrc, "v2");
    elements_[1] = driver->NewElement(NULL, HtmlName::kLink);
    driver->AddAttribute(element(1), HtmlName::kHref, "v3");
    driver->AddAttribute(element(1), HtmlName::kSrc, "v4");

    driver->AddElement(element(0), 1);
    driver->CloseElement(element(0), HtmlElement::BRIEF_CLOSE, 1);
    driver->AddElement(element(1), 2);
    driver->CloseElement(element(1), HtmlElement::BRIEF_CLOSE, 3);

    slots_[0] = MakeSlot(0, 0);
    slots_[1] = MakeSlot(0, 1);
    slots_[2] = MakeSlot(1, 0);
    slots_[3] = MakeSlot(1, 1);
  }

  virtual void TearDown() {
    rewrite_driver()->FinishParse();
    ResourceManagerTestBase::TearDown();
  }

  HtmlResourceSlotPtr MakeSlot(int element_index, int attribute_index) {
    ResourcePtr empty;
    HtmlResourceSlot* slot = new HtmlResourceSlot(
        empty, element(element_index),
        attribute(element_index, attribute_index),
        html_parse());
    return HtmlResourceSlotPtr(slot);
  }

  bool InsertAndReturnTrueIfAdded(const HtmlResourceSlotPtr& slot) {
    std::pair<HtmlResourceSlotSet::iterator, bool> p = slot_set_.insert(slot);
    return p.second;
  }

  int num_slots() const { return slot_set_.size(); }
  const HtmlResourceSlotPtr slot(int index) const { return slots_[index]; }
  HtmlElement* element(int index) { return elements_[index]; }
  HtmlElement::Attribute* attribute(int element_index, int attribute_index) {
    HtmlElement* el = element(element_index);
    HtmlElement::AttributeList* attrs = el->mutable_attributes();
    int pos = 0;
    for (net_instaweb::HtmlElement::AttributeIterator i(attrs->begin());
         i != attrs->end(); ++i, ++pos) {
      if (pos == attribute_index) {
        return i.Get();
      }
    }
    return NULL;
  }

  GoogleString GetHtmlDomAsString() {
    output_buffer_.clear();
    html_parse()->ApplyFilter(html_writer_filter_.get());
    return output_buffer_;
  }

 private:
  HtmlResourceSlotSet slot_set_;
  HtmlResourceSlotPtr slots_[4];
  HtmlElement* elements_[2];
};

TEST_F(ResourceSlotTest, Accessors) {
  EXPECT_EQ(element(0), slot(0)->element());
  EXPECT_EQ(attribute(0, 0), slot(0)->attribute());
  EXPECT_EQ(element(0), slot(1)->element());
  EXPECT_EQ(attribute(0, 1), slot(1)->attribute());
  EXPECT_EQ(element(1), slot(2)->element());
  EXPECT_EQ(attribute(1, 0), slot(2)->attribute());
  EXPECT_EQ(element(1), slot(3)->element());
  EXPECT_EQ(attribute(1, 1), slot(3)->attribute());
  EXPECT_FALSE(slot(0)->was_optimized());
  slot(0)->set_was_optimized(true);
  EXPECT_TRUE(slot(0)->was_optimized());

  EXPECT_EQ("resource_slot_test:1", slot(0)->LocationString());
  EXPECT_EQ("resource_slot_test:2-3", slot(2)->LocationString());

  const char kDataUrl[] = "data:text/plain,Huh";
  ResourcePtr resource =
      DataUrlInputResource::Make(kDataUrl, resource_manager());
  ResourceSlotPtr fetch_slot(new FetchResourceSlot(resource));
  EXPECT_EQ(StrCat("Fetch of ", kDataUrl), fetch_slot->LocationString());
}

TEST_F(ResourceSlotTest, Comparator) {
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(InsertAndReturnTrueIfAdded(slot(i)));
  }
  EXPECT_EQ(4, num_slots());

  // Adding an equivalent slot should fail and leave the number of remembered
  // slots unchanged.
  ResourcePtr empty;
  HtmlResourceSlotPtr s4_dup(MakeSlot(1, 1));
  EXPECT_FALSE(InsertAndReturnTrueIfAdded(s4_dup))
      << "s4_dup is equivalent to slots_[3] so it should not add to the set";
  EXPECT_EQ(4, num_slots());
}

// Tests that a slot resource-update has the desired effect on the DOM.
TEST_F(ResourceSlotTest, RenderUpdate) {
  SetupWriter();
  GoogleUrl gurl(kUpdatedUrl);

  // Before update: first href=v1.
  EXPECT_EQ("<link href=\"v1\" src=\"v2\"/><link href=\"v3\" src=\"v4\"/>",
            GetHtmlDomAsString());

  ResourcePtr updated(rewrite_driver()->CreateInputResource(gurl));
  slot(0)->SetResource(updated);
  slot(0)->Render();

  // After update: first href=kUpdated.
  EXPECT_EQ(StrCat("<link href=\"", kUpdatedUrl,
                   "\" src=\"v2\"/><link href=\"v3\" src=\"v4\"/>"),
            GetHtmlDomAsString());
}

// Tests that a slot deletion takes effect as expected.
TEST_F(ResourceSlotTest, RenderDelete) {
  SetupWriter();

  // Before update: first link is present.
  EXPECT_EQ("<link href=\"v1\" src=\"v2\"/><link href=\"v3\" src=\"v4\"/>",
            GetHtmlDomAsString());

  slot(0)->set_should_delete_element(true);
  slot(0)->Render();

  // After update, first link is gone.
  EXPECT_EQ("<link href=\"v3\" src=\"v4\"/>", GetHtmlDomAsString());
}

}  // namespace net_instaweb
