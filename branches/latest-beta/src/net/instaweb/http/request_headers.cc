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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/http/public/request_headers.h"

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/http/http.pb.h"  // for HttpRequestHeaders
#include "net/instaweb/http/public/headers.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_multi_map.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/writer.h"

namespace net_instaweb {

class MessageHandler;

RequestHeaders::RequestHeaders() {
  proto_.reset(new HttpRequestHeaders);
}

void RequestHeaders::Clear() {
  Headers<HttpRequestHeaders>::Clear();
  map_.reset(NULL);
  proto_->Clear();
}

void RequestHeaders::CopyFrom(const RequestHeaders& other) {
  map_.reset(NULL);
  proto_->CopyFrom(*other.proto_);
}

GoogleString RequestHeaders::ToString() const {
  GoogleString str;
  StringWriter writer(&str);
  WriteAsHttp("", &writer, NULL);
  return str;
}

// To avoid having every transitive dependency pull in the generated
// protobuf header file during compilation, we have a distinct enum
// for the RequestHeaders interface class.  We translate with switch
// statements rather than array lookups just so we don't have to bother
// initializing the array.
void RequestHeaders::set_method(Method method) {
  switch (method) {
    case kOptions:     proto_->set_method(HttpRequestHeaders::OPTIONS); break;
    case kGet:         proto_->set_method(HttpRequestHeaders::GET);     break;
    case kHead:        proto_->set_method(HttpRequestHeaders::HEAD);    break;
    case kPost:        proto_->set_method(HttpRequestHeaders::POST);    break;
    case kPut:         proto_->set_method(HttpRequestHeaders::PUT);     break;
    case kDelete:      proto_->set_method(HttpRequestHeaders::DELETE);  break;
    case kTrace:       proto_->set_method(HttpRequestHeaders::TRACE);   break;
    case kConnect:     proto_->set_method(HttpRequestHeaders::CONNECT); break;
    case kPatch:       proto_->set_method(HttpRequestHeaders::PATCH);   break;
    case kError:       proto_->set_method(HttpRequestHeaders::INVALID); break;
  }
}

RequestHeaders::Method RequestHeaders::method() const {
  switch (proto_->method()) {
    case HttpRequestHeaders::OPTIONS:     return kOptions;
    case HttpRequestHeaders::GET:         return kGet;
    case HttpRequestHeaders::HEAD:        return kHead;
    case HttpRequestHeaders::POST:        return kPost;
    case HttpRequestHeaders::PUT:         return kPut;
    case HttpRequestHeaders::DELETE:      return kDelete;
    case HttpRequestHeaders::TRACE:       return kTrace;
    case HttpRequestHeaders::CONNECT:     return kConnect;
    case HttpRequestHeaders::PATCH:       return kPatch;
    case HttpRequestHeaders::INVALID:     return kError;
  }
  DLOG(FATAL) << "Invalid method";
  return kGet;
}

const char* RequestHeaders::method_string() const {
  switch (proto_->method()) {
    case HttpRequestHeaders::OPTIONS:     return "OPTIONS";
    case HttpRequestHeaders::GET:         return "GET";
    case HttpRequestHeaders::HEAD:        return "HEAD";
    case HttpRequestHeaders::POST:        return "POST";
    case HttpRequestHeaders::PUT:         return "PUT";
    case HttpRequestHeaders::DELETE:      return "DELETE";
    case HttpRequestHeaders::TRACE:       return "TRACE";
    case HttpRequestHeaders::CONNECT:     return "CONNECT";
    case HttpRequestHeaders::PATCH:       return "PATCH";
    case HttpRequestHeaders::INVALID:     return "ERROR";
  }
  DLOG(FATAL) << "Invalid method";
  return NULL;
}

const GoogleString& RequestHeaders::message_body() const {
  return proto_->message_body();
}

void RequestHeaders::set_message_body(const GoogleString& data) {
  proto_->set_message_body(data);
}

// Serialize meta-data to a binary stream.
bool RequestHeaders::WriteAsHttp(
    const StringPiece& url, Writer* writer, MessageHandler* handler) const {
  bool ret = true;
  GoogleString buf = StringPrintf("%s %s HTTP/%d.%d\r\n",
                                  method_string(), url.as_string().c_str(),
                                  major_version(), minor_version());
  ret &= writer->Write(buf, handler);
  ret &= Headers<HttpRequestHeaders>::WriteAsHttp(writer, handler);
  return ret;
}

bool RequestHeaders::AcceptsGzip() const {
  ConstStringStarVector v;
  if (Lookup(HttpAttributes::kAcceptEncoding, &v)) {
    for (int i = 0, nv = v.size(); i < nv; ++i) {
      StringPieceVector encodings;
      SplitStringPieceToVector(*(v[i]), ",", &encodings, true);
      for (int j = 0, nencodings = encodings.size(); j < nencodings; ++j) {
        if (StringCaseEqual(encodings[j], HttpAttributes::kGzip)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool RequestHeaders::IsXmlHttpRequest() const {
  // Check if kXRequestedWith header is present to determine whether it is
  // XmlHttpRequest or not.
  // Note: Not every ajax request sends this header but many libraries like
  // jquery, prototype and mootools etc. send this header. Google closure and
  // custom ajax hacks will not set this header.
  // It is not guaranteed that javascript present in the html loaded via
  // ajax request will execute.
  const char* x_requested_with = Lookup1(HttpAttributes::kXRequestedWith);
  if (x_requested_with != NULL &&
      StringCaseEqual(x_requested_with, HttpAttributes::kXmlHttpRequest)) {
    return true;
  }
  return false;
}

}  // namespace net_instaweb
