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

#include "net/instaweb/rewriter/public/css_tag_scanner.h"

#include <cstddef>

#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/rewriter/public/domain_rewrite_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/url_left_trim_filter.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/writer.h"
#include "webutil/css/tostring.h"

namespace {
const char kTextCss[] = "text/css";
}

namespace net_instaweb {

class HtmlParse;

CssTagScanner::Transformer::~Transformer() {
}

const char CssTagScanner::kStylesheet[] = "stylesheet";
const char CssTagScanner::kAlternate[] = "alternate";
const char CssTagScanner::kUriValue[] = "url(";

// Finds CSS files and calls another filter.
CssTagScanner::CssTagScanner(HtmlParse* html_parse) {
}

bool CssTagScanner::ParseCssElement(
    HtmlElement* element,
    HtmlElement::Attribute** href,
    const char** media,
    StringPieceVector* nonstandard_attributes) {
  *media = "";
  *href = NULL;
  if (element->keyword() != HtmlName::kLink) {
    return false;
  }
  // We must have all attributes rel='stylesheet' href='name.css'; and if
  // there is a type, it must be type='text/css'. These can be in any order.
  HtmlElement::AttributeList* attrs = element->mutable_attributes();
  bool has_href = false, has_rel_stylesheet = false;
  for (HtmlElement::AttributeIterator i(attrs->begin());
       i != attrs->end(); ++i) {
    HtmlElement::Attribute& attr = *i;
    switch (attr.keyword()) {
      case HtmlName::kHref:
        if (has_href || attr.decoding_error()) {
          // Duplicate or undecipherable href.
          return false;
        }
        *href = &attr;
        has_href = true;
        break;
      case HtmlName::kRel: {
        StringPiece rel(attr.DecodedValueOrNull());
        TrimWhitespace(&rel);
        if (!StringCaseEqual(rel, kStylesheet)) {
          // rel=something_else.  Abort.  Includes alternate stylesheets.
          return false;
        }
        has_rel_stylesheet = true;
        break;
      }
      case HtmlName::kMedia:
        *media = attr.DecodedValueOrNull();
        if (*media == NULL) {
          // No value (media rather than media=), or decoding error
          return false;
        }
        break;
      case HtmlName::kType: {
        // If we see this, it must be type=text/css.  This attribute is not
        // required.
        StringPiece type(attr.DecodedValueOrNull());
        TrimWhitespace(&type);
        if (!StringCaseEqual(type, kTextCss)) {
          return false;
        }
        break;
      }
      case HtmlName::kTitle:
      case HtmlName::kPagespeedNoTransform:
        // title= is here because it indicates a default stylesheet among
        // alternatives.  See:
        // http://www.w3.org/TR/REC-html40/present/styles.html#h-14.3.1
        // We don't alter a link for which pagespeed_no_transform is set.
        return false;
      default:
        // Other tags are assumed to be harmless noise; if that is not the case
        // for a particular filter, it should be detected within that filter
        // (examples: extra tags are rejected in css_combine_filter, but they're
        // preserved by css_inline_filter).
        if (nonstandard_attributes != NULL) {
          nonstandard_attributes->push_back(attr.name_str());
        }
        break;
    }
  }

  // we require both 'href=...' and 'rel=stylesheet'.
  return (has_rel_stylesheet && has_href);
}

namespace {

// Removes the first character from *in, and puts it into *c.
// Returns true if successful
inline bool PopFirst(StringPiece* in, char* c) {
  if (!in->empty()) {
    *c = (*in)[0];
    in->remove_prefix(1);
    return true;
  } else {
    return false;
  }
}

// If in starts with expected, returns true and consumes it.
inline bool EatLiteral(const StringPiece& expected, StringPiece* in) {
  if (in->starts_with(expected)) {
    in->remove_prefix(expected.size());
    return true;
  } else {
    return false;
  }
}

// Extract string- or identifier-like content from CSS until reaching the
// given terminator (which will not be included in the output), handling simple
// escapes along the way. in will be modified to skip over the bytes consumed,
// regardless of whether successful or not (to avoid backtracking).
// If is_string is true, will handle non-termination by truncating content
// at end of line (which is the CSS behavior for unclosed strings).
// Returns whether the content could be successfully extracted.
bool CssExtractUntil(bool is_string, char term,
                     StringPiece* in, GoogleString* out, bool* found_term) {
  bool found_error = false;
  *found_term = false;

  char c;
  out->clear();
  while (PopFirst(in, &c)) {
    if (c == term) {
      *found_term = true;
      break;
    } else if (c == '\\') {
      // See if it's an escape we recognize.
      // TODO(morlovich): handle hex escapes here as well. For now we just match
      // the non-whitespace stuff we ourselves produce.
      char escape_val;
      if (PopFirst(in, &escape_val)) {
        switch (escape_val) {
          case ',':
          case '\"':
          case '\'':
          case '\\':
          case '(':
          case ')':
            out->push_back(escape_val);
            break;
          case '\n':
            // \ before newline in strings simply disappears; for everything
            // else we fallthrough to below.
            if (is_string) {
              break;
            }
            FALLTHROUGH_INTENDED;
          default:
            // We can't parse it but it's not clear that ignoring it is the
            // safest thing, so we just pass it through unmodified
            out->push_back(c);
            out->push_back(escape_val);
        };
      } else {
        found_error = true;
      }
    } else {
      out->push_back(c);
    }
  }

  if (is_string && !*found_term) {
    // Unclosed strings have a special rule -- they're terminated at first
    // newline.
    size_t pos = out->find('\n');
    if (pos != GoogleString::npos) {
      size_t full_len = out->size();

      // Truncate stuff till before the new line
      out->resize(pos);

      // Rollback the position to point to the newline. While this does
      // mean we will be re-scanning, it can't be too bad since there can't be
      // another quote of this same type again.
      const char* begin = in->data() - (full_len - out->size());
      const char* end = in->data() + in->size();
      *in = StringPiece(begin, end - begin);
    }

    return !found_error;
  }

  return *found_term && !found_error;
}


// Tries to extract a string from current position into out.
// quote_out will contain its delimeter.
bool CssExtractString(StringPiece* in, GoogleString* out, char* quote_out,
                      bool* found_term) {
  if (EatLiteral("\'", in)) {
    if (CssExtractUntil(true, '\'', in, out, found_term)) {
      *quote_out = '\'';
      return true;
    }
  } else if (EatLiteral("\"", in)) {
    if (CssExtractUntil(true, '\"', in, out, found_term)) {
      *quote_out = '\"';
      return true;
    }
  }
  return false;
}

bool WriteRange(const char* out_begin, const char* out_end,
                Writer* writer, MessageHandler* handler) {
  if (out_end > out_begin) {
    return writer->Write(StringPiece(out_begin, out_end - out_begin), handler);
  } else {
    return true;
  }
}

}  // namespace

bool CssTagScanner::TransformUrls(
    const StringPiece& contents, Writer* writer, Transformer* transformer,
    MessageHandler* handler) {
  bool ok = true;

  // Keeps track of which portion of input we should write out in
  // the next output batch. This an iterator-style interval, i.e.
  // [out_begin, out_end)
  const char* out_begin = contents.data();
  const char* out_end = contents.data();

  char c;
  GoogleString url;
  StringPiece remaining = contents;
  while (PopFirst(&remaining, &c)) {
    enum { kNone, kImport, kUrl} have_url = kNone;
    bool is_quoted = false;
    bool have_term_quote = false;
    bool have_term_paren = false;
    char quote = '?';

    if (c == '@') {
      // See if we are at an @import. We provisionally set an
      // end point for batch write to exclude the @, so if we
      // write out with transformed URL, we should start with
      // @import.
      if (EatLiteral("import", &remaining)) {
        TrimLeadingWhitespace(&remaining);
        // The code here handles @import "foo" and @import 'foo';
        // for @import url(... we simply pass the @import through and let
        // the code that handles url( below take care of it.
        if (CssExtractString(&remaining, &url, &quote, &have_term_quote)) {
          have_url = kImport;
          is_quoted = true;
        }
      }
    } else if (c == 'u') {
      // See if we are at url(. Also provisionally set an
      // end point for batch write to exclude the u, so if we
      // write out with transformed URL, we should start with
      // url(
      GoogleString wrapped_url;
      if (EatLiteral("rl(", &remaining)) {
        TrimLeadingWhitespace(&remaining);
        // Note if we have a quoted URL inside url(), it needs to be
        // parsed as such.
        if (CssExtractString(&remaining, &url, &quote, &have_term_quote)) {
          TrimLeadingWhitespace(&remaining);
          if (EatLiteral(")", &remaining)) {
            have_url = kUrl;
            is_quoted = true;
            have_term_paren = true;
          }
        } else if (CssExtractUntil(false, ')', &remaining, &wrapped_url,
                                   &have_term_paren)) {
          TrimWhitespace(wrapped_url, &url);
          have_url = kUrl;
        }
      }
    }

    if (have_url != kNone) {
      // See if we actually have to do something. If the transformer
      // wants to leave the URL alone, we will just pass the bytes through.
      switch (transformer->Transform(&url)) {
        case Transformer::kSuccess: {
          // Write out the buffered up part of input.
          ok = ok && WriteRange(out_begin, out_end, writer, handler);

          if (have_url == kImport) {
            ok = ok && writer->Write("@import ", handler);
          } else {
            ok = ok && writer->Write("url(", handler);
          }

          if (is_quoted) {
            ok = ok && writer->Write(StringPiece(&quote, 1), handler);
          }
          ok = ok && writer->Write(Css::EscapeUrl(url), handler);
          if (have_term_quote) {
            ok = ok && writer->Write(StringPiece(&quote, 1), handler);
          }

          if (have_term_paren) {
            ok = ok && writer->Write(")", handler);
          }

          // Begin accumulating input again starting from next byte.
          out_begin = remaining.data();
          break;
        }
        case Transformer::kFailure: {
          // We could not transform URL, fail fast.
          handler->Message(kWarning,
                           "Transform failed for url %s", url.c_str());
          return false;
        }
        case Transformer::kNoChange: {
          break;
        }
      }
    }

    // remaining.data() points to the next byte to read, which is exactly
    // right after the last byte we want to output.
    out_end = remaining.data();
  }

  // Write out whatever got buffered at the end.
  ok = ok && WriteRange(out_begin, out_end, writer, handler);

  return ok;
}

bool CssTagScanner::HasImport(const StringPiece& contents,
                              MessageHandler* handler) {
  // Search for case insensitive @import.
  size_t pos = -1;  // So that pos + 1 == 0 below.
  const StringPiece kImport("import");
  while ((pos = contents.find("@", pos + 1)) != StringPiece::npos) {
    // Rest is everything past the @ (non-inclusive).
    StringPiece rest = contents.substr(pos + 1);
    if (StringCaseStartsWith(rest, kImport)) {
      return true;
    }
  }
  return false;
}

bool CssTagScanner::HasUrl(const StringPiece& contents) {
  return (contents.find(CssTagScanner::kUriValue) != StringPiece::npos);
}

bool CssTagScanner::IsStylesheetOrAlternate(
    const StringPiece& attribute_value) {
  StringPieceVector values;
  SplitStringPieceToVector(attribute_value, " ", &values, true);
  for (int i = 0, n = values.size(); i < n; ++i) {
    if (StringCaseEqual(values[i], kStylesheet)) {
      return true;
    }
  }
  return false;
}

bool CssTagScanner::IsAlternateStylesheet(const StringPiece& attribute_value) {
  bool has_stylesheet = false;
  bool has_alternate = false;
  StringPieceVector values;
  SplitStringPieceToVector(attribute_value, " ", &values, true);
  for (int i = 0, n = values.size(); i < n; ++i) {
    if (StringCaseEqual(values[i], kStylesheet)) {
      has_stylesheet = true;
    } else if (StringCaseEqual(values[i], kAlternate)) {
      has_alternate = true;
    }
  }

  return has_stylesheet && has_alternate;
}

RewriteDomainTransformer::RewriteDomainTransformer(
    const GoogleUrl* old_base_url, const GoogleUrl* new_base_url,
    RewriteDriver* driver)
    : old_base_url_(old_base_url), new_base_url_(new_base_url),
      domain_rewriter_(driver->domain_rewriter()),
      url_trim_filter_(driver->url_trim_filter()),
      handler_(driver->message_handler()),
      trim_urls_(true),
      driver_(driver) {
}

RewriteDomainTransformer::~RewriteDomainTransformer() {
}

CssTagScanner::Transformer::TransformStatus RewriteDomainTransformer::Transform(
    GoogleString* str) {
  GoogleString rewritten;  // Result of rewriting domain.
  GoogleString out;        // Result after trimming.
  if (domain_rewriter_->Rewrite(*str, *old_base_url_, driver_,
                                true /* apply_sharding */,
                                &rewritten)
      == DomainRewriteFilter::kFail) {
    return kFailure;
  }
  // Note: Even if Rewrite() returned kDomainUnchanged, it will still absolutify
  // the URL into rewritten. We may return kSuccess if that URL does not get
  // re-trimmed to the original string.

  // Note: Because of complications with sharding, we cannot trim
  // sharded resources against the final sharded domain of the CSS file.
  // Specifically, that final domain depends upon the precise text of that
  // we are altering here.
  if (!trim_urls_ ||
      !url_trim_filter_->Trim(*new_base_url_, rewritten, &out, handler_)) {
    // If we couldn't trim rewritten -> out, just copy it (swap is optimization)
    out.swap(rewritten);
  }

  if (out == *str) {
    return kNoChange;
  } else {
    str->swap(out);
    return kSuccess;
  }
}

}  // namespace net_instaweb
