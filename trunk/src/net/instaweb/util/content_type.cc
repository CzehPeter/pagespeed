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

#include "net/instaweb/util/public/content_type.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

namespace {

const ContentType kTypes[] = {
  // Canonical types:
  {"text/javascript", ".js",  ContentType::kJavascript},
  {"text/css",        ".css", ContentType::kCss},
  {"text/plain",      ".txt", ContentType::kText},

  {"image/png",       ".png", ContentType::kPng},
  {"image/gif",       ".gif", ContentType::kGif},
  {"image/jpeg",      ".jpg", ContentType::kJpeg},

  // Synonyms; Note that the canonical types are referenced by index
  // in the named references declared below.
  {"application/x-javascript", ".js",   ContentType::kJavascript},
  {"application/javascript",   ".js",   ContentType::kJavascript},
  {"text/ecmascript",          ".js",   ContentType::kJavascript},
  {"application/ecmascript",   ".js",   ContentType::kJavascript},
  {"image/jpeg",               ".jpeg", ContentType::kJpeg},
};
const int kNumTypes = arraysize(kTypes);

}  // namespace

const ContentType& kContentTypeJavascript = kTypes[0];
const ContentType& kContentTypeCss = kTypes[1];
const ContentType& kContentTypeText = kTypes[2];

const ContentType& kContentTypePng = kTypes[3];
const ContentType& kContentTypeGif = kTypes[4];
const ContentType& kContentTypeJpeg = kTypes[5];

const ContentType* NameExtensionToContentType(const StringPiece& name) {
  // Get the name from the extension.
  StringPiece::size_type ext_pos = name.rfind('.');
  const ContentType* res = NULL;
  if (ext_pos != StringPiece::npos) {
    StringPiece ext = name.substr(ext_pos);
    std::string lower_cased_ext(ext.data(), ext.size());
    LowerString(&lower_cased_ext);

    // TODO(jmarantz): convert to a map if the list gets large.
    for (int i = 0; i < kNumTypes; ++i) {
      if (lower_cased_ext == kTypes[i].file_extension()) {
        res = &kTypes[i];
        break;
      }
    }
  }
  return res;
}

const ContentType* MimeTypeToContentType(const StringPiece& mime_type) {
  // TODO(jmarantz): convert to a map if the list gets large.
  std::string lower_cased_mime_type(mime_type.data(), mime_type.size());
  LowerString(&lower_cased_mime_type);
  const ContentType* res = NULL;
  for (int i = 0; i < kNumTypes; ++i) {
    if (lower_cased_mime_type == kTypes[i].mime_type()) {
      res = &kTypes[i];
      break;
    }
  }
  return res;
}

}  // namespace net_instaweb
