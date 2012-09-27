/*
 * Copyright 2012 Google Inc.
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

#include "net/instaweb/rewriter/public/collect_flush_early_content_filter.h"

#include "net/instaweb/rewriter/flush_early.pb.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class CollectFlushEarlyContentFilterTest : public RewriteTestBase {
 public:
  CollectFlushEarlyContentFilterTest() {}

 protected:
  virtual void SetUp() {
    options()->EnableFilter(RewriteOptions::kFlushSubresources);
    options()->EnableFilter(RewriteOptions::kInlineImportToLink);
    RewriteTestBase::SetUp();
    rewrite_driver()->AddFilters();
  }

  virtual bool AddHtmlTags() const { return false; }

 private:
  DISALLOW_COPY_AND_ASSIGN(CollectFlushEarlyContentFilterTest);
};

TEST_F(CollectFlushEarlyContentFilterTest, CollectFlushEarlyContentFilter) {
  GoogleString html_input =
      "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
      "<html>"
      "<head>"
        "<script src=\"a.js\">"
        "</script>"
        "<link type=\"text/css\" rel=\"stylesheet\" href=\"a.css\" "
          "media=\"print\"/>"
        "<link type=\"text/css\" rel=\"stylesheet\" href=\"b.css\"/>"
        "<script src=\"b.js\" type=\"text/javascript\"></script>"
        "<noscript>"
        "<script src=\"c.js\">"
        "</script>"
        "</noscript>"
      "</head>"
      "<body>"
        "<link type=\"text/css\" rel=\"stylesheet\" href=\"c.css\"/>"
        "<script src=\"c.js\"></script>"
      "</body>"
      "</html>";

  Parse("not_flushed_early", html_input);
  FlushEarlyInfo* flush_early_info = rewrite_driver()->flush_early_info();
  EXPECT_STREQ("<script src=\"http://test.com/a.js\"></script>"
               "<link type=\"text/css\" rel=\"stylesheet\" "
               "href=\"http://test.com/a.css\" media=\"print\"/>"
               "<link type=\"text/css\" rel=\"stylesheet\" "
               "href=\"http://test.com/b.css\"/>"
               "<script src=\"http://test.com/b.js\" type=\"text/javascript\">"
               "</script>"
               "<link type=\"text/css\" rel=\"stylesheet\" "
               "href=\"http://test.com/c.css\"/>",
               flush_early_info->resource_html());
}

TEST_F(CollectFlushEarlyContentFilterTest, WithInlineInportToLinkFilter) {
  GoogleString html_input =
      "<!doctype html PUBLIC \"HTML 4.0.1 Strict>"
      "<html>"
      "<head>"
        "<style>@import url(assets/styles.css);</style>"
      "</head>"
      "<body>"
      "</body>"
      "</html>";

  Parse("not_flushed_early", html_input);
  FlushEarlyInfo* flush_early_info = rewrite_driver()->flush_early_info();
  EXPECT_STREQ("<link rel=\"stylesheet\" "
               "href=\"http://test.com/assets/styles.css\"/>",
               flush_early_info->resource_html());
}

}  // namespace net_instaweb
