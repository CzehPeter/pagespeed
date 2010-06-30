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

#include "net/instaweb/rewriter/public/file_input_resource.h"

#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/simple_meta_data.h"

namespace net_instaweb {

FileInputResource::FileInputResource(const StringPiece& url,
                                     const StringPiece& absolute_url,
                                     const StringPiece& filename,
                                     FileSystem* file_system)
    : file_system_(file_system) {
  url.CopyToString(&url_);
  absolute_url.CopyToString(&absolute_url_);
  filename.CopyToString(&filename_);
}

FileInputResource::~FileInputResource() {
}

bool FileInputResource::Read(MessageHandler* message_handler) {
  if (!loaded() &&
      file_system_->ReadFile(filename_.c_str(), &contents_, message_handler)) {
    meta_data_.reset(new SimpleMetaData());
  }
  return meta_data_.get() != NULL;
}

const MetaData* FileInputResource::metadata() const {
  return meta_data_.get();
}

}  // namespace net_instaweb
