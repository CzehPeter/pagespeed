/*
 * Copyright 2011 Google Inc.
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

// Author: nforman@google.com (Naomi Forman)

#include "net/instaweb/rewriter/public/css_util.h"

#include <vector>

#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "util/utf8/public/unicodetext.h"
#include "webutil/css/media.h"
#include "webutil/css/parser.h"
#include "webutil/css/property.h"
#include "webutil/css/value.h"

namespace net_instaweb {

namespace css_util {

// Extract the numerical value from a values vector.
// TODO(nforman): Allow specification what what style of numbers we can handle.
int GetValueDimension(const Css::Values* values) {
  for (Css::Values::const_iterator value_iter = values->begin();
       value_iter != values->end(); ++value_iter) {
    Css::Value* value = *value_iter;
    if ((value->GetLexicalUnitType() == Css::Value::NUMBER)
        && (value->GetDimension() == Css::Value::PX)) {
      return value->GetIntegerValue();
    }
  }
  return kNoValue;
}

DimensionState GetDimensions(Css::Declarations* decls,
                             int* width, int* height) {
  bool has_width = false;
  bool has_height = false;
  *width = kNoValue;
  *height = kNoValue;
  for (Css::Declarations::iterator decl_iter = decls->begin();
       decl_iter != decls->end() && (!has_width || !has_height); ++decl_iter) {
    Css::Declaration* decl = *decl_iter;
    switch (decl->prop()) {
      case Css::Property::WIDTH: {
        *width = GetValueDimension(decl->values());
        has_width = true;
        break;
      }
      case Css::Property::HEIGHT: {
        *height = GetValueDimension(decl->values());
        has_height = true;
        break;
      }
      default:
        break;
    }
  }
  if (has_width && has_height && *width != kNoValue && *height != kNoValue) {
    return kHasBothDimensions;
  } else if ((has_width && *width == kNoValue) ||
             (has_height && *height == kNoValue)) {
    return kNotParsable;
  } else if (has_width) {
    return kHasWidthOnly;
  } else if (has_height) {
    return kHasHeightOnly;
  }
  return kNoDimensions;
}

StyleExtractor::StyleExtractor(HtmlElement* element)
    : decls_(GetDeclsFromElement(element)),
      width_px_(kNoValue),
      height_px_(kNoValue) {
  if (decls_.get() != NULL) {
    state_ = GetDimensions(decls_.get(), &width_px_, &height_px_);
  } else {
    state_ = kNoDimensions;
  }
}

StyleExtractor::~StyleExtractor() {}

// Return a Declarations* from the style attribute of an element.  If
// there is no style, return NULL.
Css::Declarations* StyleExtractor::GetDeclsFromElement(HtmlElement* element) {
  HtmlElement::Attribute* style = element->FindAttribute(HtmlName::kStyle);
  if ((style != NULL) && (style->DecodedValueOrNull() != NULL)) {
    Css::Parser parser(style->DecodedValueOrNull());
    return parser.ParseDeclarations();
  }
  return NULL;
}

void VectorizeMediaAttribute(const StringPiece& input_media,
                             StringVector* output_vector) {
  // Split on commas, trim whitespace from each element found, delete empties.
  // Note that we hand trim because SplitStringPieceToVector() trims elements
  // of zero length but not those comprising one or more whitespace chars.
  StringPieceVector media_vector;
  SplitStringPieceToVector(input_media, ",", &media_vector, false);
  std::vector<StringPiece>::iterator it;
  for (it = media_vector.begin(); it != media_vector.end(); ++it) {
    TrimWhitespace(&(*it));
    if (StringCaseEqual(*it, kAllMedia)) {
      // Special case: an element of value 'all'.
      output_vector->clear();
      break;
    } else if (!it->empty()) {
      it->CopyToString(StringVectorAdd(output_vector));
    }
  }

  return;
}

GoogleString StringifyMediaVector(const StringVector& input_media) {
  // Special case: inverse of the special rule in the vectorize function.
  if (input_media.empty()) {
    return kAllMedia;
  }
  // Hmm, we don't seem to have a useful 'join' function handy.
  GoogleString result(input_media[0]);
  for (int i = 1, n = input_media.size(); i < n; ++i) {
    StrAppend(&result, ",", input_media[i]);
  }
  return result;
}

bool IsComplexMediaQuery(const Css::MediaQuery& query) {
  return (query.qualifier() != Css::MediaQuery::NO_QUALIFIER ||
          !query.expressions().empty());
}

bool ConvertMediaQueriesToStringVector(const Css::MediaQueries& in_vector,
                                       StringVector* out_vector) {
  out_vector->clear();
  Css::MediaQueries::const_iterator iter;
  for (iter = in_vector.begin(); iter != in_vector.end(); ++iter) {
    // Reject complex media queries immediately.
    if (IsComplexMediaQuery(**iter)) {
      out_vector->clear();
      return false;
    } else {
      const UnicodeText& media_type = (*iter)->media_type();
      StringPiece element(media_type.utf8_data(), media_type.utf8_length());
      TrimWhitespace(&element);
      if (!element.empty()) {
        element.CopyToString(StringVectorAdd(out_vector));
      }
    }
  }
  return true;
}

void ConvertStringVectorToMediaQueries(const StringVector& in_vector,
                                       Css::MediaQueries* out_vector) {
  out_vector->Clear();
  std::vector<GoogleString>::const_iterator iter;
  for (iter = in_vector.begin(); iter != in_vector.end(); ++iter) {
    StringPiece element(*iter);
    TrimWhitespace(&element);
    if (!element.empty()) {
      Css::MediaQuery* query = new Css::MediaQuery;
      query->set_media_type(UTF8ToUnicodeText(element.data(), element.size()));
      out_vector->push_back(query);
    }
  }
}

void ClearVectorIfContainsMediaAll(StringVector* media) {
  StringVector::const_iterator iter;
  for (iter = media->begin(); iter != media->end(); ++iter) {
    if (StringCaseEqual(*iter, kAllMedia)) {
      media->clear();
      break;
    }
  }
}

}  // namespace css_util

}  // namespace net_instaweb
