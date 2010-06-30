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

#ifndef NET_INSTAWEB_UTIL_PUBLIC_CACHE_INTERFACE_H_
#define NET_INSTAWEB_UTIL_PUBLIC_CACHE_INTERFACE_H_

#include <string>

namespace net_instaweb {

class MessageHandler;
class Writer;

// Abstract interface for a cache.
class CacheInterface {
 public:
  enum KeyState {
    kAvailable,    // Requested key is available for serving
    kInTransit,    // Requested key is being written, but is not readable
    kNotFound      // Requested key needs to be written
  };

  virtual ~CacheInterface();
  virtual bool Get(const std::string& key, Writer* writer,
                   MessageHandler* message_handler) = 0;
  virtual void Put(const std::string& key, const std::string& value,
                   MessageHandler* message_handler) = 0;
  virtual void Delete(const std::string& key,
                      MessageHandler* message_handler) = 0;
  virtual KeyState Query(const std::string& key,
                         MessageHandler* message_handler) = 0;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_CACHE_INTERFACE_H_
