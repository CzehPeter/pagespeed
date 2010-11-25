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
// Authors: sligocki@google.com (Shawn Ligocki),
//          lsong@google.com (Libo Song)

#ifndef NET_INSTAWEB_UTIL_PUBLIC_MD5_HASHER_H_
#define NET_INSTAWEB_UTIL_PUBLIC_MD5_HASHER_H_

#include <algorithm>  // for std::min
#include "base/basictypes.h"
#include "net/instaweb/util/public/hasher.h"

namespace net_instaweb {

class MD5Hasher : public Hasher {
 public:
  static const int kMaxHashSize;
  static const int kDefaultHashSize = 10;

  MD5Hasher() : hash_size_(kDefaultHashSize) {}
  MD5Hasher(int hash_size) {
    CHECK(hash_size >= 0);
    hash_size_ = std::min(hash_size, kMaxHashSize);
  }
  virtual ~MD5Hasher();

  virtual std::string Hash(const StringPiece& content) const;

  virtual int HashSizeInChars() const { return hash_size_; }

 private:
  int hash_size_;

  DISALLOW_COPY_AND_ASSIGN(MD5Hasher);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_MD5_HASHER_H_
