// Copyright 2013 Google Inc. All Rights Reserved.
// Author: mpalem@google.com (Maya Palem)

#include "net/instaweb/rewriter/public/css_url_extractor.h"

#include "net/instaweb/rewriter/public/css_tag_scanner.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/null_writer.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

CssUrlExtractor::~CssUrlExtractor() {
}

void CssUrlExtractor::ExtractUrl(const StringPiece& in_text,
                                 StringVector* urls) {
  // We dont care about the output, we just want the url string captured.
  NullWriter out;
  NullMessageHandler handler;
  out_urls_ = urls;
  CssTagScanner::TransformUrls(in_text, &out, this, &handler);
}

CssTagScanner::Transformer::TransformStatus CssUrlExtractor::Transform(
    GoogleString* str) {
  if (!str->empty()) {
    // Push the Url into the output vector
    GoogleString* url = StringVectorAdd(out_urls_);
    *url = *str;
  }
  return kNoChange;
}

}  // namespace net_instaweb
