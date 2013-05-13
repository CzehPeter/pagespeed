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

#include "net/instaweb/http/public/http_dump_url_writer.h"

#include "base/logging.h"
#include "net/instaweb/http/public/http_dump_url_fetcher.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/url_fetcher.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/file_writer.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gzip_inflater.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/writer.h"
#include "net/instaweb/util/stack_buffer.h"

namespace net_instaweb {

HttpDumpUrlWriter::~HttpDumpUrlWriter() {
}

bool HttpDumpUrlWriter::StreamingFetchUrl(
    const GoogleString& url, const RequestHeaders& request_headers,
    ResponseHeaders* response_headers, Writer* response_writer,
    MessageHandler* handler, const RequestContextPtr& request_context) {
  bool ret = true;
  GoogleString filename;

  GoogleUrl gurl(url);
  if (!dump_fetcher_.GetFilename(gurl, &filename, handler)) {
    handler->Message(kError, "HttpDumpUrlWriter: Invalid url: %s", url.c_str());
    ret = false;
  } else if (!file_system_->Exists(filename.c_str(), handler).is_true()) {
    // Do the Fetch first, before opening the output file, so that if the
    // fetch fails, do not make an empty file.
    //
    // TODO(jmarantz): Re-integrate the use of SplitWriter.  We'll have
    // to do a lazy-open of the OutputFile* in a custom writer, though, to
    // avoid opening up a zero-size file when the URL fetch fails.
    GoogleString contents;
    StringWriter string_writer(&contents);
    // TODO(sligocki): Have this actually stream to response_writer.

    // In general we will want to always ask the origin for gzipped output,
    // but we are leaving in variable so this could be overridden by the
    // instantiator of the DumpUrlWriter.
    RequestHeaders compress_headers;
    ResponseHeaders compressed_response;
    compress_headers.CopyFrom(request_headers);
    if (accept_gzip_) {
      compress_headers.Replace(HttpAttributes::kAcceptEncoding,
                               HttpAttributes::kGzip);
    }
    ret = base_fetcher_->StreamingFetchUrl(url, compress_headers,
                                           &compressed_response, &string_writer,
                                           handler, request_context);
    compressed_response.Replace(HttpAttributes::kContentLength,
                                IntegerToString(contents.size()));
    // TODO(jmarantz): http://tools.ietf.org/html/rfc2616#section-13.5.1
    // tells us we can also remove Keep-Alive, Proxy-Authenticate,
    // Proxy-Authorization, TE, Trailers, Transfer-Encoding, and Upgrade.
    compressed_response.RemoveAll(HttpAttributes::kConnection);
    compressed_response.ComputeCaching();

    // Do not write an empty file if the fetch failed.
    if (ret) {
      // Check to see if a response marked as gzipped are really unzippable.
      if (compressed_response.WasGzippedLast()) {
        GzipInflater inflater(GzipInflater::kGzip);
        inflater.Init();
        if (contents.data() == NULL || contents.size() == 0) {
          compressed_response.Remove(HttpAttributes::kContentEncoding,
                                     HttpAttributes::kGzip);
        } else {
          CHECK(inflater.SetInput(contents.data(), contents.size()));
          while (inflater.HasUnconsumedInput()) {
            char buf[kStackBufferSize];
            if ((inflater.InflateBytes(buf, sizeof(buf)) == 0) ||
                inflater.error()) {
              compressed_response.RemoveAll(HttpAttributes::kContentEncoding);
              break;
            }
          }
        }
      }

      FileSystem::OutputFile* file = file_system_->OpenTempFile(
          filename + ".temp", handler);
      if (file != NULL) {
        handler->Message(kInfo, "Storing %s as %s", url.c_str(),
                     filename.c_str());
        GoogleString temp_filename = file->filename();
        FileWriter file_writer(file);
        ret = compressed_response.WriteAsHttp(&file_writer, handler) &&
            file->Write(contents, handler);
        ret &= file_system_->Close(file, handler);
        ret &= file_system_->RenameFile(temp_filename.c_str(), filename.c_str(),
                                        handler);
      } else {
        ret = false;
      }
    }

    // We are not going to be able to read the response from the file
    // system so we better pass the error message through.
    if (!ret) {
      response_headers->CopyFrom(compressed_response);
      if (!response_headers->headers_complete()) {
        response_headers->SetStatusAndReason(HttpStatus::kNotFound);
        response_headers->ComputeCaching();
        // TODO(jmarantz): set_headers_complete
        // response_headers->set_headers_complete(true);
      }
      response_writer->Write(contents, handler);
    }
  }

  // Always use the HttpDumpUrlFetcher, even if we are reading the file
  // ourselves.  Thus the problem of inflating gzipped requests for requesters
  // that want cleartext only is solved only in that file.
  return ret && dump_fetcher_.StreamingFetchUrl(
      url, request_headers, response_headers, response_writer, handler,
      request_context);
}

}  // namespace net_instaweb
