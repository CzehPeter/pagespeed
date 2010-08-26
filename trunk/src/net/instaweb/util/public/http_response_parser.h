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

#ifndef NET_INSTAWEB_UTIL_PUBLIC_HTTP_RESPONSE_PARSER_H_
#define NET_INSTAWEB_UTIL_PUBLIC_HTTP_RESPONSE_PARSER_H_

// TODO(sligocki): Find a way to forward declare FileSystem::InputFile.
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class MessageHandler;
class MetaData;
class Writer;

// Helper class to fascilitate parsing a raw streaming HTTP response including
// headers and body.
class HttpResponseParser {
 public:
  HttpResponseParser(MetaData* response_headers, Writer* writer,
                     MessageHandler* handler)
      : reading_headers_(true),
        ok_(true),
        response_headers_(response_headers),
        writer_(writer),
        handler_(handler) {
  }

  // Parse complete HTTP response from a file.
  bool ParseFile(FileSystem::InputFile* file);

  // Parse complete HTTP response from a FILE stream.
  // TODO(sligocki): We need a Readable abstraction (like Writer)
  bool Parse(FILE* stream);

  // Read a chunk of HTTP response, populating response_headers and call
  // writer on output body, returning true if the status is ok.
  bool ParseChunk(const StringPiece& data);

  bool ok() const { return ok_; }

 private:
  bool reading_headers_;
  bool ok_;
  MetaData* response_headers_;
  Writer* writer_;
  MessageHandler* handler_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_HTTP_RESPONSE_PARSER_H_
