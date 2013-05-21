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

// Author: nforman@google.com (Naomi Forman)
//
// This file contains unit tests for the InsertGAFilter.

#include "net/instaweb/rewriter/public/insert_ga_filter.h"

#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/null_message_handler.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

namespace {

const char kGaId[] = "UA-21111111-1";

// Test fixture for InsertGAFilter unit tests.
class InsertGAFilterTest : public RewriteTestBase {
 protected:
  virtual void SetUp() {
    options()->set_ga_id(kGaId);
    options()->EnableFilter(RewriteOptions::kInsertGA);
    RewriteTestBase::SetUp();
  }
};

const char kHtmlInput[] =
    "<head>\n"
    "<title>Something</title>\n"
    "</head>"
    "<body> Hello World!</body>";

const char kHtmlOutputFormat[] =
    "<head><script type=\"text/javascript\">"
    "%s</script>\n<title>Something</title>\n"
    "</head><body> Hello World!"
    "<script type=\"text/javascript\">%s</script>"
    "</body>";


GoogleString GenerateExpectedHtml(GoogleString domain_name,
                                  GoogleString experiment_vars,
                                  GoogleString speed_tracking,
                                  GoogleString url_prefix) {
  GoogleString experiment_snippet = StringPrintf(kGAExperimentSnippet,
                                                 speed_tracking.c_str(),
                                                 experiment_vars.c_str());
  GoogleString analytics_js = StringPrintf(kGAJsSnippet,
                                           kGaId,
                                           domain_name.c_str(),
                                           url_prefix.c_str());
  GoogleString output = StringPrintf(kHtmlOutputFormat,
                                     experiment_snippet.c_str(),
                                     analytics_js.c_str());
  return output;
}

TEST_F(InsertGAFilterTest, SimpleInsert) {
  rewrite_driver()->AddFilters();
  GoogleString output = GenerateExpectedHtml("test.com", "", kGASpeedTracking,
                                             "http://www");
  ValidateExpected("simple_addition", kHtmlInput, output);

  output = GenerateExpectedHtml("www.test1.com", "", kGASpeedTracking,
                                "https://ssl");
  ValidateExpectedUrl("https://www.test1.com/index.html", kHtmlInput,
                      output);
}

TEST_F(InsertGAFilterTest, NoIncreasedSpeed) {
  options()->set_increase_speed_tracking(false);
  rewrite_driver()->AddFilters();
  GoogleString output = GenerateExpectedHtml("test.com", "", "",
                                             "http://www");
  ValidateExpected("simple_addition", kHtmlInput, output);
}

TEST_F(InsertGAFilterTest, Experiment) {
  NullMessageHandler handler;
  RewriteOptions* options = rewrite_driver()->options()->Clone();
  options->set_running_experiment(true);
  ASSERT_TRUE(options->AddExperimentSpec("id=2;percent=10;slot=4;", &handler));
  ASSERT_TRUE(options->AddExperimentSpec(
      "id=7;percent=10;level=CoreFilters;slot=4;", &handler));
  options->SetExperimentState(2);

  // Setting up experiments automatically enables AddInstrumentation.
  // Turn it off so our output is easier to understand.
  options->DisableFilter(RewriteOptions::kAddInstrumentation);
  rewrite_driver()->set_custom_options(options);
  rewrite_driver()->AddFilters();

  GoogleString variable_value = StringPrintf(
      "_gaq.push(['_setCustomVar', 4, 'ExperimentState', '%s']);",
      options->ToExperimentString().c_str());
  GoogleString output = GenerateExpectedHtml("test.com", variable_value,
                                             kGASpeedTracking, "http://www");
  ValidateExpected("simple_addition", kHtmlInput, output);
}

const char kHtmlInputWithGASnippetFormat[] =
    "<head>\n<title>Something</title>\n"
    "</head><body> Hello World!"
    "<script type=\"text/javascript\">%s</script>"
    "</body>";

TEST_F(InsertGAFilterTest, ExperimentNoDouble) {
  NullMessageHandler handler;
  RewriteOptions* options = rewrite_driver()->options()->Clone();
  options->set_running_experiment(true);
  ASSERT_TRUE(options->AddExperimentSpec("id=2;percent=10;", &handler));
  ASSERT_TRUE(options->AddExperimentSpec("id=7;percent=10;level=CoreFilters",
                                         &handler));
  options->SetExperimentState(2);

  // Setting up experiments automatically enables AddInstrumentation.
  // Turn it off so our output is easier to understand.
  options->DisableFilter(RewriteOptions::kAddInstrumentation);
  rewrite_driver()->set_custom_options(options);
  rewrite_driver()->AddFilters();

  // Input already has a GA js snippet.
  GoogleString analytics_js =
      StringPrintf(kGAJsSnippet, kGaId, "test.com", "http://www");
  GoogleString input = StringPrintf(kHtmlInputWithGASnippetFormat,
                                     analytics_js.c_str());
  GoogleString variable_value = StringPrintf(
      "_gaq.push(['_setCustomVar', 1, 'ExperimentState', '%s']);",
      options->ToExperimentString().c_str());
  GoogleString experiment_snippet = StringPrintf(kGAExperimentSnippet,
                                                 kGASpeedTracking,
                                                 variable_value.c_str());
  // The output should still have the original GA snippet as well as an inserted
  // experiment snippet.
  GoogleString output = StringPrintf(
      kHtmlOutputFormat, experiment_snippet.c_str(), analytics_js.c_str());

  ValidateExpected("variable_added", input, output);
}

TEST_F(InsertGAFilterTest, ManyHeadsAndBodies) {
  // Make sure we only add the GA snippet in one place.
  rewrite_driver()->AddFilters();
  const char* kHeadsFmt = "<head>%s</head><head></head><head></head></head>"
      "<body>%s</body><body></body>";
  GoogleString input = StringPrintf(kHeadsFmt, "", "");
  GoogleString experiment_snippet = StringPrintf(kGAExperimentSnippet,
                                                 kGASpeedTracking,
                                                 "");
  GoogleString analytics_js = StringPrintf(kGAJsSnippet, kGaId, "test.com",
                                           "http://www");

  GoogleString output = StringPrintf(kHeadsFmt,
                                     StrCat("<script type=\"text/javascript\">",
                                            experiment_snippet,
                                            "</script>").c_str(),
                                     StrCat("<script type=\"text/javascript\">",
                                            analytics_js, "</script>").c_str());
  ValidateExpected("many_heads_and_bodies", input, output);
}

}  // namespace

}  // namespace net_instaweb
