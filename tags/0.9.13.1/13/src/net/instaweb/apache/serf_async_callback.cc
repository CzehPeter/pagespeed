// Copyright 2010 Google Inc.
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
//         lsong@google.com (Libo Song)

#include "net/instaweb/apache/serf_async_callback.h"
#include "net/instaweb/util/public/writer.h"

namespace net_instaweb {

namespace {

class ProtectedWriter : public Writer {
 public:
  ProtectedWriter(SerfAsyncCallback* callback, Writer* orig_writer)
      : callback_(callback),
        orig_writer_(orig_writer) {
  }

  virtual bool Write(const StringPiece& buf, MessageHandler* handler) {
    bool ret = true;

    // If the callback has not timed out and been released, then pass
    // the data through.
    if (!callback_->released()) {
      ret = orig_writer_->Write(buf, handler);
    }
    return ret;
  }

  virtual bool Flush(MessageHandler* handler) {
    bool ret = true;

    // If the callback has not timed out and been released, then pass
    // the flush through.
    if (!callback_->released()) {
      ret = orig_writer_->Flush(handler);
    }
    return ret;
  }

 private:
  SerfAsyncCallback* callback_;
  Writer* orig_writer_;

  DISALLOW_COPY_AND_ASSIGN(ProtectedWriter);
};

}  // namespace

SerfAsyncCallback::SerfAsyncCallback(MetaData* response_headers, Writer* writer)
    : done_(false),
      success_(false),
      released_(false),
      response_headers_(response_headers),
      writer_(new ProtectedWriter(this, writer)) {
}

SerfAsyncCallback::~SerfAsyncCallback() {
}

void SerfAsyncCallback::Done(bool success) {
  done_ = true;
  success_ = success;
  if (released_) {
    delete this;
  } else {
    response_headers_->CopyFrom(response_headers_buffer_);
  }
}

void SerfAsyncCallback::Release() {
  released_ = true;
  if (done_) {
    delete this;
  }
}

}  // namespace net_instaweb
