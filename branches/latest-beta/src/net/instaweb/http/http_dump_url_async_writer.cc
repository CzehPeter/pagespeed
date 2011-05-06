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

// Author: sligocki@google.com (Shawn Ligocki)

#include "net/instaweb/http/public/http_dump_url_async_writer.h"

#include "net/instaweb/http/public/http_dump_url_fetcher.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "net/instaweb/http/public/url_fetcher.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/file_writer.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/writer.h"

namespace net_instaweb {

class HttpDumpUrlAsyncWriter::Fetch : UrlAsyncFetcher::Callback {
 public:
  Fetch(const GoogleString& url, const RequestHeaders& request_headers,
        ResponseHeaders* response_headers, Writer* response_writer,
        MessageHandler* handler, Callback* callback,
        const GoogleString& filename, UrlFetcher* dump_fetcher,
        FileSystem* file_system)
      : url_(url), response_headers_(response_headers),
        response_writer_(response_writer), handler_(handler),
        callback_(callback), filename_(filename), dump_fetcher_(dump_fetcher),
        file_system_(file_system), string_writer_(&contents_) {
    request_headers_.CopyFrom(request_headers);
  }

  // Like UrlAsyncFetcher::StreamingFetch, returns true if callback has been
  // called already.
  bool StartFetch(const bool accept_gzip, UrlAsyncFetcher* base_fetcher) {
    // In general we will want to always ask the origin for gzipped output,
    // but we are leaving in variable so this could be overridden by the
    // instantiator of the DumpUrlWriter.
    compress_headers_.CopyFrom(request_headers_);
    if (accept_gzip) {
      compress_headers_.Replace(HttpAttributes::kAcceptEncoding,
                                HttpAttributes::kGzip);
    }

    return base_fetcher->StreamingFetch(url_, compress_headers_,
                                         &compressed_response_, &string_writer_,
                                         handler_, this);
  }

  // Finishes the Fetch when called back.
  void Done(bool success) {
    compressed_response_.Replace(HttpAttributes::kContentLength,
                                 IntegerToString(contents_.size()));
    compressed_response_.ComputeCaching();

    // Do not write an empty file if the fetch failed.
    if (success) {
      FileSystem::OutputFile* file = file_system_->OpenTempFile(
          filename_ + ".temp", handler_);
      if (file != NULL) {
        handler_->Message(kInfo, "Storing %s as %s", url_.c_str(),
                          filename_.c_str());
        GoogleString temp_filename = file->filename();
        FileWriter file_writer(file);
        success = compressed_response_.WriteAsHttp(&file_writer, handler_) &&
            file->Write(contents_, handler_);
        success &= file_system_->Close(file, handler_);
        success &= file_system_->RenameFile(temp_filename.c_str(),
                                        filename_.c_str(),
                                        handler_);
      } else {
        success = false;
      }
    }

    // We are not going to be able to read the response from the file
    // system so we better pass the error message through.
    if (!success) {
      response_headers_->CopyFrom(compressed_response_);
      response_writer_->Write(contents_, handler_);
    } else {
      // Let dump fetcher fetch the actual response so that it can decompress.
      success = dump_fetcher_->StreamingFetchUrl(
          url_, request_headers_, response_headers_, response_writer_,
          handler_);
    }

    callback_->Done(success);
    delete this;
  }

 private:
  const GoogleString url_;
  RequestHeaders request_headers_;
  ResponseHeaders* response_headers_;
  Writer* response_writer_;
  MessageHandler* handler_;
  Callback* callback_;

  const GoogleString filename_;
  UrlFetcher* dump_fetcher_;
  FileSystem* file_system_;

  GoogleString contents_;
  StringWriter string_writer_;
  RequestHeaders compress_headers_;
  ResponseHeaders compressed_response_;

  DISALLOW_COPY_AND_ASSIGN(Fetch);
};

HttpDumpUrlAsyncWriter::~HttpDumpUrlAsyncWriter() {
}

bool HttpDumpUrlAsyncWriter::StreamingFetch(
    const GoogleString& url, const RequestHeaders& request_headers,
    ResponseHeaders* response_headers, Writer* response_writer,
    MessageHandler* handler, Callback* callback) {
  GoogleString filename;
  GoogleUrl gurl(url);
  dump_fetcher_.GetFilename(gurl, &filename, handler);

  if (file_system_->Exists(filename.c_str(), handler).is_true()) {
    bool success = dump_fetcher_.StreamingFetchUrl(
        url, request_headers, response_headers, response_writer, handler);
    callback->Done(success);
    return true;
  } else {
    Fetch* fetch = new Fetch(url, request_headers, response_headers,
                             response_writer, handler, callback, filename,
                             &dump_fetcher_, file_system_);
    return fetch->StartFetch(accept_gzip_, base_fetcher_);
  }
}

}  // namespace net_instaweb
