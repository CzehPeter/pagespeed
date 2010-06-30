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
//
// Input resource created based on a network resource.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_URL_INPUT_RESOURCE_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_URL_INPUT_RESOURCE_H_

#include "base/scoped_ptr.h"
#include "net/instaweb/rewriter/public/input_resource.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class MessageHandler;
class MetaData;
class UrlFetcher;

class UrlInputResource : public InputResource {
 public:
  explicit UrlInputResource(const StringPiece& url,
                            const StringPiece& absolute_url,
                            UrlFetcher* url_fetcher);
  virtual ~UrlInputResource();

  // Read complete resource, content is stored in contents_.
  virtual bool Read(MessageHandler* message_handler);

  virtual const std::string& url() const { return url_; }
  virtual const std::string& absolute_url() const { return absolute_url_; }
  virtual bool loaded() const { return meta_data_.get() != NULL; }
  // contents are only available when loaded()
  virtual bool ContentsValid() const;
  virtual const std::string& contents() const { return contents_; }
  virtual const MetaData* metadata() const { return meta_data_.get(); }

 private:
  std::string url_;
  std::string absolute_url_;
  std::string contents_;
  scoped_ptr<MetaData> meta_data_;
  UrlFetcher* url_fetcher_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_URL_INPUT_RESOURCE_H_
