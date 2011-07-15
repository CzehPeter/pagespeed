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
// Author: jmaessen@google.com (Jan Maessen)

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_IMAGE_URL_ENCODER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_IMAGE_URL_ENCODER_H_

#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/url_segment_encoder.h"

namespace net_instaweb {
class MessageHandler;

// This class implements the encoding of image urls with optional additional
// dimension metadata.  It basically prepends characters indicating image
// dimensions on the page and webp eligibility (this information is conveyed in
// the ResourceContext).
//   http://...path.../50x75xurl...  No webp, image is 50x75 on page
//   http://...path.../50x75wurl...  Webp requested, image is 50x75 on page
//   http://...path.../xurl...  Page does not specify both dimensions.  No webp.
//   http://...path.../wurl...  Webp requested, page missing dimensions.
class ImageUrlEncoder : public UrlSegmentEncoder {
 public:
  ImageUrlEncoder() {}
  virtual ~ImageUrlEncoder();

  virtual void Encode(const StringVector& urls,
                      const ResourceContext* dim,
                      GoogleString* rewritten_url) const;

  virtual bool Decode(const StringPiece& url_segment,
                      StringVector* urls,
                      ResourceContext* dim,
                      MessageHandler* handler) const;

  static bool HasDimensions(const ResourceContext& data) {
    return (data.has_image_tag_dims() &&
            HasValidDimensions(data.image_tag_dims()));
  }

  static bool HasValidDimensions(const ImageDim& dims) {
    return (dims.has_width() && dims.has_height());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ImageUrlEncoder);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_IMAGE_URL_ENCODER_H_
