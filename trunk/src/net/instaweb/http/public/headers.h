// Copyright 2011 Google Inc.
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

// Author: jmarantz@google.com (Joshua Marantz)

#ifndef NET_INSTAWEB_HTTP_PUBLIC_HEADERS_H_
#define NET_INSTAWEB_HTTP_PUBLIC_HEADERS_H_

#include <stdlib.h>
#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/http/public/meta_data.h"  // HttpAttributes, HttpStatus
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class MessageHandler;
class HttpResponseHeaders;
class StringMultiMapInsensitive;
class Writer;

// Read/write API for HTTP headers (shared base class)
template<class Proto> class Headers {
 public:
  Headers();
  virtual ~Headers();

  virtual void Clear();

  int major_version() const;
  bool has_major_version() const;
  int minor_version() const;
  void set_major_version(int major_version);
  void set_minor_version(int major_version);

  // Raw access for random access to attribute name/value pairs.
  int NumAttributes() const;
  const GoogleString& Name(int i) const;
  const GoogleString& Value(int i) const;

  // Note that Lookup, though declared const, is NOT thread-safe.  This
  // is because it lazily generates a map.
  //
  // TODO(jmarantz): this is a problem waiting to happen, but I believe it
  // will not be a problem in the immediate future.  We can refactor our way
  // around this problem by moving the Map to an explicit separate class that
  // can be instantiated to assist with Lookups and Remove.  But that should
  // be done in a separate CL from the one I'm typing into now.
  bool Lookup(const StringPiece& name, StringStarVector* values) const;

  // Likewise, NumAttributeNames is const but not thread-safe.
  int NumAttributeNames() const;

  // Adds a new header, even if a header with the 'name' exists already.
  virtual void Add(const StringPiece& name, const StringPiece& value);

  // Remove all headers by name.  Return true if anything was removed.
  virtual bool RemoveAll(const StringPiece& name);

  // Similar to RemoveAll followed by Add.  Note that the attribute
  // order may be changed as a side effect of this operation.
  virtual void Replace(const StringPiece& name, const StringPiece& value);

  // Serialize HTTP header to a binary stream.
  virtual bool WriteAsBinary(Writer* writer, MessageHandler* message_handler);

  // Read HTTP header from a binary string.
  virtual bool ReadFromBinary(const StringPiece& buf, MessageHandler* handler);

  // Serialize HTTP headers in HTTP format so it can be re-parsed
  virtual bool WriteAsHttp(Writer* writer, MessageHandler* handler) const;

 protected:
  void PopulateMap() const;  // the 'const' is a lie -- it mutates map_.

  // We have two represenations for the name/value pairs.  The
  // HttpResponseHeader protobuf contains a simple string-pair vector, but
  // lacks a fast associative lookup.  So we will build structures for
  // associative lookup lazily, and keep them up-to-date if they are
  // present.
  mutable scoped_ptr<StringMultiMapInsensitive> map_;
  scoped_ptr<Proto> proto_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Headers);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_HTTP_PUBLIC_HEADERS_H_
