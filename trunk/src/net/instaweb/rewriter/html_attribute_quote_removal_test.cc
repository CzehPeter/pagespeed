/**
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

// Author: mdsteele@google.com (Matthew D. Steele)

#include "base/basictypes.h"
#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/rewriter/public/html_attribute_quote_removal.h"

namespace net_instaweb {

class HtmlAttributeQuoteRemovalTest : public HtmlParseTestBase {
 protected:
  HtmlAttributeQuoteRemovalTest()
      : html_attribute_quote_removal_(&html_parse_) {
    html_parse_.AddFilter(&html_attribute_quote_removal_);
  }

  virtual bool AddBody() const { return true; }

 private:
  HtmlAttributeQuoteRemoval html_attribute_quote_removal_;

  DISALLOW_COPY_AND_ASSIGN(HtmlAttributeQuoteRemovalTest);
};

TEST_F(HtmlAttributeQuoteRemovalTest, NoQuotesNoChange) {
  ValidateNoChanges("no_quotes_no_change",
                    "<div class=foo id=bar>foobar</div>");
}

TEST_F(HtmlAttributeQuoteRemovalTest, DoNotRemoveNeededQuotes) {
  ValidateNoChanges("do_not_remove_needed_quotes",
                    "<a href=\"http://www.example.com/\">foobar</a>");
}

TEST_F(HtmlAttributeQuoteRemovalTest, DoNotDeleteEmptyAttrs) {
  ValidateNoChanges("do_not_delete_empty_attrs",
                    "<div id=''></div>");
}

TEST_F(HtmlAttributeQuoteRemovalTest, RemoveUnneededQuotes) {
  ValidateExpected("remove_unneeded_quotes",
                   "<div class=\"foo\" id='bar'>foobar</div>",
                   "<div class=foo id=bar>foobar</div>");
}

TEST_F(HtmlAttributeQuoteRemovalTest, NoValueNoChange) {
  ValidateNoChanges("no_value_no_change",
                    "<input checked type=checkbox>");
}

TEST_F(HtmlAttributeQuoteRemovalTest, DoNotRemoveQuotesInXhtml) {
  // TODO(mdsteele): This test is kinda sketchy; the DOCTYPE directive should
  //    really come at the very start of the document, but HtmlParseTestBase
  //    will insert <html> and <body> tags around this.
  ValidateNoChanges("do_not_remove_quotes_in_xhtml",
                    "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
                    "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
                    "<div class=\"foo\" id='bar'>foobar</div>");
}

}  // namespace net_instaweb
