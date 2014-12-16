/*
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
//

#include "pagespeed/kernel/base/gtest.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>  // For getpid()
#include <vector>
#include "pagespeed/kernel/base/stack_buffer.h"
#include "pagespeed/kernel/base/string_util.h"

#include "pagespeed/kernel/base/string.h"

namespace net_instaweb {

GoogleString GTestSrcDir() {
  // Climb up the directory hierarchy till we find "src".
  // TODO(jmarantz): check to make sure we are not in a subdirectory of
  // our top-level 'src' named src.

  char cwd[kStackBufferSize];
  CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
  StringPieceVector components;
  SplitStringPieceToVector(cwd, "/", &components, true);
  int level = components.size();
  bool found = false;
  GoogleString src_dir;
  for (int i = level - 1; i >= 0; --i) {
    if (components[i] == "src") {
      level = i + 1;
      found = true;
      break;
    }
  }
  if (found) {
    for (int i = 0; i < level; ++i) {
      src_dir += "/";
      components[i].AppendToString(&src_dir);
    }
  } else {
    // Try going down the directory structure to see if we can find "src".
    // Just go down one layer, in case there are multiple clients with
    // multiple src dirs from where you are.
    src_dir += cwd;
    src_dir += "/src";
    struct stat file_info;
    // Attempt to get the file attributes
    int ret = stat(src_dir.c_str(), &file_info);
    if (ret == 0 && S_ISDIR(file_info.st_mode)) {
      found = true;
    }
  }
  CHECK(found) << "Cannot find 'src' directory from cwd=" << cwd;
  return src_dir;
}

GoogleString GTestTempDir() {
  return StringPrintf("/tmp/gtest.%d", getpid());
}


}  // namespace net_instaweb
