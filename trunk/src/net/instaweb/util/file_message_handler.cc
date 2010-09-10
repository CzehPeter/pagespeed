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

#include "net/instaweb/util/public/file_message_handler.h"

#include <stdio.h>
#include <stdlib.h>

namespace net_instaweb {

FileMessageHandler::FileMessageHandler(FILE* file) : file_(file) {
}

void FileMessageHandler::MessageVImpl(MessageType type, const char* msg,
                                      va_list args) {
  fprintf(file_, "%s: ", MessageTypeToString(type));
  vfprintf(file_, msg, args);
  fputc('\n', file_);

  if (type == kFatal) {
    abort();
  }
}

void FileMessageHandler::FileMessageVImpl(MessageType type, const char* filename,
                                          int line, const char *msg, va_list args) {
  fprintf(file_, "%s: %s:%d: ", MessageTypeToString(type), filename, line);
  vfprintf(file_, msg, args);
  fputc('\n', file_);

  if (type == kFatal) {
    abort();
  }
}

}  // namespace net_instaweb
