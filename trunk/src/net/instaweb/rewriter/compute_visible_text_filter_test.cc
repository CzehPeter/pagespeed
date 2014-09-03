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

// Author: guptaa@google.com (Ashish Gupta)

#include "net/instaweb/rewriter/public/compute_visible_text_filter.h"

#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/rewriter/public/blink_util.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "pagespeed/kernel/base/gtest.h"
#include "pagespeed/kernel/base/mock_timer.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

namespace {

const char kHtmlInput[] =
    "<html>"
    "<head>"
    "<title>Title.</title>"
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<script>Script.</script>"
    "<script>"
    "<![CDATA["
    "document.write('foo')"
    "]]>"
    "</script>"
    "<style>Style.</style>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "</head>"
    "<body>"
    "<noscript>No Script.</noscript>"
    "<!--[if IE]>"
    "<p>This is IE.</p>"
    "<![endif]-->"
    "<div><span id=\"foo\"></span></div>"
    "<div id=\"header\">Header.</div>"
    "<div id=\"container\" class>"
      "<h2 Id=\"beforeItems\">Header 2.</h2>"
      "<div class=\"another item here\">"
         "<img alt=\"alt1\" src=\"image1\"/>"
         "<img src=\"image2\"/>"
      "</div>"
      "<div class=\"item\">"
         "<img src=\"image3\"/>"
         "<p>Paragraph text.</p>"
      "</div>"
    "</div>"
    "</body></html>";

const char kTextContent[] =
    "Title."
    "<meta http-equiv=\"last-modified\" content=\"2012-08-09T11:03:27Z\"/>"
    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\"/>"
    "Header.Header 2.image1image2image3Paragraph text.";

}  // namespace

class ComputeVisibleTextFilterTest : public RewriteTestBase {
 public:
  ComputeVisibleTextFilterTest() {}

  virtual void SetUp() {
    options_->EnableFilter(RewriteOptions::kComputeVisibleText);
    options_->DisableFilter(RewriteOptions::kHtmlWriterFilter);
    SetUseManagedRewriteDrivers(true);
    RewriteTestBase::SetUp();
    rewrite_driver_->SetWriter(&write_to_string_);

    response_headers_.set_status_code(HttpStatus::kOK);
    response_headers_.SetDateAndCaching(MockTimer::kApr_5_2010_ms, 0);
    rewrite_driver()->set_response_headers_ptr(&response_headers_);
  }

  virtual bool AddHtmlTags() const { return false; }

 protected:
  HTTPValue value_;
  ResponseHeaders response_headers_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ComputeVisibleTextFilterTest);
};

TEST_F(ComputeVisibleTextFilterTest, ComputeVisibleText) {
  ValidateExpected("strip_tags", kHtmlInput,
                   StrCat(kTextContent,
                          BlinkUtil::kComputeVisibleTextFilterOutputEndMarker,
                          kHtmlInput));
}

}  // namespace net_instaweb
