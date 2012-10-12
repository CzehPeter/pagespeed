/*
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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/rewriter/public/domain_lawyer.h"

#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/mock_message_handler.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace {

const char kResourceUrl[] = "styles/style.css?appearance=reader";
const char kCdnPrefix[] = "http://graphics8.nytimes.com/";
const char kRequestDomain[] = "http://www.nytimes.com/";
const char kRequestDomainPort[] = "http://www.nytimes.com:8080/";

}  // namespace

namespace net_instaweb {

class DomainLawyerTest : public testing::Test {
 protected:
  DomainLawyerTest()
      : orig_request_("http://www.nytimes.com/index.html"),
        port_request_("http://www.nytimes.com:8080/index.html"),
        https_request_("https://www.nytimes.com/index.html") {
  }

  // Syntactic sugar to map a request.
  bool MapRequest(const GoogleUrl& original_request,
                  const StringPiece& resource_url,
                  GoogleString* mapped_domain_name) {
    GoogleUrl resolved_request;
    return MapRequest(original_request, resource_url, mapped_domain_name,
                      &resolved_request);
  }

  // Syntactic sugar to map a request.
  bool MapRequest(const GoogleUrl& original_request,
                  const StringPiece& resource_url,
                  GoogleString* mapped_domain_name,
                  GoogleUrl* resolved_request) {
    return domain_lawyer_.MapRequestToDomain(
        original_request, resource_url, mapped_domain_name, resolved_request,
        &message_handler_);
  }

  bool MapOrigin(const StringPiece& in, GoogleString* out) {
    return domain_lawyer_.MapOrigin(in, out);
  }

  bool AddOriginDomainMapping(const StringPiece& dest, const StringPiece& src) {
    return domain_lawyer_.AddOriginDomainMapping(dest, src, &message_handler_);
  }

  bool AddRewriteDomainMapping(const StringPiece& dest,
                               const StringPiece& src) {
    return domain_lawyer_.AddRewriteDomainMapping(dest, src, &message_handler_);
  }

  bool AddShard(const StringPiece& domain, const StringPiece& shards) {
    return domain_lawyer_.AddShard(domain, shards, &message_handler_);
  }

  GoogleUrl orig_request_;
  GoogleUrl port_request_;
  GoogleUrl https_request_;
  DomainLawyer domain_lawyer_;
  MockMessageHandler message_handler_;
};

TEST_F(DomainLawyerTest, RelativeDomain) {
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(orig_request_, kResourceUrl, &mapped_domain_name));
  EXPECT_STREQ(kRequestDomain, mapped_domain_name);
  EXPECT_FALSE(domain_lawyer_.can_rewrite_domains());
}

TEST_F(DomainLawyerTest, AbsoluteDomain) {
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(orig_request_, StrCat(kRequestDomain, kResourceUrl),
                         &mapped_domain_name));
  EXPECT_STREQ(kRequestDomain, mapped_domain_name);
  EXPECT_FALSE(domain_lawyer_.can_rewrite_domains());
}

TEST_F(DomainLawyerTest, ExternalDomainNotDeclared) {
  GoogleString mapped_domain_name;
  EXPECT_FALSE(MapRequest(
      orig_request_, StrCat(kCdnPrefix, kResourceUrl), &mapped_domain_name));
}

TEST_F(DomainLawyerTest, ExternalDomainDeclared) {
  StringPiece cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix));

  // Any domain is authorized with respect to an HTML from the same domain.
  GoogleUrl orig_domain(orig_request_.Origin());
  EXPECT_TRUE(domain_lawyer_.IsDomainAuthorized(orig_request_, orig_domain));

  // But to pull in a resource from another domain, we must first authorize it.
  GoogleUrl cdn_gurl(cdn_domain);
  EXPECT_FALSE(domain_lawyer_.IsDomainAuthorized(orig_request_, cdn_gurl));
  ASSERT_TRUE(domain_lawyer_.AddDomain(cdn_domain, &message_handler_));
  EXPECT_TRUE(domain_lawyer_.IsDomainAuthorized(orig_request_, cdn_gurl));
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(
      orig_request_, StrCat(kCdnPrefix, kResourceUrl), &mapped_domain_name));
  EXPECT_STREQ(cdn_domain, mapped_domain_name);

  // Make sure that we do not allow requests when the port is present; we've
  // only authorized origin "http://www.nytimes.com/",
  // not "http://www.nytimes.com:8080/".
  // The '-1' below is to strip the trailing slash.
  GoogleString orig_cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix) - 1);
  GoogleString port_cdn_domain(cdn_domain.data(), cdn_domain.size() - 1);
  port_cdn_domain += ":8080/";
  EXPECT_FALSE(MapRequest(
      orig_request_, StrCat(port_cdn_domain, "/", kResourceUrl),
      &mapped_domain_name));
  EXPECT_FALSE(domain_lawyer_.DoDomainsServeSameContent(
      port_cdn_domain, cdn_domain));
}

TEST_F(DomainLawyerTest, ExternalUpperCaseDomainDeclared) {
  GoogleString cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix));
  UpperString(&cdn_domain);   // will get normalized in AddDomain.
  ASSERT_TRUE(domain_lawyer_.AddDomain(cdn_domain, &message_handler_));
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(
      orig_request_, StrCat(kCdnPrefix, kResourceUrl), &mapped_domain_name));
  LowerString(&cdn_domain);
  EXPECT_STREQ(cdn_domain, mapped_domain_name);

  // Make sure that we do not allow requests when the port is present; we've
  // only authorized origin "http://www.nytimes.com/",
  // not "http://www.nytimes.com:8080/".
  // The '-1' below is to strip the trailing slash.
  GoogleString orig_cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix) - 1);
  GoogleString port_cdn_domain(cdn_domain.data(), cdn_domain.size() - 1);
  port_cdn_domain += ":8080/";
  EXPECT_FALSE(MapRequest(
      orig_request_, StrCat(port_cdn_domain, "/", kResourceUrl),
      &mapped_domain_name));
}

TEST_F(DomainLawyerTest, ExternalDomainDeclaredWithoutScheme) {
  StringPiece cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix));
  ASSERT_TRUE(domain_lawyer_.AddDomain(kCdnPrefix + strlen("http://"),
                                       &message_handler_));
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(
      orig_request_, StrCat(kCdnPrefix, kResourceUrl), &mapped_domain_name));
  EXPECT_STREQ(cdn_domain, mapped_domain_name);
}

TEST_F(DomainLawyerTest, ExternalDomainDeclaredWithoutTrailingSlash) {
  StringPiece cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix));
  // The '-1' below is to strip the trailing slash.
  StringPiece cdn_domain_no_slash(kCdnPrefix, STATIC_STRLEN(kCdnPrefix) - 1);
  ASSERT_TRUE(domain_lawyer_.AddDomain(cdn_domain_no_slash, &message_handler_));
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(
      orig_request_, StrCat(kCdnPrefix, kResourceUrl), &mapped_domain_name));
  EXPECT_STREQ(cdn_domain, mapped_domain_name);
}

TEST_F(DomainLawyerTest, WildcardDomainDeclared) {
  StringPiece cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix));
  ASSERT_TRUE(domain_lawyer_.AddDomain("*.nytimes.com", &message_handler_));
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(
      orig_request_, StrCat(kCdnPrefix, kResourceUrl), &mapped_domain_name));
  EXPECT_STREQ(cdn_domain, mapped_domain_name);
}

TEST_F(DomainLawyerTest, RelativeDomainPort) {
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(port_request_, kResourceUrl, &mapped_domain_name));
  EXPECT_STREQ(kRequestDomainPort, mapped_domain_name);
}

TEST_F(DomainLawyerTest, AbsoluteDomainPort) {
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(
      port_request_, StrCat(kRequestDomainPort, kResourceUrl),
      &mapped_domain_name));
  EXPECT_STREQ(kRequestDomainPort, mapped_domain_name);
}

TEST_F(DomainLawyerTest, PortExternalDomainNotDeclared) {
  GoogleString mapped_domain_name;
  EXPECT_FALSE(MapRequest(
      port_request_, StrCat(kCdnPrefix, kResourceUrl), &mapped_domain_name));
}

TEST_F(DomainLawyerTest, PortExternalDomainDeclared) {
  GoogleString port_cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix) - 1);
  port_cdn_domain += ":8080/";
  ASSERT_TRUE(domain_lawyer_.AddDomain(port_cdn_domain, &message_handler_));
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(
      port_request_, StrCat(port_cdn_domain, kResourceUrl),
      &mapped_domain_name));
  EXPECT_STREQ(port_cdn_domain, mapped_domain_name);

  // Make sure that we do not allow requests when the port is missing; we've
  // only authorized origin "http://www.nytimes.com:8080/",
  // not "http://www.nytimes.com:8080".
  GoogleString orig_cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix) - 1);
  orig_cdn_domain += "/";
  EXPECT_FALSE(MapRequest(port_request_, StrCat(orig_cdn_domain, kResourceUrl),
                          &mapped_domain_name));
}

TEST_F(DomainLawyerTest, PortWildcardDomainDeclared) {
  GoogleString port_cdn_domain(kCdnPrefix, STATIC_STRLEN(kCdnPrefix) - 1);
  port_cdn_domain += ":8080/";
  ASSERT_TRUE(domain_lawyer_.AddDomain("*.nytimes.com:*", &message_handler_));
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(port_request_, StrCat(port_cdn_domain, kResourceUrl),
                         &mapped_domain_name));
  EXPECT_STREQ(port_cdn_domain, mapped_domain_name);
}

TEST_F(DomainLawyerTest, HttpsDomain) {
  ASSERT_TRUE(domain_lawyer_.AddDomain("https://nytimes.com",
                                       &message_handler_));
}

TEST_F(DomainLawyerTest, ResourceFromHttpsPage) {
  ASSERT_TRUE(domain_lawyer_.AddDomain("www.nytimes.com", &message_handler_));
  GoogleString mapped_domain_name;

  // We now handle requests for https, though subsequent fetching might fail.
  ASSERT_TRUE(MapRequest(https_request_, kResourceUrl, &mapped_domain_name));
  ASSERT_TRUE(MapRequest(https_request_, StrCat(kRequestDomain, kResourceUrl),
                         &mapped_domain_name));
}

TEST_F(DomainLawyerTest, MapHttpsAcrossHosts) {
  ASSERT_TRUE(AddOriginDomainMapping("http://insecure.nytimes.com",
                                     "https://secure.nytimes.com"));
  ASSERT_FALSE(AddOriginDomainMapping("https://secure.nytimes.com",
                                      "http://insecure.nytimes.com"));
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin(
      "https://secure.nytimes.com/css/stylesheet.css", &mapped));
  EXPECT_STREQ("http://insecure.nytimes.com/css/stylesheet.css", mapped);
}

TEST_F(DomainLawyerTest, MapHttpsAcrossSchemes) {
  ASSERT_TRUE(AddOriginDomainMapping("http://nytimes.com",
                                     "https://nytimes.com"));
  ASSERT_FALSE(AddOriginDomainMapping("https://nytimes.com",
                                      "http://nytimes.com"));
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("https://nytimes.com/css/stylesheet.css", &mapped));
  EXPECT_STREQ("http://nytimes.com/css/stylesheet.css", mapped);
}

TEST_F(DomainLawyerTest, MapHttpsAcrossPorts) {
  ASSERT_TRUE(AddOriginDomainMapping("http://nytimes.com:8181",
                                     "https://nytimes.com"));
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("https://nytimes.com/css/stylesheet.css", &mapped));
  EXPECT_STREQ("http://nytimes.com:8181/css/stylesheet.css", mapped);
}

TEST_F(DomainLawyerTest, MapHttpsAcrossSchemesAndPorts) {
  ASSERT_TRUE(AddOriginDomainMapping("http://localhost:8080",
                                     "https://nytimes.com:8443"));
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin(
      "https://nytimes.com:8443/css/stylesheet.css", &mapped));
  EXPECT_STREQ("http://localhost:8080/css/stylesheet.css", mapped);
}

TEST_F(DomainLawyerTest, AddTwoProtocolDomainMapping) {
  ASSERT_TRUE(domain_lawyer_.AddTwoProtocolOriginDomainMapping(
      "ref.nytimes.com", "www.nytimes.com", &message_handler_));
  // This will rewrite domains of fetches, but not change urls in page:
  EXPECT_FALSE(domain_lawyer_.can_rewrite_domains());
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin(
      "http://www.nytimes.com/index.html", &mapped));
  EXPECT_STREQ("http://ref.nytimes.com/index.html", mapped);
  ASSERT_TRUE(MapOrigin(
      "https://www.nytimes.com/index.html", &mapped));
  EXPECT_STREQ("https://ref.nytimes.com/index.html", mapped);
}

TEST_F(DomainLawyerTest, RewriteHttpsAcrossHosts) {
  ASSERT_TRUE(AddRewriteDomainMapping("http://insecure.nytimes.com",
                                      "https://secure.nytimes.com"));
  EXPECT_TRUE(domain_lawyer_.DoDomainsServeSameContent(
      "insecure.nytimes.com", "https://secure.nytimes.com"));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  GoogleString mapped_domain_name;
  GoogleUrl insecure_gurl("http://insecure.nytimes.com/index.html");
  ASSERT_TRUE(MapRequest(insecure_gurl,
                         "https://secure.nytimes.com/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://insecure.nytimes.com/", mapped_domain_name);
  // Succeeds because http://insecure... is authorized and matches the request.
  GoogleUrl https_gurl("https://secure.nytimes.com/index.html");
  ASSERT_TRUE(MapRequest(https_gurl,
                         "http://insecure.nytimes.com/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://insecure.nytimes.com/", mapped_domain_name);
  // Succeeds because https://secure... maps to http://insecure...
  ASSERT_TRUE(MapRequest(https_gurl,
                         "https://secure.nytimes.com/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://insecure.nytimes.com/", mapped_domain_name);
}

TEST_F(DomainLawyerTest, RewriteHttpsAcrossPorts) {
  ASSERT_TRUE(AddRewriteDomainMapping("http://nytimes.com:8181",
                                      "https://nytimes.com"));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  GoogleString mapped_domain_name;
  // Succeeds because we map it as specified above.
  GoogleUrl nyt_gurl("http://nytimes.com/index.html");
  ASSERT_TRUE(MapRequest(nyt_gurl, "https://nytimes.com/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://nytimes.com:8181/", mapped_domain_name);
  // Fails because http://nytimes/ is not authorized.
  GoogleUrl nyt_https("https://nytimes.com/index.html");
  ASSERT_FALSE(MapRequest(nyt_https,
                          "http://nytimes.com/css/stylesheet.css",
                          &mapped_domain_name));
  // Succeeds because http://nytimes:8181/ is authorized & matches the request.
  ASSERT_TRUE(MapRequest(nyt_https,
                         "http://nytimes.com:8181/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://nytimes.com:8181/", mapped_domain_name);
  // Succeeds because https://nytimes/ maps to http://nytimes:8181/.
  ASSERT_TRUE(MapRequest(nyt_https,
                         "https://nytimes.com/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://nytimes.com:8181/", mapped_domain_name);
}

TEST_F(DomainLawyerTest, RewriteHttpsAcrossSchemes) {
  ASSERT_TRUE(AddRewriteDomainMapping("http://nytimes.com",
                                      "https://nytimes.com"));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  GoogleString mapped_domain_name;
  GoogleUrl nyt_http("http://nytimes.com/index.html");
  ASSERT_TRUE(MapRequest(nyt_http,
                         "https://nytimes.com/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://nytimes.com/", mapped_domain_name);
  // Succeeds because http://nytimes/ is authorized and matches the request.
  GoogleUrl nyt_https("https://nytimes.com/index.html");
  ASSERT_TRUE(MapRequest(nyt_https,
                         "http://nytimes.com/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://nytimes.com/", mapped_domain_name);
  // Succeeds because https://nytimes/ maps to http://nytimes/.
  ASSERT_TRUE(MapRequest(nyt_https,
                         "https://nytimes.com/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://nytimes.com/", mapped_domain_name);
}

TEST_F(DomainLawyerTest, RewriteHttpsAcrossSchemesAndPorts) {
  ASSERT_TRUE(AddRewriteDomainMapping("http://localhost:8080",
                                      "https://nytimes.com:8443"));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  GoogleString mapped_domain_name;
  GoogleUrl local_8080("http://localhost:8080/index.html");
  ASSERT_TRUE(MapRequest(local_8080,
                         "https://nytimes.com:8443/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://localhost:8080/", mapped_domain_name);
  // Succeeds b/c http://localhost:8080/ is authorized and matches the request.
  GoogleUrl https_nyt_8443("https://nytimes.com:8443/index.html");
  ASSERT_TRUE(MapRequest(https_nyt_8443,
                         "http://localhost:8080/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://localhost:8080/", mapped_domain_name);
  // Succeeds because https://nytimes:8443/ maps to http://localhost:8080/.
  ASSERT_TRUE(MapRequest(https_nyt_8443,
                         "https://nytimes.com:8443/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://localhost:8080/", mapped_domain_name);
  // Relative path also succeeds.
  ASSERT_TRUE(MapRequest(https_nyt_8443, "css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("http://localhost:8080/", mapped_domain_name);
}

TEST_F(DomainLawyerTest, RewriteHttpsToHttps) {
  ASSERT_TRUE(AddRewriteDomainMapping("https://localhost:8443",
                                      "https://nytimes.com:8443"));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  GoogleString mapped_domain_name;
  GoogleUrl local_8443("https://localhost:8443/index.html");
  ASSERT_TRUE(MapRequest(local_8443,
                         "https://nytimes.com:8443/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("https://localhost:8443/", mapped_domain_name);
  // Succeeds b/c https://localhost:8443/ is authorized and matches the request.
  GoogleUrl https_nyt_8443("https://nytimes.com:8443/index.html");
  ASSERT_TRUE(MapRequest(https_nyt_8443,
                         "https://localhost:8443/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("https://localhost:8443/", mapped_domain_name);
  // Succeeds because https://nytimes:8443/ maps to https://localhost:8443/.
  ASSERT_TRUE(MapRequest(https_nyt_8443,
                         "https://nytimes.com:8443/css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("https://localhost:8443/", mapped_domain_name);
  // Relative path also succeeds.
  ASSERT_TRUE(MapRequest(https_nyt_8443, "css/stylesheet.css",
                         &mapped_domain_name));
  EXPECT_STREQ("https://localhost:8443/", mapped_domain_name);
}

TEST_F(DomainLawyerTest, AddTwoProtocolRewriteDomainMapping) {
  ASSERT_TRUE(domain_lawyer_.AddTwoProtocolRewriteDomainMapping(
      "www.nytimes.com", "ref.nytimes.com", &message_handler_));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  GoogleString mapped_domain;
  GoogleUrl containing_page_http("http://www.nytimes.com/index.html");
  GoogleUrl containing_page_https("https://www.nytimes.com/index.html");
  // http page asks for http stylesheet.
  ASSERT_TRUE(MapRequest(
      containing_page_http,
      "http://ref.nytimes.com/css/stylesheet.css", &mapped_domain));
  EXPECT_STREQ("http://www.nytimes.com/", mapped_domain);
  // http page asks for an https stylesheet.  Should still re-map.
  ASSERT_TRUE(MapRequest(
      containing_page_http,
      "https://ref.nytimes.com/css/stylesheet.css", &mapped_domain));
  EXPECT_STREQ("https://www.nytimes.com/", mapped_domain);
  // https page asks for an https stylesheet.
  ASSERT_TRUE(MapRequest(
      containing_page_https,
      "https://ref.nytimes.com/css/stylesheet.css", &mapped_domain));
  EXPECT_STREQ("https://www.nytimes.com/", mapped_domain);
  // https page asks for an http stylesheet.  It shouldn't be doing that, but we
  // preserve the bad behavior so the user realizes something fishy could
  // happen.
  ASSERT_TRUE(MapRequest(
      containing_page_https,
      "http://ref.nytimes.com/css/stylesheet.css", &mapped_domain));
  EXPECT_STREQ("http://www.nytimes.com/", mapped_domain);
}

TEST_F(DomainLawyerTest, FindDomainsRewrittenTo) {
  // No mapping.
  ConstStringStarVector from_domains;
  GoogleUrl gurl("http://www1.example.com/");
  domain_lawyer_.FindDomainsRewrittenTo(gurl, &from_domains);
  EXPECT_EQ(0, from_domains.size());

  // Add mapping.
  ASSERT_TRUE(domain_lawyer_.AddTwoProtocolRewriteDomainMapping(
      "www1.example.com", "www.example.com", &message_handler_));
  ASSERT_TRUE(domain_lawyer_.AddTwoProtocolRewriteDomainMapping(
      "www1.example.com", "xyz.example.com", &message_handler_));

  domain_lawyer_.FindDomainsRewrittenTo(gurl, &from_domains);
  ASSERT_EQ(2, from_domains.size());
  EXPECT_STREQ("http://www.example.com/", *(from_domains[0]));
  EXPECT_STREQ("http://xyz.example.com/", *(from_domains[1]));
}

TEST_F(DomainLawyerTest, AddDomainRedundantly) {
  ASSERT_TRUE(domain_lawyer_.AddDomain("www.nytimes.com", &message_handler_));
  ASSERT_FALSE(domain_lawyer_.AddDomain("www.nytimes.com", &message_handler_));
  ASSERT_TRUE(domain_lawyer_.AddDomain("*", &message_handler_));
  ASSERT_FALSE(domain_lawyer_.AddDomain("*", &message_handler_));
}

TEST_F(DomainLawyerTest, VerifyPortIsDistinct1) {
  ASSERT_TRUE(domain_lawyer_.AddDomain("www.example.com", &message_handler_));
  GoogleString mapped_domain_name;
  GoogleUrl context_gurl("http://www.other.com/index.html");
  EXPECT_FALSE(MapRequest(
      context_gurl,
      "http://www.example.com:81/styles.css",
      &mapped_domain_name));
}

TEST_F(DomainLawyerTest, VerifyPortIsDistinct2) {
  ASSERT_TRUE(
      domain_lawyer_.AddDomain("www.example.com:81", &message_handler_));
  GoogleString mapped_domain_name;
  GoogleUrl context_gurl("http://www.other.com/index.html");
  EXPECT_FALSE(MapRequest(
      context_gurl,
      "http://www.example.com/styles.css",
      &mapped_domain_name));
}

TEST_F(DomainLawyerTest, VerifyWildcardedPortSpec) {
  ASSERT_TRUE(domain_lawyer_.AddDomain("www.example.com*", &message_handler_));
  GoogleUrl context_gurl("http://www.origin.com/index.html");
  GoogleString mapped_domain_name;
  EXPECT_TRUE(MapRequest(
      context_gurl,
      "http://www.example.com/styles.css",
      &mapped_domain_name));
  EXPECT_TRUE(MapRequest(
      context_gurl,
      "http://www.example.com:81/styles.css",
      &mapped_domain_name));
}

TEST_F(DomainLawyerTest, MapRewriteDomain) {
  GoogleUrl context_gurl("http://www.origin.com/index.html");
  ASSERT_TRUE(domain_lawyer_.AddDomain("http://cdn.com/", &message_handler_));
  ASSERT_TRUE(domain_lawyer_.AddDomain("http://origin.com/",
                                       &message_handler_));
  EXPECT_FALSE(domain_lawyer_.DoDomainsServeSameContent(
      "cdn.com", "origin.com"));
  ASSERT_TRUE(AddRewriteDomainMapping("http://cdn.com", "http://origin.com"));
  EXPECT_TRUE(domain_lawyer_.DoDomainsServeSameContent(
      "cdn.com", "origin.com"));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  // First try the mapping from "origin.com" to "cdn.com".
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(
      context_gurl,
      "http://origin.com/styles/blue.css",
      &mapped_domain_name));
  EXPECT_STREQ("http://cdn.com/", mapped_domain_name);

  // But a relative reference will not map because we mapped "origin.com",
  // not "www.origin.com".
  ASSERT_TRUE(MapRequest(
      context_gurl,
      "styles/blue.css",
      &mapped_domain_name));
  EXPECT_STREQ("http://www.origin.com/", mapped_domain_name);

  // Now add the mapping from "www".
  ASSERT_TRUE(AddRewriteDomainMapping("http://cdn.com",
                                      "http://www.origin.com"));
  ASSERT_TRUE(MapRequest(
      context_gurl,
      "styles/blue.css",
      &mapped_domain_name));
  EXPECT_STREQ("http://cdn.com/", mapped_domain_name);
}

TEST_F(DomainLawyerTest, MapRewriteDomainAndPath) {
  GoogleUrl context_gurl("http://www.origin.com/index.html");
  ASSERT_TRUE(domain_lawyer_.AddDomain("http://cdn.com/origin/",
                                       &message_handler_));
  ASSERT_TRUE(domain_lawyer_.AddDomain("http://origin.com/",
                                       &message_handler_));
  ASSERT_TRUE(AddRewriteDomainMapping("http://cdn.com/origin",
                                      "http://origin.com"));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  // First try the mapping from "origin.com" to "cdn.com/origin".
  GoogleUrl resolved_request;
  GoogleString mapped_domain_name;
  ASSERT_TRUE(MapRequest(
      context_gurl,
      "http://origin.com/styles/blue.css",
      &mapped_domain_name,
      &resolved_request));
  EXPECT_STREQ("http://cdn.com/origin/", mapped_domain_name);
  EXPECT_STREQ("http://cdn.com/origin/styles/blue.css",
               resolved_request.Spec());

  // But a relative reference will not map because we mapped "origin.com",
  // not "www.origin.com".
  ASSERT_TRUE(MapRequest(
      context_gurl,
      "styles/blue.css",
      &mapped_domain_name,
      &resolved_request));
  EXPECT_STREQ("http://www.origin.com/", mapped_domain_name);
  EXPECT_STREQ("http://www.origin.com/styles/blue.css",
               resolved_request.Spec());

  // Now add the mapping from "www".
  ASSERT_TRUE(AddRewriteDomainMapping("http://cdn.com/origin",
                                      "http://www.origin.com"));
  ASSERT_TRUE(MapRequest(
      context_gurl,
      "styles/blue.css",
      &mapped_domain_name,
      &resolved_request));
  EXPECT_STREQ("http://cdn.com/origin/", mapped_domain_name);
  EXPECT_STREQ("http://cdn.com/origin/styles/blue.css",
               resolved_request.Spec());
}

TEST_F(DomainLawyerTest, RewriteWithPath) {
  GoogleUrl context_gurl("http://example.com/index.html");
  ASSERT_TRUE(AddRewriteDomainMapping(
      "http://example.com/static/images/", "http://static.com/images/"));
  GoogleString mapped_domain_name;
  GoogleUrl resolved_request;
  ASSERT_TRUE(MapRequest(context_gurl,
                         "http://static.com/images/teapot.png",
                         &mapped_domain_name, &resolved_request));
  EXPECT_STREQ("http://example.com/static/images/", mapped_domain_name);
  EXPECT_STREQ("http://example.com/static/images/teapot.png",
               resolved_request.Spec());
}

TEST_F(DomainLawyerTest, OriginWithPath) {
  ASSERT_TRUE(AddOriginDomainMapping(
      "http://origin.com/subdir/", "http://external.com"));
  GoogleString origin_url;
  ASSERT_TRUE(MapOrigin("http://external.com/styles/main.css", &origin_url));
  EXPECT_STREQ("http://origin.com/subdir/styles/main.css", origin_url);
}

TEST_F(DomainLawyerTest, OriginAndExternWithPaths) {
  ASSERT_TRUE(AddOriginDomainMapping(
      "http://origin.com/subdir/", "http://external.com/static/"));
  GoogleString origin_url;
  ASSERT_TRUE(MapOrigin("http://external.com/static/styles/main.css",
                        &origin_url));
  EXPECT_STREQ("http://origin.com/subdir/styles/main.css", origin_url);
}

TEST_F(DomainLawyerTest, OriginAndExternWithMultipleMatches) {
  domain_lawyer_.AddDomain("http://origin.com", &message_handler_);
  domain_lawyer_.AddDomain("http://origin.com/a/b", &message_handler_);
  domain_lawyer_.AddDomain("http://external.com", &message_handler_);
  ASSERT_TRUE(AddOriginDomainMapping(
      "http://origin.com/a/", "http://external.com/static/"));

  GoogleString origin_url;
  ASSERT_TRUE(MapOrigin("http://external.com/static/styles/main.css",
                        &origin_url));
  EXPECT_STREQ("http://origin.com/a/styles/main.css", origin_url);

  // No mappings should occur on a top level page on external.com,
  // since our directive should apply only to external.com/static.
  const char kTopLevelExternalPage[] = "http://external.com/index.html";
  origin_url.clear();
  ASSERT_TRUE(MapOrigin(kTopLevelExternalPage, &origin_url));
  EXPECT_STREQ(kTopLevelExternalPage, origin_url);
}

TEST_F(DomainLawyerTest, RootDomainOfProxySourceNotAuthorized) {
  ASSERT_TRUE(AddOriginDomainMapping(
      "http://origin.com/a/", "http://external.com/static/"));
  GoogleUrl context_gurl("http://origin.com/index.html");
  GoogleUrl external_domain("http://external.com");

  // It is not OK to rewrite content on external.com.
  EXPECT_FALSE(domain_lawyer_.IsDomainAuthorized(context_gurl,
                                                 external_domain));

  // But it *is* OK to rewrite content on external.com/static.
  external_domain.Reset("http://external.com/static/");
  EXPECT_TRUE(domain_lawyer_.IsDomainAuthorized(context_gurl,
                                                external_domain));
}

TEST_F(DomainLawyerTest, OriginAndExternWithMultipleMatchesDoubleSlash) {
  domain_lawyer_.AddDomain("http://origin.com", &message_handler_);
  domain_lawyer_.AddDomain("http://external.com", &message_handler_);
  ASSERT_TRUE(AddOriginDomainMapping(
      "http://origin.com/subdir/", "http://external.com/static/"));

  GoogleString origin_url;
  ASSERT_TRUE(MapOrigin("http://external.com/static/styles//main.css",
                        &origin_url));
  EXPECT_STREQ("http://origin.com/subdir/styles//main.css", origin_url);
}

TEST_F(DomainLawyerTest, MapOriginDomain) {
  ASSERT_TRUE(AddOriginDomainMapping(
      "http://localhost:8080", "http://origin.com:8080"));
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("http://origin.com:8080/a/b/c?d=f",
                                       &mapped));
  EXPECT_STREQ("http://localhost:8080/a/b/c?d=f", mapped);

  // The origin domain, which might be, say, 'localhost', is not necessarily
  // authorized as a domain for input resources.
  GoogleUrl gurl("http://origin.com:8080/index.html");
  EXPECT_FALSE(MapRequest(gurl, "http://localhost:8080/blue.css", &mapped));
  GoogleUrl page_url("http://origin.com:8080");
  GoogleUrl candidate_url("http://localhost:8080");
  EXPECT_FALSE(domain_lawyer_.IsDomainAuthorized(page_url, candidate_url));

  // Of course, if we were to explicitly authorize then it would be ok.
  // First use a wildcard, which will not cover the ":8080", so the
  // Map will still fail.
  ASSERT_TRUE(domain_lawyer_.AddDomain("localhost*", &message_handler_));
  EXPECT_FALSE(MapRequest(gurl, "http://localhost:8080/blue.css", &mapped));

  // Now, include the port explicitly, and the mapping will be allowed.
  ASSERT_TRUE(domain_lawyer_.AddDomain("localhost:8080", &message_handler_));
  EXPECT_TRUE(MapRequest(gurl, "http://localhost:8080/blue.css", &mapped));
}

TEST_F(DomainLawyerTest, Merge) {
  // Add some mappings for domain_lawywer_.
  ASSERT_TRUE(domain_lawyer_.AddDomain("http://d1.com/", &message_handler_));
  ASSERT_TRUE(AddRewriteDomainMapping(
      "http://cdn1.com", "http://www.o1.com"));
  ASSERT_TRUE(AddOriginDomainMapping(
      "http://localhost:8080", "http://o1.com:8080"));

  // We'll also a mapping that will conflict, and one that won't.
  ASSERT_TRUE(AddOriginDomainMapping("http://dest1/", "http://common_src1"));
  ASSERT_TRUE(AddOriginDomainMapping("http://dest2/", "http://common_src2"));
  ASSERT_TRUE(AddShard("foo.com", "bar1.com,bar2.com"));
  EXPECT_TRUE(domain_lawyer_.DoDomainsServeSameContent("foo.com", "bar1.com"));
  EXPECT_TRUE(domain_lawyer_.DoDomainsServeSameContent("foo.com", "bar2.com"));
  EXPECT_TRUE(domain_lawyer_.DoDomainsServeSameContent("bar1.com", "bar2.com"));
  EXPECT_TRUE(domain_lawyer_.DoDomainsServeSameContent("bar1.com", "foo.com"));
  EXPECT_TRUE(domain_lawyer_.DoDomainsServeSameContent("bar2.com", "foo.com"));
  EXPECT_TRUE(domain_lawyer_.DoDomainsServeSameContent("bar2.com", "bar1.com"));

  // Now add a similar set of mappings for another lawyer.
  DomainLawyer merged;
  ASSERT_TRUE(merged.AddDomain("http://d2.com/", &message_handler_));
  ASSERT_TRUE(merged.AddRewriteDomainMapping(
      "http://cdn2.com", "http://www.o2.com", &message_handler_));
  ASSERT_TRUE(merged.AddOriginDomainMapping(
      "http://localhost:8080", "http://o2.com:8080", &message_handler_));

  // Here's a different mapping for the same source.
  ASSERT_TRUE(merged.AddOriginDomainMapping(
      "http://dest3/", "http://common_src1", &message_handler_));
  ASSERT_TRUE(domain_lawyer_.AddOriginDomainMapping(
      "http://dest4/", "http://common_src3", &message_handler_));

  merged.Merge(domain_lawyer_);

  // Now the tests for both domains should work post-merger.

  GoogleString mapped;
  GoogleUrl resolved_request;
  GoogleUrl o1_index_gurl("http://www.o1.com/index.html");
  ASSERT_TRUE(merged.MapRequestToDomain(
      o1_index_gurl,
      "styles/blue.css", &mapped, &resolved_request, &message_handler_));
  EXPECT_STREQ("http://cdn1.com/", mapped);
  GoogleUrl o2_index_gurl("http://www.o2.com/index.html");
  ASSERT_TRUE(merged.MapRequestToDomain(
      o2_index_gurl,
      "styles/blue.css", &mapped, &resolved_request, &message_handler_));
  EXPECT_STREQ("http://cdn2.com/", mapped);

  ASSERT_TRUE(merged.MapOrigin("http://o1.com:8080/a/b/c?d=f", &mapped));
  EXPECT_STREQ("http://localhost:8080/a/b/c?d=f", mapped);
  ASSERT_TRUE(merged.MapOrigin("http://o2.com:8080/a/b/c?d=f", &mapped));
  EXPECT_STREQ("http://localhost:8080/a/b/c?d=f", mapped);

  // The conflict will be silently resolved to prefer the mapping from
  // the domain that got merged, which is domain_laywer_1, overriding
  // what was previously in the target.
  ASSERT_TRUE(merged.MapOrigin("http://common_src1", &mapped));
  EXPECT_STREQ("http://dest1/", mapped);

  // Now check the domains that were added.
  ASSERT_TRUE(merged.MapOrigin("http://common_src2", &mapped));
  EXPECT_STREQ("http://dest2/", mapped);

  ASSERT_TRUE(merged.MapOrigin("http://common_src3", &mapped));
  EXPECT_STREQ("http://dest4/", mapped);

  GoogleString shard;
  ASSERT_TRUE(merged.ShardDomain("http://foo.com/", 0, &shard));
  EXPECT_STREQ("http://bar1.com/", shard);

  EXPECT_TRUE(merged.DoDomainsServeSameContent("foo.com", "bar1.com"));
  EXPECT_TRUE(merged.DoDomainsServeSameContent("foo.com", "bar2.com"));
  EXPECT_TRUE(merged.DoDomainsServeSameContent("bar1.com", "bar2.com"));
  EXPECT_TRUE(merged.DoDomainsServeSameContent("bar1.com", "foo.com"));
  EXPECT_TRUE(merged.DoDomainsServeSameContent("bar2.com", "foo.com"));
  EXPECT_TRUE(merged.DoDomainsServeSameContent("bar2.com", "bar1.com"));

  EXPECT_TRUE(merged.DoDomainsServeSameContent("cdn1.com", "www.o1.com"));
  EXPECT_TRUE(merged.DoDomainsServeSameContent("cdn2.com", "www.o2.com"));
  EXPECT_FALSE(merged.DoDomainsServeSameContent("cdn1.com", "cdn2.com"));
}

TEST_F(DomainLawyerTest, AddMappingFailures) {
  // Corner cases.
  ASSERT_FALSE(AddRewriteDomainMapping("", "http://origin.com"));
  ASSERT_FALSE(AddRewriteDomainMapping("http://cdn.com", ""));
  ASSERT_FALSE(AddRewriteDomainMapping("http://cdn.com", ","));

  // Ensure that we ignore a mapping of a domain to itself.
  ASSERT_FALSE(AddRewriteDomainMapping("http://origin.com",
                                       "http://origin.com"));
  EXPECT_FALSE(domain_lawyer_.can_rewrite_domains());
  ASSERT_FALSE(AddRewriteDomainMapping("http://origin.com/newroot",
                                       "http://origin.com"));
  EXPECT_FALSE(domain_lawyer_.can_rewrite_domains());

  ASSERT_FALSE(AddRewriteDomainMapping("http://origin.com",
                                       "http://origin.com,"));
  ASSERT_FALSE(AddRewriteDomainMapping("http://origin.com",
                                       ",http://origin.com"));
  ASSERT_FALSE(AddRewriteDomainMapping("http://origin.com/newroot",
                                       "http://origin.com,"));
  ASSERT_FALSE(AddRewriteDomainMapping("http://origin.com/newroot",
                                       ",http://origin.com"));

  // You can never wildcard the target domains.
  EXPECT_FALSE(AddRewriteDomainMapping("foo*.com", "bar.com"));
  EXPECT_FALSE(AddOriginDomainMapping("foo*.com", "bar.com"));
  EXPECT_FALSE(AddShard("foo*.com", "bar.com"));

  // You can use wildcard in source domains for Rewrite and Origin, but not
  // Sharding.
  EXPECT_TRUE(AddRewriteDomainMapping("foo.com", "bar*.com"));
  EXPECT_TRUE(domain_lawyer_.AddOriginDomainMapping("foo.com", "bar*.com",
                                                    &message_handler_));
  EXPECT_FALSE(AddShard("foo.com", "bar*.com"));

  EXPECT_TRUE(AddShard("foo.com", "bar1.com,bar2.com"));
}

TEST_F(DomainLawyerTest, Shard) {
  EXPECT_FALSE(domain_lawyer_.can_rewrite_domains());
  ASSERT_TRUE(AddShard("foo.com", "bar1.com,bar2.com"));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  GoogleString shard;
  ASSERT_TRUE(domain_lawyer_.ShardDomain("http://foo.com/", 0, &shard));
  EXPECT_STREQ("http://bar1.com/", shard);
  ASSERT_TRUE(domain_lawyer_.ShardDomain("http://foo.com/", 1, &shard));
  EXPECT_STREQ("http://bar2.com/", shard);
  EXPECT_FALSE(domain_lawyer_.ShardDomain("http://other.com/", 0, &shard));
}

TEST_F(DomainLawyerTest, ShardHttps) {
  EXPECT_FALSE(domain_lawyer_.can_rewrite_domains());
  ASSERT_TRUE(AddShard("https://foo.com", "https://bar1.com,https://bar2.com"));
  EXPECT_TRUE(domain_lawyer_.can_rewrite_domains());
  GoogleString shard;
  ASSERT_TRUE(domain_lawyer_.ShardDomain("https://foo.com/", 0, &shard));
  EXPECT_STREQ("https://bar1.com/", shard);
  ASSERT_TRUE(domain_lawyer_.ShardDomain("https://foo.com/", 1, &shard));
  EXPECT_STREQ("https://bar2.com/", shard);
  EXPECT_FALSE(domain_lawyer_.ShardDomain("https://other.com/", 0, &shard));
}

TEST_F(DomainLawyerTest, WillDomainChange) {
  ASSERT_TRUE(AddShard("foo.com", "bar1.com,bar2.com"));
  ASSERT_TRUE(AddRewriteDomainMapping("http://cdn.com", "http://origin.com"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("http://foo.com/"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("foo.com/"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("http://foo.com"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("foo.com"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("http://origin.com/"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("http://bar1.com/"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("http://bar2.com/"));
  EXPECT_FALSE(domain_lawyer_.WillDomainChange("http://cdn.com/"));
  EXPECT_FALSE(domain_lawyer_.WillDomainChange("http://other_domain.com/"));
}

TEST_F(DomainLawyerTest, WillDomainChangeOnlyOneShard) {
  ASSERT_TRUE(AddShard("foo.com", "bar1.com"));
  ASSERT_TRUE(AddRewriteDomainMapping("http://cdn.com", "http://origin.com"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("http://foo.com/"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("foo.com/"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("http://foo.com"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("foo.com"));
  EXPECT_TRUE(domain_lawyer_.WillDomainChange("http://origin.com/"));
  EXPECT_FALSE(domain_lawyer_.WillDomainChange("http://bar1.com/"));
  EXPECT_FALSE(domain_lawyer_.WillDomainChange("http://cdn.com/"));
  EXPECT_FALSE(domain_lawyer_.WillDomainChange("http://other_domain.com/"));
}

TEST_F(DomainLawyerTest, MapRewriteToOriginDomain) {
  ASSERT_TRUE(AddRewriteDomainMapping("rewrite.com", "myhost.com"));
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost.com"));
  GoogleString mapped;

  // Check that we can warp all the way from the rewrite to localhost.
  ASSERT_TRUE(MapOrigin("http://rewrite.com/a/b/c?d=f", &mapped));
  EXPECT_STREQ("http://localhost/a/b/c?d=f", mapped);
}

TEST_F(DomainLawyerTest, MapShardToOriginDomain) {
  ASSERT_TRUE(AddRewriteDomainMapping("cdn.myhost.com", "myhost.com"));
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost.com"));
  ASSERT_TRUE(AddShard("cdn.myhost.com", "s1.com,s2.com"));
  GoogleString mapped;

  // Check that we can warp all the way from the cdn to localhost.
  ASSERT_TRUE(MapOrigin("http://s1.com/a/b/c?d=f", &mapped));
  EXPECT_STREQ("http://localhost/a/b/c?d=f", mapped);
  mapped.clear();
  ASSERT_TRUE(MapOrigin("http://s2.com/a/b/c?d=f", &mapped));
  EXPECT_STREQ("http://localhost/a/b/c?d=f", mapped);
}

TEST_F(DomainLawyerTest, ConflictedOrigin1) {
  ASSERT_TRUE(AddOriginDomainMapping(
      "localhost", "myhost.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());

  ASSERT_TRUE(AddOriginDomainMapping(
      "other", "myhost.com"));
  EXPECT_EQ(1, message_handler_.SeriousMessages());

  // The second one will win.
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("http://myhost.com/x", &mapped));
  EXPECT_STREQ("http://other/x", mapped);
}

TEST_F(DomainLawyerTest, NoConflictOnMerge1) {
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost1.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());

  // We are rewriting multiple source domains to the same domain.  Both
  // source domains have the same origin mapping so there is no conflict
  // message.
  ASSERT_TRUE(AddRewriteDomainMapping("cdn.com", "myhost1.com,myhost2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());

  // Of course there's no conflict so it's obvious 'localhost' will win.  Check.
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("http://myhost1.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
  ASSERT_TRUE(MapOrigin("http://myhost2.com/y", &mapped));
  EXPECT_STREQ("http://localhost/y", mapped);
  ASSERT_TRUE(MapOrigin("http://cdn.com/z", &mapped));
  EXPECT_STREQ("http://localhost/z", mapped);
}

TEST_F(DomainLawyerTest, ConflictedOrigin2) {
  ASSERT_TRUE(AddOriginDomainMapping("origin1.com", "myhost1.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddOriginDomainMapping("origin2.com", "myhost2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());

  // We are rewriting multiple source domains to the same domain.  Both
  // source domains have the *different* origin mappings so there will be a
  // conflict message.
  ASSERT_TRUE(AddRewriteDomainMapping("cdn.com", "myhost1.com,myhost2.com"));
  EXPECT_EQ(1, message_handler_.SeriousMessages());

  // The second mapping will win for the automatic propagation for "cdn.com".
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("http://cdn.com/x", &mapped));
  EXPECT_STREQ("http://origin2.com/x", mapped);

  // However, "myhost1.com"'s explicitly set origin will not be overridden.
  ASSERT_TRUE(MapOrigin("http://myhost1.com/y", &mapped));
  EXPECT_STREQ("http://origin1.com/y", mapped);
}

TEST_F(DomainLawyerTest, NoShardConflict) {
  // We are origin-mapping multiple source domains to the same domain.
  // Even though we've overspecified the origin domain in this graph,
  // there are no conflict messages because the origins are the same.
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost1.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddRewriteDomainMapping("cdn.com", "myhost1.com,myhost2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddShard("cdn.com", "s1.com,s2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());

  // Unambiguous mappings from either shard or rewrite domain.
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("http://cdn.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
  mapped.clear();
  ASSERT_TRUE(MapOrigin("http://s1.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
  mapped.clear();
  ASSERT_TRUE(MapOrigin("http://s2.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
}

TEST_F(DomainLawyerTest, NoShardConflictReverse) {
  // This is the same exact test as NoShardConflict, but now we set up
  // the shards first, then the rewrite domain, then the origin mappings.
  ASSERT_TRUE(AddShard("cdn.com", "s1.com,s2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddRewriteDomainMapping("cdn.com", "myhost1.com,myhost2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost1.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());

  // Unambiguous mappings from either shard or rewrite domain.
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("http://cdn.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
  mapped.clear();
  ASSERT_TRUE(MapOrigin("http://s1.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
  mapped.clear();
  ASSERT_TRUE(MapOrigin("http://s2.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
}

TEST_F(DomainLawyerTest, NoShardConflictScramble) {
  // Yet another copy of NoShardConflict, but do the rewrite-mapping last.
  ASSERT_TRUE(AddShard("cdn.com", "s1.com,s2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost1.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddOriginDomainMapping("localhost", "myhost2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddRewriteDomainMapping("cdn.com", "myhost1.com,myhost2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());

  // Unambiguous mappings from either shard or rewrite domain.
  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("http://cdn.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
  mapped.clear();
  ASSERT_TRUE(MapOrigin("http://s1.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
  mapped.clear();
  ASSERT_TRUE(MapOrigin("http://s2.com/x", &mapped));
  EXPECT_STREQ("http://localhost/x", mapped);
}

TEST_F(DomainLawyerTest, ShardConflict1) {
  ASSERT_TRUE(AddShard("cdn1.com", "s1.com,s2.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());

  ASSERT_FALSE(AddShard("cdn2.com", "s2.com,s3.com"));
  EXPECT_EQ(1, message_handler_.SeriousMessages());
}

TEST_F(DomainLawyerTest, RewriteOriginCycle) {
  ASSERT_TRUE(AddShard("b.com", "a.com"));
  ASSERT_TRUE(AddRewriteDomainMapping("b.com", "a.com"));
  // We now have "a.com" and "b.com" in a shard/rewrite cycle.  That's
  // ugly and we don't actually detect that because we don't have a
  // graph traversal that can detect it until we start applying origin
  // domains, which auto-propagate.
  //
  // We will have no serious errors reported until we create the
  // conflict which will chase pointers in a cycle, which gets cut
  // by breadcrumbing, but we wind up with 2 serious errors from
  // one call.

  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddOriginDomainMapping("origin1.com", "a.com"));
  EXPECT_EQ(0, message_handler_.SeriousMessages());
  ASSERT_TRUE(AddOriginDomainMapping("origin2.com", "b.com"));
  EXPECT_EQ(2, message_handler_.SeriousMessages());
}

TEST_F(DomainLawyerTest, WildcardOrder) {
  ASSERT_TRUE(AddOriginDomainMapping("host1", "abc*.com"));
  ASSERT_TRUE(AddOriginDomainMapping("host2", "*z.com"));

  GoogleString mapped;
  ASSERT_TRUE(MapOrigin("http://abc.com/x", &mapped));
  EXPECT_STREQ("http://host1/x", mapped);
  ASSERT_TRUE(MapOrigin("http://z.com/x", &mapped));
  EXPECT_STREQ("http://host2/x", mapped);

  // Define a second lawyer with definitions "*abc*.com" which should
  // come after "abc*.com".
  DomainLawyer second_lawyer, merged_lawyer;
  ASSERT_TRUE(second_lawyer.AddOriginDomainMapping("host3", "*abc*.com",
                                                   &message_handler_));
  ASSERT_TRUE(second_lawyer.AddOriginDomainMapping(
      "host1", "abc*.com", &message_handler_));  // duplicate entry.
  merged_lawyer.Merge(domain_lawyer_);
  merged_lawyer.Merge(second_lawyer);
  EXPECT_EQ(3, merged_lawyer.num_wildcarded_domains());

  // Hopefully we didn't bork the order of "abc* and "*".  Note that just
  // iterating over a std::set will yield the "*" first, as '*' is ascii
  // 42 and 'a' is ascii 97, and the domain-map is over GoogleString.
  ASSERT_TRUE(merged_lawyer.MapOrigin("http://abc.com/x", &mapped));
  EXPECT_STREQ("http://host1/x", mapped);
  ASSERT_TRUE(merged_lawyer.MapOrigin("http://xyz.com/x", &mapped));
  EXPECT_STREQ("http://host2/x", mapped);
  ASSERT_TRUE(merged_lawyer.MapOrigin("http://xabc.com/x", &mapped));
  EXPECT_STREQ("http://host3/x", mapped);
}

TEST_F(DomainLawyerTest, ComputeSignatureTest) {
  DomainLawyer first_lawyer, second_lawyer;
  ASSERT_TRUE(first_lawyer.AddOriginDomainMapping("host1", "*abc*.com",
                                                  &message_handler_));

  ASSERT_TRUE(second_lawyer.AddRewriteDomainMapping("cdn.com",
                                                    "myhost1.com,myhost2.com",
                                                    &message_handler_));
  EXPECT_STREQ("D:http://*abc*.com/__a_O:http://host1/_-D:http://host1/__n_-",
               first_lawyer.Signature());
  EXPECT_STREQ("D:http://cdn.com/__a_-D:http://myhost1.com/__a_R:http://cdn.com"
               "/_-D:http://myhost2.com/__a_R:http://cdn.com/_-",
               second_lawyer.Signature());

  EXPECT_TRUE(first_lawyer.AddShard("domain1", "shard", &message_handler_));
  EXPECT_STREQ("D:http://*abc*.com/__a_O:http://host1/_-D:http://domain1/__a_S:"
               "http://shard/_-D:http://host1/__n_-D:http://shard/__a_R:"
               "http://domain1/_-", first_lawyer.Signature());
}

TEST_F(DomainLawyerTest, ToStringTest) {
  DomainLawyer first_lawyer, second_lawyer;
  EXPECT_TRUE(first_lawyer.AddDomain("static.example.com", &message_handler_));
  EXPECT_TRUE(first_lawyer.AddOriginDomainMapping("host1", "*abc*.com",
                                                  &message_handler_));
  EXPECT_STREQ(
      "http://*abc*.com/ Auth OriginDomain:http://host1/\n"
      "http://host1/\n"
      "http://static.example.com/ Auth\n",
      first_lawyer.ToString());

  EXPECT_TRUE(second_lawyer.AddRewriteDomainMapping("myhost.cdn.com",
                                                    "myhost1.com,myhost2.com",
                                                    &message_handler_));
  EXPECT_TRUE(
      second_lawyer.AddShard("domain1", "shard,shard2", &message_handler_));
  EXPECT_STREQ(
      "http://domain1/ Auth Shards:{http://shard/, http://shard2/}\n"
      "http://myhost.cdn.com/ Auth\n"
      "http://myhost1.com/ Auth RewriteDomain:http://myhost.cdn.com/\n"
      "http://myhost2.com/ Auth RewriteDomain:http://myhost.cdn.com/\n"
      "http://shard/ Auth RewriteDomain:http://domain1/\n"
      "http://shard2/ Auth RewriteDomain:http://domain1/\n",
      second_lawyer.ToString());
}

TEST_F(DomainLawyerTest, IsOriginKnownTest) {
  DomainLawyer lawyer;
  lawyer.AddDomain("a.com", &message_handler_);
  lawyer.AddDomain("a.com:42", &message_handler_);
  lawyer.AddDomain("https://a.com:43", &message_handler_);
  lawyer.AddRewriteDomainMapping("b.com", "c.com", &message_handler_);
  lawyer.AddOriginDomainMapping("e.com", "d.com", &message_handler_);
  lawyer.AddShard("f.com", "s1.f.com,s2.f.com", &message_handler_);

  GoogleUrl z_com("http://z.com");
  EXPECT_FALSE(lawyer.IsOriginKnown(z_com));

  GoogleUrl a_com("http://a.com");
  EXPECT_TRUE(lawyer.IsOriginKnown(a_com));

  GoogleUrl a_com_42("http://a.com:42/sardine");
  EXPECT_TRUE(lawyer.IsOriginKnown(a_com_42));

  GoogleUrl a_com_43("http://a.com:43/bass");
  EXPECT_FALSE(lawyer.IsOriginKnown(a_com_43));

  GoogleUrl s_a_com_43("https://a.com:43/bass");
  EXPECT_TRUE(lawyer.IsOriginKnown(s_a_com_43));

  GoogleUrl s_a_com_44("https://a.com:44/bass");
  EXPECT_FALSE(lawyer.IsOriginKnown(s_a_com_44));

  GoogleUrl b_com("http://b.com");
  EXPECT_TRUE(lawyer.IsOriginKnown(b_com));

  GoogleUrl c_com("http://c.com");
  EXPECT_TRUE(lawyer.IsOriginKnown(c_com));

  GoogleUrl d_com("http://d.com");
  EXPECT_TRUE(lawyer.IsOriginKnown(d_com));

  GoogleUrl e_com("http://e.com");
  EXPECT_TRUE(lawyer.IsOriginKnown(e_com));

  GoogleUrl f_com("http://f.com");
  EXPECT_TRUE(lawyer.IsOriginKnown(f_com));

  GoogleUrl s1_f_com("http://s1.f.com");
  EXPECT_TRUE(lawyer.IsOriginKnown(s1_f_com));

  GoogleUrl s2_f_com("http://s2.f.com");
  EXPECT_TRUE(lawyer.IsOriginKnown(s2_f_com));
}

}  // namespace net_instaweb
