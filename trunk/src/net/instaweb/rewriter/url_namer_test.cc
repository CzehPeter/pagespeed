// Copyright 2012 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jmarantz@google.com (Matt Atterbury)

// Unit-test base-class url naming.

#include "net/instaweb/rewriter/public/url_namer.h"

#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

class RewriteFilter;

class UrlNamerTest : public RewriteTestBase {
 protected:
};

TEST_F(UrlNamerTest, UrlNamerEncoding) {
  DomainLawyer* lawyer = options()->WriteableDomainLawyer();
  const char kRewriteDomain[] = "http://to.example.com/";
  const char kShard1[] = "http://s1.example.com/";
  const char kShard2[] = "http://s2.example.com/";
  ASSERT_TRUE(lawyer->AddRewriteDomainMapping(
      kRewriteDomain, "from.example.com", message_handler()));
  ASSERT_TRUE(lawyer->AddShard(
      kRewriteDomain, StrCat(kShard1, ",", kShard2), message_handler()));
  GoogleUrl gurl(Encode("http://to.example.com/", "cf", "0",
                        "file.css", "css"));
  RewriteFilter* filter;
  OutputResourcePtr resource = rewrite_driver()->DecodeOutputResource(
      gurl, &filter);
  UrlNamer url_namer;
  EXPECT_EQ(Encode(kShard1, "cf", "0", "file.css", "css"),
            url_namer.Encode(options(), *resource.get(), UrlNamer::kSharded))
      << "with sharding";
  EXPECT_EQ(Encode(kRewriteDomain, "cf", "0", "file.css", "css"),
            url_namer.Encode(options(), *resource.get(), UrlNamer::kUnsharded))
      << "without sharding";
}

TEST_F(UrlNamerTest, ResolveToOriginUrlWithoutReferer) {
  UrlNamer url_namer;
  // There is no origin mappings so nothings will get updated.
  GoogleUrl url("http://www1.test.com/index.html");
  StringPiece referer;
  GoogleString host_header;
  EXPECT_FALSE(url_namer.ResolveToOriginUrl(*options(), referer,
                                            &host_header, &url));
  EXPECT_STREQ("www1.test.com", host_header);

  NullMessageHandler handler;
  options()->WriteableDomainLawyer()->AddOriginDomainMapping(
      "www.test.com", "www1.test.com/www.test.com", "" /* host_header */,
      &handler);

  EXPECT_FALSE(url_namer.ResolveToOriginUrl(*options(), referer,
                                            &host_header, &url));
  EXPECT_STREQ("www1.test.com", host_header);
  host_header.clear();

  url.Reset("http://www1.test.com/www.test.com/index.html");
  EXPECT_TRUE(url_namer.ResolveToOriginUrl(*options(), referer,
                                           &host_header, &url));
  EXPECT_EQ("http://www.test.com/index.html", url.Spec());
  EXPECT_STREQ("www1.test.com", host_header);
  host_header.clear();

  url.Reset("http://www1.test.com/img/index.html");
  EXPECT_FALSE(url_namer.ResolveToOriginUrl(*options(), referer, &host_header,
                                            &url));
  EXPECT_STREQ("www1.test.com", host_header);
}

TEST_F(UrlNamerTest, ResolveToOriginUrl) {
  UrlNamer url_namer;
  // There is no origin mappings so nothings will get updated.
  GoogleUrl url("http://www1.test.com/index.html");
  StringPiece referer;
  GoogleString host_header;
  referer = "http://www1.test.com/www.test.com/img/";
  EXPECT_FALSE(url_namer.ResolveToOriginUrl(*options(), referer, &host_header,
                                            &url));
  EXPECT_STREQ("www1.test.com", host_header);
  host_header.clear();

  NullMessageHandler handler;
  options()->WriteableDomainLawyer()->AddOriginDomainMapping(
      "www.test.com", "www1.test.com/www.test.com", "" /* host_header */,
      &handler);

  EXPECT_TRUE(url_namer.ResolveToOriginUrl(*options(), referer, &host_header,
                                           &url));
  EXPECT_EQ("http://www.test.com/index.html", url.Spec());
  EXPECT_STREQ("www1.test.com", host_header);
  host_header.clear();

  // There is not origin rule for "www1.test.com/m.test.com", so referer is
  // used for determining origin domain.
  url.Reset("http://www1.test.com/m.test.com/index.html");
  EXPECT_TRUE(url_namer.ResolveToOriginUrl(*options(), referer,
                                           &host_header, &url));
  EXPECT_EQ("http://www.test.com/m.test.com/index.html", url.Spec());
  EXPECT_STREQ("www1.test.com", host_header);
  host_header.clear();

  // If request url has origin rule, then referer origin rule is ignored.
  options()->WriteableDomainLawyer()->AddOriginDomainMapping(
      "m.test.com", "www1.test.com/m.test.com", "" /* host_header */, &handler);
  url.Reset("http://www1.test.com/m.test.com/index.html");
  EXPECT_TRUE(url_namer.ResolveToOriginUrl(*options(), referer,
                                           &host_header, &url));
  EXPECT_EQ("http://m.test.com/index.html", url.Spec());
  EXPECT_STREQ("www1.test.com", host_header);
}

}  // namespace net_instaweb
