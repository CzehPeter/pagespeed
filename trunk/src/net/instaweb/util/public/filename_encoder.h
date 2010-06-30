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

// Author: sligocki@google.com (Shawn Ligocki)

#ifndef NET_INSTAWEB_UTIL_PUBLIC_FILENAME_ENCODER_H_
#define NET_INSTAWEB_UTIL_PUBLIC_FILENAME_ENCODER_H_

#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class FilenameEncoder {
 public:
  FilenameEncoder() {}
  virtual ~FilenameEncoder();

  virtual void Encode(const std::string& filename_prefix,
                      const std::string& filename_ending,
                      std::string* encoded_filename);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_FILENAME_ENCODER_H_
