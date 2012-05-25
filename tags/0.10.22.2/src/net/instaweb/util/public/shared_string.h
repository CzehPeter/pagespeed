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
//
// Implements a ref-counted string class, with full sharing.  This
// class does *not* implement copy-on-write semantics, however, it
// does support a unique() method for determining, prior to writing,
// whether other references exist.  Thus it is feasible to implement
// copy-on-write as a layer over this class.

#ifndef NET_INSTAWEB_UTIL_PUBLIC_SHARED_STRING_H_
#define NET_INSTAWEB_UTIL_PUBLIC_SHARED_STRING_H_

#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

// Reference-counted string.  This class just adds the StringPiece constructor.
class SharedString : public RefCountedObj<GoogleString> {
 public:
  SharedString() {}
  explicit SharedString(const StringPiece& str) {
    str.CopyToString(get());
  }

  // When constructing with a GoogleString, we going through the StringPiece
  // ctor above causes an extra copy compared with string implementations that
  // use copy-on-write.
  explicit SharedString(const GoogleString& str)
      : RefCountedObj<GoogleString>(str) {}

  // Given the two constructors above, it is ambiguous which one gets
  // called when passed a string-literal, so making an explicit const char*
  // constructor eliminates the ambiguity.  This is likely beneficial mostly
  // for tests.
  explicit SharedString(const char* str) {
    *get() = str;
  }
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_SHARED_STRING_H_
