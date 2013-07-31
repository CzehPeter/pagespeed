/*
 * Copyright 2013 Google Inc.
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
// Author: slamm@google.com (Stephen Lamm)

#include "net/instaweb/rewriter/public/critical_selector_finder.h"

#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/critical_selectors.pb.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/test_rewrite_driver_factory.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/mock_property_page.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"

namespace net_instaweb {

namespace {

const char kRequestUrl[] = "http://www.example.com";

class CriticalSelectorFinderTest : public RewriteTestBase {
 protected:
  virtual void SetUp() {
    RewriteTestBase::SetUp();
    finder_.reset(new CriticalSelectorFinder(
        RewriteDriver::kBeaconCohort, statistics()));
    SetupCohort(page_property_cache(), RewriteDriver::kBeaconCohort);
    ResetDriver();
  }

  void ResetDriver() {
    rewrite_driver()->Clear();
    rewrite_driver()->set_request_context(
        RequestContext::NewTestRequestContext(factory()->thread_system()));
    MockPropertyPage* page = NewMockPage(kRequestUrl);
    rewrite_driver()->set_property_page(page);
    PropertyCache* pcache = server_context_->page_property_cache();
    pcache->Read(page);
  }

  void CheckCriticalSelectorFinderStats(int hits, int expiries, int not_found) {
    EXPECT_EQ(hits, statistics()->GetVariable(
        CriticalSelectorFinder::kCriticalSelectorsValidCount)->Get());
    EXPECT_EQ(expiries, statistics()->GetVariable(
        CriticalSelectorFinder::kCriticalSelectorsExpiredCount)->Get());
    EXPECT_EQ(not_found, statistics()->GetVariable(
        CriticalSelectorFinder::kCriticalSelectorsNotFoundCount)->Get());
  }

  bool IsCriticalSelector(StringPiece selector) {
    scoped_ptr<CriticalSelectorSet> read_selectors(
        finder_->DecodeCriticalSelectorsFromPropertyCache(rewrite_driver()));
    if (read_selectors.get() == NULL) {
      return false;
    }
    for (int i = 0; i < read_selectors->critical_selectors_size(); ++i) {
      if (read_selectors->critical_selectors(i) == selector) {
        return true;
      }
    }
    return false;
  }

 protected:
  scoped_ptr<CriticalSelectorFinder> finder_;
};

TEST_F(CriticalSelectorFinderTest, StoreRestore) {
  CheckCriticalSelectorFinderStats(0, 0, 0);
  scoped_ptr<CriticalSelectorSet> read_selectors;
  read_selectors.reset(
      finder_->DecodeCriticalSelectorsFromPropertyCache(rewrite_driver()));
  EXPECT_TRUE(read_selectors.get() == NULL);
  CheckCriticalSelectorFinderStats(0, 0, 1);

  StringSet selectors;
  selectors.insert(".foo");
  selectors.insert("#bar");

  finder_->WriteCriticalSelectorsToPropertyCache(selectors, rewrite_driver());

  const PropertyCache::Cohort* cohort =
      page_property_cache()->GetCohort(RewriteDriver::kBeaconCohort);
  rewrite_driver()->property_page()->WriteCohort(cohort);

  ResetDriver();

  read_selectors.reset(
      finder_->DecodeCriticalSelectorsFromPropertyCache(rewrite_driver()));
  ASSERT_TRUE(read_selectors.get() != NULL);
  ASSERT_EQ(2, read_selectors->critical_selectors_size());
  EXPECT_EQ("#bar", read_selectors->critical_selectors(0));
  EXPECT_EQ(".foo", read_selectors->critical_selectors(1));
  CheckCriticalSelectorFinderStats(1, 0, 1);

  // Now test expiration.
  ResetDriver();
  AdvanceTimeMs(2 * options()->finder_properties_cache_expiration_time_ms());
  read_selectors.reset(
      finder_->DecodeCriticalSelectorsFromPropertyCache(rewrite_driver()));
  EXPECT_TRUE(read_selectors.get() == NULL);
  CheckCriticalSelectorFinderStats(1, 1, 1);
}

// Verify that writing multiple beacon results are stored and aggregated. The
// critical selector set should be equal to all selectors seen in the last
// NumSetsToKeep() beacon responses.
TEST_F(CriticalSelectorFinderTest, StoreMultiple) {
  StringSet selectors;
  selectors.insert(".a");
  finder_->WriteCriticalSelectorsToPropertyCache(selectors, rewrite_driver());
  EXPECT_TRUE(IsCriticalSelector(".a"));
  EXPECT_FALSE(IsCriticalSelector(".b"));

  selectors.clear();
  selectors.insert(".b");
  for (int i = 0; i < finder_->NumSetsToKeep() - 1; ++i) {
    finder_->WriteCriticalSelectorsToPropertyCache(selectors, rewrite_driver());
    EXPECT_TRUE(IsCriticalSelector(".a"));
    EXPECT_TRUE(IsCriticalSelector(".b"));
  }

  // We send one more beacon response, which should kick .a out of the critical
  // selector set.
  selectors.clear();
  selectors.insert("#c");
  finder_->WriteCriticalSelectorsToPropertyCache(selectors, rewrite_driver());
  EXPECT_FALSE(IsCriticalSelector(".a"));
  EXPECT_TRUE(IsCriticalSelector(".b"));
  EXPECT_TRUE(IsCriticalSelector("#c"));
}

}  // namespace

}  // namespace net_instaweb