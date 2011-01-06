// Copyright 2010 Google Inc. All Rights Reserved.
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

#ifndef NET_INSTAWEB_APACHE_HEADER_UTIL_H_
#define NET_INSTAWEB_APACHE_HEADER_UTIL_H_

#include "net/instaweb/util/public/meta_data.h"
// The httpd header must be after the meta_data.h.  Otherwise, the
// compiler will complain
//   "strtoul_is_not_a_portable_function_use_strtol_instead".
#include "httpd.h"

namespace net_instaweb {

// Converts Apache header structure into an Instaweb MetaData.
//
// proto_num is the version number of protocol; 1.1 = 1001
void ApacheHeaderToMetaData(const apr_table_t* apache_headers,
                            int status_code,
                            int proto_num,
                            MetaData* meta_data);

// Converts MetaData structure into an Apache header.
void MetaDataToApacheHeader(const MetaData& meta_data, request_rec* request);

// Remove downstream filters that might corrupt our caching headers.
void DisableDownstreamHeaderFilters(request_rec* request);

// Debug utility for printing Apache headers to stdout
void PrintHeaders(request_rec* request);

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_APACHE_HEADER_UTIL_H_
