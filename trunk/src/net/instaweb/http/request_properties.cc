// Copyright 2013 Google Inc. All Rights Reserved.
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

#include "net/instaweb/http/public/request_properties.h"

#include <vector>

#include "net/instaweb/http/public/device_properties.h"
#include "net/instaweb/http/public/user_agent_matcher.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

RequestProperties::RequestProperties(UserAgentMatcher* matcher)
    : device_properties_(new DeviceProperties(matcher)),
      supports_image_inlining_(kNotSet),
      supports_js_defer_(kNotSet),
      supports_lazyload_images_(kNotSet),
      supports_webp_(kNotSet),
      supports_webp_lossless_alpha_(kNotSet) {
}

RequestProperties::~RequestProperties() {
}

void RequestProperties::set_user_agent(const StringPiece& user_agent_string) {
  device_properties_->set_user_agent(user_agent_string);
}

bool RequestProperties::SupportsImageInlining() const {
  if (supports_image_inlining_ == kNotSet) {
    supports_image_inlining_ =
        device_properties_->SupportsImageInlining() ? kTrue : kFalse;
  }
  return (supports_image_inlining_ == kTrue);
}

bool RequestProperties::SupportsLazyloadImages() const {
  if (supports_lazyload_images_ == kNotSet) {
    supports_lazyload_images_ =
        device_properties_->SupportsLazyloadImages() ? kTrue : kFalse;
  }
  return (supports_lazyload_images_ == kTrue);
}

bool RequestProperties::SupportsCriticalImagesBeacon() const {
  // For now this script has the same user agent requirements as image inlining,
  // however that could change in the future if more advanced JS is used by the
  // beacon.
  return device_properties_->SupportsCriticalImagesBeacon();
}

// Note that the result of the function is cached as supports_js_defer_. This
// must be cleared before calling the function a second time with a different
// value for allow_mobile.
bool RequestProperties::SupportsJsDefer(bool allow_mobile) const {
  if (supports_js_defer_ == kNotSet) {
    supports_js_defer_ =
        device_properties_->SupportsJsDefer(allow_mobile) ? kTrue : kFalse;
  }
  return (supports_js_defer_ == kTrue);
}

bool RequestProperties::SupportsWebp() const {
  if (supports_webp_ == kNotSet) {
    supports_webp_ =
        device_properties_->SupportsWebp() ? kTrue : kFalse;
  }
  return (supports_webp_ == kTrue);
}

bool RequestProperties::SupportsWebpLosslessAlpha() const {
  if (supports_webp_lossless_alpha_ == kNotSet) {
    supports_webp_lossless_alpha_ =
        device_properties_->SupportsWebpLosslessAlpha() ? kTrue : kFalse;
  }
  return (supports_webp_lossless_alpha_ == kTrue);
}

bool RequestProperties::IsBot() const {
  return device_properties_->IsBot();
}

bool RequestProperties::IsMobile() const {
  return device_properties_->IsMobile();
}

bool RequestProperties::SupportsSplitHtml(bool allow_mobile) const {
  return device_properties_->SupportsSplitHtml(allow_mobile);
}

bool RequestProperties::CanPreloadResources() const {
  // TODO(anupama): Why do we not use a lazybool for this?
  return device_properties_->CanPreloadResources();
}

bool RequestProperties::GetScreenResolution(int* width, int* height) const {
  return device_properties_->GetScreenResolution(width, height);
}

void RequestProperties::SetScreenResolution(int width, int height) const {
  device_properties_->SetScreenResolution(width, height);
}

UserAgentMatcher::DeviceType RequestProperties::GetDeviceType() const {
  return device_properties_->GetDeviceType();
}

void RequestProperties::SetPreferredImageQualities(
    const std::vector<int>* webp, const std::vector<int>* jpeg) {
  device_properties_->SetPreferredImageQualities(webp, jpeg);
}


bool RequestProperties::GetPreferredImageQualities(
    DeviceProperties::ImageQualityPreference preference, int* webp, int* jpeg)
    const {
  return device_properties_->GetPreferredImageQualities(preference, webp, jpeg);
}


int RequestProperties::GetPreferredImageQualityCount() {
  return DeviceProperties::GetPreferredImageQualityCount();
}

}  // namespace net_instaweb
