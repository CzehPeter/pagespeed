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

// Author: jmarantz@google.com (Joshua Marantz)

#ifndef NET_INSTAWEB_UTIL_PUBLIC_STDIO_FILE_SYSTEM_H_
#define NET_INSTAWEB_UTIL_PUBLIC_STDIO_FILE_SYSTEM_H_

#include "net/instaweb/util/public/file_system.h"

namespace net_instaweb {

class StdioFileSystem : public FileSystem {
 public:
  StdioFileSystem() {}
  virtual ~StdioFileSystem();

  virtual InputFile* OpenInputFile(const char* filename,
                                   MessageHandler* message_handler);
  virtual OutputFile* OpenOutputFile(const char* filename,
                                     MessageHandler* message_handler);
  virtual OutputFile* OpenTempFile(const StringPiece& prefix_name,
                                   MessageHandler* message_handle);

  virtual bool RemoveFile(const char* filename, MessageHandler* handler);
  virtual bool RenameFile(const char* old_file, const char* new_file,
                          MessageHandler* handler);
  virtual bool MakeDir(const char* directory_path, MessageHandler* handler);
  virtual BoolOrError Exists(const char* path, MessageHandler* handler);
  virtual BoolOrError IsDir(const char* path, MessageHandler* handler);

 private:
  DISALLOW_COPY_AND_ASSIGN(StdioFileSystem);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_STDIO_FILE_SYSTEM_H_
