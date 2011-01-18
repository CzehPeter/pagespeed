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

#ifndef NET_INSTAWEB_UTIL_PUBLIC_STRING_HASH_H_
#define NET_INSTAWEB_UTIL_PUBLIC_STRING_HASH_H_

#include <stddef.h>

namespace net_instaweb {

inline size_t HashString(const char* s, size_t len) {
  // TODO(jmarantz): The portability layers defined in
  //   third_party/chromium/src/base/hash_tables.h
  //   third_party/protobuf2/src/src/google/protobuf/stubs/hash.h
  // do not make it obvious how to hash a string directly -- they only
  // make it possible to instantiate hash_set<string> and hash_map<string,...>.
  // A reasonable hashing function looks available in apr_hashfunc_default,
  // but for build isolation reasons we don't want to reference that here.
  //
  // For now, define our own implemention, copied from code in
  // third_party/chromium/src/base/hash_tables.h.
  size_t result = 0;
  for (size_t i = 0; i < len; ++i, ++s) {
    result = (result * 131) + *s;
  }
  return result;
}

// Combine two hash values in a reasonable way.  Here to avoid
// excessive mysticism in the remainder of the code.
inline size_t JoinHash(size_t a, size_t b) {
  return (a + 56) * 137 + b * 151;  // Uses different prime multipliers.
}

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_STRING_HASH_H_
