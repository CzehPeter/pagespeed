// Copyright 2010 Google Inc. All Rights Reserved.
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

#ifndef HTML_REWRITER_MD5_HASHER_H_
#define HTML_REWRITER_MD5_HASHER_H_

#include <string>

#include "base/md5.h"
#include "net/instaweb/util/public/hasher.h"

using net_instaweb::Hasher;

namespace html_rewriter {

class Md5Hasher : public Hasher {
 public:
  virtual ~Md5Hasher();

  // Interface to accummulate a hash of data.
  virtual void Reset() { MD5Init(&ctx_); }
  virtual void Add(const net_instaweb::StringPiece& content) {
    MD5Update(&ctx_, content.data(), content.size());
  }
  virtual void ComputeHash(std::string* hash);
 private:
  MD5Context ctx_;
};

}  // namespace html_rewriter

#endif  // HTML_REWRITER_MD5_HASHER_H_
