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

// Author: jmaessen@google.com (Jan Maessen)
//
// An input resource representing a data: url.  This is uncommon in web
// pages, but we generate these urls as a result of image inlining and
// this confuses subsequent filters in certain cases.

#include "base/scoped_ptr.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/util/public/data_url.h"

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_DATA_URL_INPUT_RESOURCE_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_DATA_URL_INPUT_RESOURCE_H_

namespace net_instaweb {

class ResourceManager;
class ContentType;
enum Encoding;

class DataUrlInputResource : public Resource {
 public:
  // We expose a factory; parse failure returns NULL.
  static DataUrlInputResource* Make(const StringPiece& url,
                                    ResourceManager* manager) {
    const ContentType* type;
    Encoding encoding;
    StringPiece encoded_contents;
    // We create the local copy of the url early, because
    // encoded_contents will in general be a substring of this
    // local copy and must have the same lifetime.
    std::string* url_copy = new std::string();
    url.CopyToString(url_copy);
    if (!ParseDataUrl(*url_copy, &type, &encoding, &encoded_contents)) {
      return NULL;
    }
    return new DataUrlInputResource(url_copy, encoding, type, encoded_contents,
                                    manager);
  }

  virtual ~DataUrlInputResource() { }

  virtual std::string url() const { return *url_.get(); }

 protected:
  virtual bool ReadIfCached(MessageHandler* message_handler);
  virtual bool IsCacheable() const;

 private:
  DataUrlInputResource(const std::string* url,
                       Encoding encoding,
                       const ContentType* type,
                       const StringPiece& encoded_contents,
                       ResourceManager* manager)
      : Resource(manager, type),
        url_(url),
        encoding_(encoding),
        encoded_contents_(encoded_contents) {
  }

  scoped_ptr<const std::string> url_;
  const Encoding encoding_;
  const StringPiece encoded_contents_;  // substring of url.
  std::string decoded_contents_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_DATA_URL_INPUT_RESOURCE_H_
