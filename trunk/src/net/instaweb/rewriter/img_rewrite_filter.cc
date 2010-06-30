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

// Author: jmaessen@google.com (Jan Maessen)

#include "net/instaweb/rewriter/public/img_rewrite_filter.h"

#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/rewriter/public/image.h"
#include "net/instaweb/rewriter/public/img_tag_scanner.h"
#include "net/instaweb/rewriter/public/input_resource.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/rewrite.pb.h"  // for ImgRewriteUrl
#include "net/instaweb/util/public/atom.h"
#include "net/instaweb/util/public/content_type.h"
#include "net/instaweb/util/public/file_system.h"
#include <string>
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/meta_data.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string_writer.h"

namespace net_instaweb {

// Rewritten image must be < kMaxRewrittenRatio * origSize to be worth
// rewriting.
// TODO(jmaessen): Make this ratio adjustable.
const double kMaxRewrittenRatio = 1.0;

// Re-scale image if area / originalArea < kMaxAreaRatio
// Should probably be much less than 1 due to jpeg quality loss.
// Might need to differ depending upon img format.
// TODO(jmaessen): Make adjustable.
const double kMaxAreaRatio = 1.0;

// Should we log each image element as we encounter it?  Handy for debug.
// TODO(jmaessen): Hook into event logging infrastructure.
const bool kLogImageElements = false;

ImgRewriteFilter::ImgRewriteFilter(StringPiece path_prefix,
                                   HtmlParse* html_parse,
                                   ResourceManager* resource_manager,
                                   FileSystem* file_system)
    : RewriteFilter(path_prefix),
      file_system_(file_system),
      html_parse_(html_parse),
      img_filter_(new ImgTagScanner(html_parse)),
      resource_manager_(resource_manager),
      s_width_(html_parse->Intern("width")),
      s_height_(html_parse->Intern("height")),
      rewrite_count_(NULL),
      rewrite_saved_bytes_(NULL) {
  Statistics* stats = resource_manager_->statistics();
  if (stats != NULL) {
    rewrite_count_ = stats->AddVariable("image_rewrites");
    rewrite_saved_bytes_ = stats->AddVariable("image_rewrite_saved_bytes");
  }
}

void ImgRewriteFilter::OptimizeImage(
    const ImgRewriteUrl& url_proto, Image* image, OutputResource* result) {
  int img_width, img_height, width, height;
  if (url_proto.has_width() && url_proto.has_height() &&
      image->Dimensions(&img_width, &img_height)) {
    width = url_proto.width();
    height = url_proto.height();
    int64 area = static_cast<int64>(width) * height;
    int64 img_area = static_cast<int64>(img_width) * img_height;
    if (area < img_area * kMaxAreaRatio) {
      if (image->ResizeTo(width, height)) {
          html_parse_->InfoHere("Resized from %dx%d to %dx%d",
                                img_width, img_height, width, height);
      } else {
          html_parse_->InfoHere("Couldn't resize from %dx%d to %dx%d",
                                img_width, img_height, width, height);
      }
    } else if (area < img_area) {
      html_parse_->InfoHere("Not worth resizing from %dx%d to %dx%d",
                            img_width, img_height, width, height);
    }
  }
  // Unconditionally write resource back so we don't re-attempt optimization.
  MessageHandler* message_handler = html_parse_->message_handler();
  if (image->output_size() < image->input_size() * kMaxRewrittenRatio) {
    Writer* writer = result->BeginWrite(message_handler);
    if (writer != NULL) {
      image->WriteTo(writer);
      result->EndWrite(writer, message_handler);
    }
    if (rewrite_saved_bytes_ != NULL) {
      // Note: if we are serving a request from a different server
      // than the server that rewrote the <img> tag, and they don't
      // share a file system, then we will be bumping the byte-count
      // here without bumping the rewrite count.  This seems ok,
      // though perhaps we may need to revisit.
      //
      // Currently this will be a problem even when serving on a
      // different file that *does* share a filesystem,
      // HashResourceManager does not yet load its internal map
      // by scanning the filesystemon startup.
      rewrite_saved_bytes_->Add(image->input_size() - image->output_size());
    }
  } else {
    // Write nothing and set status code to indicate not to rewrite
    // in future.
    result->metadata()->set_status_code(HttpStatus::INTERNAL_SERVER_ERROR);
    Writer* writer = result->BeginWrite(message_handler);
    if (writer != NULL) {
      result->EndWrite(writer, message_handler);
    }
  }
}

OutputResource* ImgRewriteFilter::OptimizedImageFor(
    const ImgRewriteUrl& url_proto,
    const std::string& url_string,
    InputResource* img_resource) {
  OutputResource* result = NULL;
  MessageHandler* message_handler = html_parse_->message_handler();
  if (img_resource == NULL) {
    html_parse_->WarningHere("no input resource for %s",
                             url_proto.origin_url().c_str());
  } else if (!img_resource->Read(message_handler)) {
    html_parse_->WarningHere("%s wasn't loaded",
                             img_resource->url().c_str());
  } else if (!img_resource->ContentsValid()) {
    html_parse_->WarningHere("Img contents from %s are invalid.",
                             img_resource->url().c_str());
  } else {
    // TODO(jmaessen): Be even lazier about resource loading!
    // [hard b/c of content type; right now this loads whole file, whereas we
    // can learn image type from the first few bytes of the file.]
    Image image = Image(img_resource->contents(), img_resource->url(),
                        resource_manager_->file_prefix(), file_system_,
                        message_handler);
    // TODO(jmaessen): content type can change after re-compression.
    const ContentType* content_type = image.content_type();
    if (content_type != NULL) {
      result = resource_manager_->NamedOutputResource(
          filter_prefix_, url_string, *content_type);
    }
    if (result != NULL && !result->IsWritten()) {
      OptimizeImage(url_proto, &image, result);
    }
  }
  return result;
}

void ImgRewriteFilter::RewriteImageUrl(const HtmlElement& element,
                                       HtmlElement::Attribute* src) {
  // TODO(jmaessen): content type can change after re-compression.
  // How do we deal with that given only URL?
  // Separate input and output content type?
  int width, height;
  ImgRewriteUrl rewrite_url;
  std::string rewritten_url;
  MessageHandler* message_handler = html_parse_->message_handler();
  InputResource* input_resource =
      resource_manager_->CreateInputResource(src->value(), message_handler);
  if (input_resource != NULL) {
    // Always rewrite to absolute url used to obtain resource.
    // This lets us do context-free fetches of content.
    rewrite_url.set_origin_url(input_resource->absolute_url());
    OutputResource* output_resource;
    if (element.IntAttributeValue(s_width_, &width) &&
        element.IntAttributeValue(s_height_, &height)) {
      // Specific image size is called for.  Rewrite to that size.
      rewrite_url.set_width(width);
      rewrite_url.set_height(height);
    }
    Encode(rewrite_url, &rewritten_url);
    output_resource =
        OptimizedImageFor(rewrite_url, rewritten_url, input_resource);
    if (output_resource != NULL && output_resource->IsWritten() &&
        output_resource->metadata()->status_code() == HttpStatus::OK) {
      html_parse_->InfoHere("%s remapped to %s",
                            src->value(),
                            output_resource->url().as_string().c_str());
      src->set_value(output_resource->url());
      if (rewrite_count_ != NULL) {
        rewrite_count_->Add(1);
      }
    }
  }
}

void ImgRewriteFilter::EndElement(HtmlElement* element) {
  HtmlElement::Attribute *src = img_filter_->ParseImgElement(element);
  if (src != NULL) {
    if (kLogImageElements) {
      // We now know that element is an img tag.
      // Log the element in its original form.
      std::string tagstring;
      element->ToString(&tagstring);
      html_parse_->Info(
          html_parse_->filename(), element->begin_line_number(),
          "Found image: %s", tagstring.c_str());
    }
    RewriteImageUrl(*element, src);
  }
}

void ImgRewriteFilter::Flush() {
  // TODO(jmaessen): wait here for resources to have been rewritten??
}

bool ImgRewriteFilter::Fetch(StringPiece url,
                             Writer* writer,
                             const MetaData& request_header,
                             MetaData* response_headers,
                             UrlAsyncFetcher* fetcher,
                             MessageHandler* message_handler,
                             UrlAsyncFetcher::Callback* callback) {
  bool ok = true;
  const char* failure_reason = "";
  const ContentType *content_type =
      NameExtensionToContentType(url);
  if (content_type != NULL) {
    StringPiece stripped_url =
        url.substr(0, url.size() - strlen(content_type->file_extension()));
    ImgRewriteUrl url_proto;
    if (Decode(stripped_url, &url_proto)) {
      std::string stripped_url_string = stripped_url.as_string();
      InputResource* input_image = resource_manager_->CreateInputResource(
          url_proto.origin_url(), message_handler);
      OutputResource* image_resource = OptimizedImageFor(
          url_proto, stripped_url_string, input_image);
      if (image_resource != NULL) {
        assert(image_resource->IsWritten());
        if (image_resource->Read(writer, response_headers, message_handler)) {
          resource_manager_->SetDefaultHeaders(*content_type, response_headers);
          callback->Done(true);
        } else {
          ok = false;
          failure_reason = "Server could not read image resource.";
        }
        if (image_resource->metadata()->status_code() != HttpStatus::OK) {
          // Note that this should not happen, because the url should not have
          // escaped into the wild.  We're content serving an empty response if
          // it does.  We *could* serve / redirect to the origin_url as a fail
          // safe, but it's probably not worth it.  Instead we log and hope that
          // this causes us to find and fix the problem.
          message_handler->Error(url.as_string().c_str(), 0,
                                 "Rewriting of %s rejected, "
                                 "but URL requested (mistaken rewriting?).",
                                 url_proto.origin_url().c_str());
        }
      } else {
        ok = false;
        failure_reason = "Server could not find source image.";
      }
      // Image processing has failed, forward the original image data.
      if (!ok && input_image != NULL) {
        if (input_image->ContentsValid()) {
          ok = writer->Write(input_image->contents(), message_handler);
        }
        if (ok) {
          resource_manager_->SetDefaultHeaders(*content_type, response_headers);
        } else {
          message_handler->Error(url.as_string().c_str(), 0, failure_reason);
          ok = writer->Write("<img src=\"", message_handler);
          ok &= writer->Write(url_proto.origin_url(), message_handler);
          ok &= writer->Write("\" alt=\"Temporarily Moved\"/>",
                              message_handler);
          response_headers->set_major_version(1);
          response_headers->set_minor_version(1);
          response_headers->set_status_code(HttpStatus::TEMPORARY_REDIRECT);
          response_headers->set_reason_phrase("Temporary redirect");
          response_headers->Add("Location", url_proto.origin_url().c_str());
          response_headers->Add("Content-Type", "text/html");
        }
        if (ok) {
          callback->Done(true);
        }
      }
    } else {
      ok = false;
      failure_reason = "Server could not decode image source.";
    }
  } else {
    ok = false;
    failure_reason = "Unrecognized image content type.";
  }

  if (!ok) {
    writer->Write(failure_reason, message_handler);
    response_headers->set_status_code(HttpStatus::NOT_FOUND);
    response_headers->set_reason_phrase(failure_reason);
    message_handler->Error(url.as_string().c_str(), 0, failure_reason);
  }
  return ok;
}

}  // namespace net_instaweb
