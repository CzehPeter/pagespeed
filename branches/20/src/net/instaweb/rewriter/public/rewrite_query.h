// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_QUERY_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_QUERY_H_

#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class MessageHandler;
class RequestHeaders;
class RewriteOptions;
class QueryParams;

class RewriteQuery {
 public:
  // The names of query-params.
  static const char kModPagespeed[];
  static const char kModPagespeedDisableForBots[];
  static const char kModPagespeedFilters[];

  enum Status {
    kSuccess,
    kInvalid,
    kNoneFound
  };

  // Scans query parameters and request headers for "ModPagespeed"
  // flags, populating 'options' if there were any "ModPagespeed"
  // flags found, and they were all parsed successfully.  If any were
  // parsed unsuccessfully kInvalid is returned.  If none found,
  // kNoneFound is returned.
  //
  // TODO(jmarantz): consider allowing an alternative prefix to "ModPagespeed"
  // to accomodate other Page Speed Automatic applications that might want to
  // brand differently.
  static Status Scan(const QueryParams& query_params,
                     const RequestHeaders& request_headers,
                     RewriteOptions* options,
                     MessageHandler* handler);

 private:
  static Status ScanNameValue(const StringPiece& name,
                              const GoogleString& value,
                              RewriteOptions* options,
                              MessageHandler* handler);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_QUERY_H_
