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

#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/message_handler.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/stack_buffer.h"

namespace net_instaweb {

FileSystem::~FileSystem() {
}

FileSystem::File::~File() {
}

FileSystem::InputFile::~InputFile() {
}

FileSystem::OutputFile::~OutputFile() {
}

bool FileSystem::ReadFile(const char* filename, std::string* buffer,
                          MessageHandler* message_handler) {
  InputFile* input_file = OpenInputFile(filename, message_handler);
  bool ret = false;
  if (input_file != NULL) {
    char buf[kStackBufferSize];
    int nread;
    while ((nread = input_file->Read(buf, sizeof(buf), message_handler)) > 0) {
      buffer->append(buf, nread);
    }
    ret = (nread == 0);
    ret &= Close(input_file, message_handler);
  }
  return ret;
}

bool FileSystem::WriteFile(const char* filename, const StringPiece& buffer,
                           MessageHandler* message_handler) {
  OutputFile* output_file = OpenOutputFile(filename, message_handler);
  bool ret = false;
  if (output_file != NULL) {
    ret = output_file->Write(buffer, message_handler);
    ret &= output_file->SetWorldReadable(message_handler);
    ret &= Close(output_file, message_handler);
  }
  return ret;
}

bool FileSystem::WriteTempFile(const StringPiece& prefix_name,
                               const StringPiece& buffer,
                               std::string* filename,
                               MessageHandler* message_handler) {
  OutputFile* output_file = OpenTempFile(prefix_name, message_handler);
  bool ok = (output_file != NULL);
  if (ok) {
    // Store filename early, since it's invalidated by Close.
    *filename = output_file->filename();
    ok = output_file->Write(buffer, message_handler);
    // attempt Close even if write fails.
    ok &= Close(output_file, message_handler);
  }
  if (!ok) {
    // Clear filename so we end in a consistent state.
    filename->clear();
  }
  return ok;
}

bool FileSystem::Close(File* file, MessageHandler* message_handler) {
  bool ret = file->Close(message_handler);
  delete file;
  return ret;
}


bool FileSystem::RecursivelyMakeDir(const StringPiece& full_path_const,
                                    MessageHandler* handler) {
  bool ret = true;
  std::string full_path = full_path_const.as_string();
  EnsureEndsInSlash(&full_path);
  std::string subpath;
  size_t old_pos = 0, new_pos;
  // Note that we intentionally start searching at pos = 1 to avoid having
  // subpath be "" on absolute paths.
  while ((new_pos = full_path.find('/', old_pos + 1)) != std::string::npos) {
    // Build up path, one segment at a time.
    subpath.append(full_path.data() + old_pos, new_pos - old_pos);
    if (Exists(subpath.c_str(), handler).is_false()) {
      if (!MakeDir(subpath.c_str(), handler)) {
        ret = false;
        break;
      }
    } else if (IsDir(subpath.c_str(), handler).is_false()) {
      handler->Message(kError, "Subpath '%s' of '%s' is a non-directory file.",
                       subpath.c_str(), full_path.c_str());
      ret = false;
      break;
    }
    old_pos = new_pos;
  }
  return ret;
}

}  // namespace net_instaweb
