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

#include "net/instaweb/apache/apache_config.h"

#include "base/logging.h"
#include "net/instaweb/public/version.h"

namespace net_instaweb {

class ThreadSystem;

namespace {

const char kModPagespeedStatisticsHandlerPath[] = "/mod_pagespeed_statistics";

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
    : SystemRewriteOptions(thread_system),
      description_(description.data(), description.size()) {
  Init();
}

ApacheConfig::ApacheConfig(ThreadSystem* thread_system)
    : SystemRewriteOptions(thread_system) {
  Init();
}

void ApacheConfig::Init() {
  DCHECK(apache_properties_ != NULL)
      << "Call ApacheConfig::Initialize() before construction";
  InitializeOptions(apache_properties_);

  // Apache-specific default.
  // TODO(sligocki): Get rid of this line and let both Apache and Nginx use
  // /pagespeed_statistics as the handler.
  statistics_handler_path_.set_default(kModPagespeedStatisticsHandlerPath);
}

void ApacheConfig::AddProperties() {
  AddApacheProperty(
      false, &ApacheConfig::experimental_fetch_from_mod_spdy_, "effms",
      RewriteOptions::kExperimentalFetchFromModSpdy,
      "Under construction. Do not use");

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
  ApacheConfig config(NULL);
  config.set_default_x_header_value(kModPagespeedVersion);
}

ApacheConfig* ApacheConfig::Clone() const {
  ApacheConfig* options = new ApacheConfig(description_, thread_system());
  options->Merge(*this);
  return options;
}

ApacheConfig* ApacheConfig::NewOptions() const {
  return new ApacheConfig(thread_system());
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

}  // namespace net_instaweb
