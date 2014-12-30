// Copyright 2010 Google Inc.
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
// Author: jmarantz@google.com (Joshua Marantz)

#include "pagespeed/apache/apache_config.h"

#include "base/logging.h"
#include "net/instaweb/public/version.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

namespace {

const char kModPagespeedStatisticsHandlerPath[] = "/mod_pagespeed_statistics";
const char kProxyAuth[] = "ProxyAuth";

}  // namespace

RewriteOptions::Properties* ApacheConfig::apache_properties_ = NULL;

void ApacheConfig::Initialize() {
  if (Properties::Initialize(&apache_properties_)) {
    SystemRewriteOptions::Initialize();
    AddProperties();
  }
}

void ApacheConfig::Terminate() {
  if (Properties::Terminate(&apache_properties_)) {
    SystemRewriteOptions::Terminate();
  }
}

ApacheConfig::ApacheConfig(const StringPiece& description,
                           ThreadSystem* thread_system)
    : SystemRewriteOptions(description, thread_system) {
  Init();
}

ApacheConfig::ApacheConfig(ThreadSystem* thread_system)
    : SystemRewriteOptions(thread_system) {
  Init();
}

ApacheConfig::~ApacheConfig() {
}

void ApacheConfig::Init() {
  DCHECK(apache_properties_ != NULL)
      << "Call ApacheConfig::Initialize() before construction";
  InitializeOptions(apache_properties_);
}

void ApacheConfig::AddProperties() {
  AddApacheProperty(
      false, &ApacheConfig::fetch_from_mod_spdy_, "ffms",
      RewriteOptions::kFetchFromModSpdy,
      "Fetch SSL resources with help of recent mod_spdy",
      true /* safe_to_print */);

  AddApacheProperty(
      "", &ApacheConfig::proxy_auth_, "prxa",
      kProxyAuth,
      "CookieName[=Value][:RedirectUrl] -- checks proxy requests for "
      "CookieName.  If CookieValue is specified, checks for that.  If "
      "Redirect is specified, a failure results in a redirection to that URL "
      "otherwise a 403 is generated.",
      false /* safe_to_print */);

  MergeSubclassProperties(apache_properties_);

  // Default properties are global but to set them the current API requires
  // an ApacheConfig instance and we're in a static method.
  //
  // TODO(jmarantz): Perform these operations on the Properties directly and
  // get rid of this hack.
  //
  // Instantiation of the options with a null thread system wouldn't usually be
  // safe but it's ok here because we're only updating the static properties on
  // process startup.  We won't have a thread-system yet or multiple threads.
  ApacheConfig config("dummy_options", NULL);
  config.set_default_x_header_value(kModPagespeedVersion);
}

ApacheConfig* ApacheConfig::Clone() const {
  ApacheConfig* options =
      new ApacheConfig(StrCat("cloned from ", description()), thread_system());
  options->Merge(*this);
  return options;
}

ApacheConfig* ApacheConfig::NewOptions() const {
  return new ApacheConfig(StrCat("derived from ", description()),
                          thread_system());
}

const ApacheConfig* ApacheConfig::DynamicCast(const RewriteOptions* instance) {
  const ApacheConfig* config = dynamic_cast<const ApacheConfig*>(instance);
  DCHECK(config != NULL);
  return config;
}

ApacheConfig* ApacheConfig::DynamicCast(RewriteOptions* instance) {
  ApacheConfig* config = dynamic_cast<ApacheConfig*>(instance);
  DCHECK(config != NULL);
  return config;
}

bool ApacheConfig::GetProxyAuth(StringPiece* name, StringPiece* value,
                                StringPiece* redirect) const {
  StringPiece auth = proxy_auth_.value();
  TrimWhitespace(&auth);
  if (auth.empty()) {
    return false;
  }

  // Strip the redirect off the tail if a colon is present.  Note that
  // a colon may exist in the redirect URL but we search from the beginning
  // so it's no problem.
  stringpiece_ssize_type colon = auth.find(':');
  if (colon == StringPiece::npos) {
    redirect->clear();
  } else {
    *redirect = auth.substr(colon + 1);
    auth = auth.substr(0, colon);
  }

  // Split into name/value if an equals is present.
  stringpiece_ssize_type equals = auth.find('=');
  if (equals == StringPiece::npos) {
    *name = auth;
    value->clear();
  } else {
    *name = auth.substr(0, equals);
    *value = auth.substr(equals + 1);
  }
  return true;
}

}  // namespace net_instaweb
