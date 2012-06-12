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

// Author: pulkitg@google.com (Pulkit Goyal)

#include "net/instaweb/rewriter/public/critical_images_finder.h"

#include <map>

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

CriticalImagesFinder::CriticalImagesFinder() {
}

CriticalImagesFinder::~CriticalImagesFinder() {
}

bool CriticalImagesFinder::IsCriticalImage(
    const GoogleString& image_url, const RewriteDriver* driver) const {
  const StringSet* critical_images_set = driver->critical_images();
  return critical_images_set != NULL &&
      critical_images_set->find(image_url) != critical_images_set->end();
}

void CriticalImagesFinder::UpdateCriticalImagesSetInDriver(
    RewriteDriver* driver) {
  // Default interface is empty and derived classes can override.
}

void CriticalImagesFinder::ComputeCriticalImages(
    StringPiece url, RewriteDriver* driver, bool must_compute) {
  // Default interface is empty and derived classes can override.
}

}  // namespace net_instaweb
