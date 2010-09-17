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

// Copyright 2006 Google Inc. All Rights Reserved.
// Author: dpeng@google.com (Daniel Peng)

#include "webutil/css/parser.h"

#include <ctype.h>  // isascii

#include <string>
#include <vector>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "strings/memutil.h"
#include "strings/strutil.h"
#include "third_party/utf/utf.h"
#include "util/gtl/stl_util-inl.h"
#include "webutil/css/string.h"
#include "webutil/css/string_util.h"
#include "webutil/css/util.h"
#include "webutil/css/value.h"
#include "webutil/css/valuevalidator.h"



namespace Css {

// Using isascii with signed chars is unfortunately undefined.
static inline bool IsAscii(char c) {
  return isascii(static_cast<unsigned char>(c));
}


class Tracer {  // in opt mode, do nothing.
 public:
  Tracer(const char* name, const char** in) { }
};



// ****************
// Helper functions
// ****************

// is c a space?  Only the characters "space" (Unicode code 32), "tab"
// (9), "line feed" (10), "carriage return" (13), and "form feed" (12)
// can occur in whitespace. Other space-like characters, such as
// "em-space" (8195) and "ideographic space" (12288), are never part
// of whitespace.
// http://www.w3.org/TR/REC-CSS2/syndata.html#whitespace
static bool IsSpace(char c) {
  switch (c) {
    case ' ': case '\t': case '\r': case '\n': case '\f':
      return true;
    default:
      return false;
  }
}

// If the character c is a hex digit, DeHex returns the number it
// represents ('0' => 0, 'A' => 10, 'F' => 15).  Otherwise, DeHex
// returns -1.
static int DeHex(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  } else if (c >= 'A' && c <= 'F') {
    return (c - 'A') + 10;
  } else if (c >= 'a' && c <= 'f') {
    return (c - 'a') + 10;
  } else {
    return -1;
  }
}

// ****************
// Recursive-descent functions.
//
// The best documentation for these is in cssparser.h.
//
// ****************

// consume whitespace and comments.
void Parser::SkipSpace() {
  Tracer trace(__func__, &in_);
  while (in_ < end_) {
    if (IsSpace(*in_))
      in_++;
    else if (in_ + 1 < end_ && in_[0] == '/' && in_[1] == '*')
      SkipComment();
    else
      return;
  }
}

// consume comment /* aoeuaoe */
void Parser::SkipComment() {
  DCHECK(in_ + 2 <= end_ && in_[0] == '/' && in_[1] == '*');
  in_ += 2;  // skip the /*
  while (in_ + 1 < end_) {
    if (in_[0] == '*' && in_[1] == '/') {
      in_ += 2;
      return;
    } else {
      in_++;
    }
  }
  in_ = end_;
}

// skips until delim is seen or end-of-stream. returns if delim is actually
// seen.
bool Parser::SkipPastDelimiter(char delim) {
  SkipSpace();
  while (in_ < end_ && *in_ != delim) {
    ++in_;
    SkipSpace();
  }

  if (Done()) return false;
  ++in_;
  return true;
}

// returns true if there might be a token to read
bool Parser::SkipToNextToken() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  while (in_ < end_) {
    switch (*in_) {
      case '{':
        ParseBlock();  // ignore
        break;
      case '@':
        in_++;
        ParseIdent();  // ignore
        break;
      case ';': case '}':
      case '!':
        return false;
      default:
        return true;
    }
    SkipSpace();
  }
  return false;
}

// In CSS2, identifiers (including element names, classes, and IDs in
// selectors) can contain only the characters [A-Za-z0-9] and ISO
// 10646 characters 161 and higher, plus the hyphen (-); they cannot
// start with a hyphen or a digit. They can also contain escaped
// characters and any ISO 10646 character as a numeric code (see next
// item). For instance, the identifier "B&W?" may be written as
// "B\&W\?" or "B\26 W\3F".
//
// We're a little more forgiving than the standard and permit hyphens
// and digits to start identifiers.
//
// FIXME(yian): actually, IE is more forgiving than Firefox in using a class
// selector starting with digits.
//
// http://www.w3.org/TR/REC-CSS2/syndata.html#value-def-identifier
static bool StartsIdent(char c) {
  return ((c >= 'A' && c <= 'Z')
          || (c >= 'a' && c <= 'z')
          || (c >= '0' && c <= '9')
          || c == '-' || c == '_'
          || !IsAscii(c));
}

UnicodeText Parser::ParseIdent() {
  Tracer trace(__func__, &in_);
  UnicodeText s;
  while (in_ < end_) {
    if ((*in_ >= 'A' && *in_ <= 'Z')
        || (*in_ >= 'a' && *in_ <= 'z')
        || (*in_ >= '0' && *in_ <= '9')
        || *in_ == '-' || *in_ == '_') {
      s.push_back(*in_);
      in_++;
    } else if (!IsAscii(*in_)) {
      Rune rune;
      int len = charntorune(&rune, in_, end_-in_);
      if (len && rune != Runeerror) {
        if (rune >= 161) {
          s.push_back(rune);
          in_ += len;
        } else {  // characters 128-160 can't be in identifiers.
          return s;
        }
      } else {  // Encoding error.  Be a little forgiving.
        in_++;
      }
    } else if (*in_ == '\\') {
      s.push_back(ParseEscape());
    } else {
      return s;
    }
  }
  return s;
}

// Returns the codepoint for the current escape.
// \abcdef => codepoint 0xabcdef.  also consumes whitespace afterwards.
// \(UTF8-encoded unicode character) => codepoint for that character
char32 Parser::ParseEscape() {
  SkipSpace();
  DCHECK_LT(in_, end_);
  DCHECK_EQ(*in_, '\\');
  in_++;
  if (Done()) return static_cast<char32>('\\');

  int dehexed = DeHex(*in_);
  if (dehexed == -1) {
    Rune rune;
    int len = charntorune(&rune, in_, end_-in_);
    if (len && rune != Runeerror) {
      in_ += len;
    } else {
      in_++;
    }
    return rune;
  } else {
    char32 codepoint = 0;
    for (int count = 0; count < 6 && in_ < end_; count++) {
      dehexed = DeHex(*in_);
      if (dehexed == -1)
        break;
      in_++;
      codepoint = codepoint << 4 | dehexed;
    }
    if (end_ - in_ >= 2 && memcmp(in_, "\r\n", 2) == 0)
      in_ += 2;
    else if (IsSpace(*in_))
      in_++;
    return codepoint;
  }
}

// Starts at delim.
template<char delim>
UnicodeText Parser::ParseString() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  DCHECK_LT(in_, end_);
  DCHECK_EQ(*in_, delim);
  in_++;
  if (Done()) return UnicodeText();

  UnicodeText s;
  while (in_ < end_) {
    switch (*in_) {
      case delim:
        in_++;
        return s;
      case '\n':
        return s;
      case '\\':
        if (in_ + 1 < end_ && in_[1] == '\n') {
          in_ += 2;
        } else {
          s.push_back(ParseEscape());
        }
        break;
      default:
        if (!IsAscii(*in_)) {
          Rune rune;
          int len = charntorune(&rune, in_, end_-in_);
          if (len && rune != Runeerror) {
            s.push_back(rune);
            in_ += len;
          } else {
            in_++;
          }
        } else {
          s.push_back(*in_);
          in_++;
        }
        break;
    }
  }
  return s;
}

// parse ident or 'string'
UnicodeText Parser::ParseStringOrIdent() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return UnicodeText();
  DCHECK_LT(in_, end_);

  if (*in_ == '\'') {
    return ParseString<'\''>();
  } else if (*in_ == '"') {
    return ParseString<'"'>();
  } else {
    return ParseIdent();
  }
}

// Parse a CSS number, including unit or percent sign.
Value* Parser::ParseNumber() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  const char* begin = in_;
  if (in_ < end_ && (*in_ == '-' || *in_ == '+'))  // sign
    in_++;
  while (in_ < end_ && isdigit(*in_)) {
    in_++;
  }
  if (*in_ == '.') {
    in_++;

    while (in_ < end_ && isdigit(*in_)) {
      in_++;
    }
  }
  double num = 0;
  if (in_ == begin || !ParseDouble(begin, in_ - begin, &num)) {
    return NULL;
  }
  if (*in_ == '%') {
    in_++;
    return new Value(num, Value::PERCENT);
  } else if (StartsIdent(*in_)) {
    return new Value(num, ParseIdent());
  } else {
    return new Value(num, Value::NO_UNIT);
  }
}

HtmlColor Parser::ParseColor() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return HtmlColor("", 0);
  DCHECK_LT(in_, end_);

  unsigned char hexdigits[6] = {0};
  int dehexed;
  int i = 0;

  const char* oldin = in_;

  // To further mess things up, IE also accepts string values happily.
  if (*in_ == '"' || *in_ == '\'') {
    in_++;
    if (Done()) return HtmlColor("", 0);
  }

  bool rgb_valid = quirks_mode_ || *in_ == '#';

  if (*in_ == '#') in_++;

  while (in_ < end_ && i < 6 && (dehexed = DeHex(*in_)) != -1) {
    hexdigits[i] = static_cast<unsigned char>(dehexed);
    i++;
    in_++;
  }

  // close strings. Assume a named color if there are trailing characters
  if (*oldin == '"' || *oldin == '\'') {
    if (Done() || *in_ != *oldin)  // no need to touch in_, will redo anyway.
      i = 0;
    else
      in_++;
  }

  // Normally, ParseXXX() routines stop wherever it cannot be consumed and
  // doesn't check whether the next character is valid. which should be caught
  // by the next ParseXXX() routine. But ParseColor may be called to test
  // whether a numerical value can be used as color, and fail over to a normal
  // ParseAny(). We need to do an immediate check here to guarantine a valid
  // non-color number (such as 100%) will not be accepted as a color.
  //
  // We also do not want rrggbb (without #) to be accepted in non-quirks mode,
  // but HtmlColor will happily accept it anyway. Do a sanity check here.
  if (i == 3 || i == 6) {
    if (!rgb_valid ||
        (!Done() && (*in_ == '%' || StartsIdent(*in_))))
      return HtmlColor("", 0);
  }

  if (i == 3) {
    return HtmlColor(hexdigits[0] | hexdigits[0] << 4,
                     hexdigits[1] | hexdigits[1] << 4,
                     hexdigits[2] | hexdigits[2] << 4);
  } else if (i == 6) {
    return HtmlColor(hexdigits[1] | hexdigits[0] << 4,
                     hexdigits[3] | hexdigits[2] << 4,
                     hexdigits[5] | hexdigits[4] << 4);
  } else {
    in_ = oldin;

    // A named color must not begin with #, but we need to parse it anyway and
    // report failure later.
    bool name_valid = true;
    if (*in_ == '#') {
      in_++;
      name_valid = false;
    }

    string ident = UnicodeTextToUTF8(ParseStringOrIdent());
    HtmlColor val("", 0);
    if (name_valid) {
      val.SetValueFromName(ident.c_str());
      if (!val.IsDefined())
        Util::GetSystemColor(ident, &val);
    }
    return val;
  }
}

// Returns the 0-255 RGB value corresponding to Value v.  Only
// unusual thing is percentages are interpreted as percentages of
// 255.0.
unsigned char Parser::ValueToRGB(Value* v) {
  int toret = 0;
  if (v == NULL) {
    toret = 0;
  } else if (v->GetLexicalUnitType() == Value::NUMBER) {
    if (v->GetDimension() == Value::PERCENT) {
      toret = static_cast<int>(v->GetFloatValue()/100.0 * 255.0);
    } else {
      toret = v->GetIntegerValue();
    }
  } else {
    toret = 0;
  }

  // RGB values outside the device gamut should be clipped according to spec.
  if (toret > 255)
    toret = 255;
  if (toret < 0)
    toret = 0;
  return static_cast<unsigned char>(toret);
}

// parse RGB color 25, 32, 12 or 25%, 1%, 7%.
// stops without consuming final right-paren
Value* Parser::ParseRgbColor() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  unsigned char rgb[3];

  for (int i = 0; i < 3; i++) {
    scoped_ptr<Value> val(ParseNumber());
    if (!val.get() || val->GetLexicalUnitType() != Value::NUMBER ||
        (val->GetDimension() != Value::PERCENT &&
         val->GetDimension() != Value::NO_UNIT))
      break;
    rgb[i] = ValueToRGB(val.get());
    SkipSpace();
    // Make sure the correct syntax is followed.
    if (Done() || (*in_ != ',' && *in_ != ')') || (*in_ == ')' && i != 2))
      break;

    if (*in_ == ')')
      return new Value(HtmlColor(rgb[0], rgb[1], rgb[2]));
    in_++;  // ','
  }

  return NULL;
}

// parse url yellow.png or 'yellow.png'
// (doesn't consume subsequent right-paren).
Value* Parser::ParseUrl() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  UnicodeText s;
  if (*in_ == '\'') {
    s = ParseString<'\''>();
  } else if (*in_ == '"') {
    s = ParseString<'"'>();
  } else {
    while (in_ < end_) {
      if (IsSpace(*in_) || *in_ == ')') {
        break;
      } else if (*in_ == '\\') {
        s.push_back(ParseEscape());
      } else if (!IsAscii(*in_)) {
        Rune rune;
        int len = charntorune(&rune, in_, end_-in_);
        if (len && rune != Runeerror) {
          s.push_back(rune);
          in_ += len;
        } else {
          in_++;
        }
      } else {
        s.push_back(*in_);
        in_++;
      }
    }
  }
  SkipSpace();
  if (!Done() && *in_ == ')')
    return new Value(Value::URI, s);

  return NULL;
}

// parse rect(top, right, bottom, left) without consuming final right-paren.
// Spaces are allowed as delimiters here for historical reasons.
Value* Parser::ParseRect() {
  scoped_ptr<Values> params(new Values);

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  // Never parse after the final right-paren!
  if (*in_ == ')') return NULL;

  for (int i = 0; i < 4; i++) {
    scoped_ptr<Value> val(ParseAny());
    if (!val.get())
      break;
    params->push_back(val.release());
    SkipSpace();
    // Make sure the correct syntax is followed.
    if (Done() || (*in_ == ')' && i != 3))
      break;

    if (*in_ == ')')
      return new Value(Value::RECT, params.release());
    else if (*in_ == ',')
      in_++;
  }

  return NULL;
}

Value* Parser::ParseAnyExpectingColor() {
  Tracer trace(__func__, &in_);
  Value* toret = NULL;

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  const char* oldin = in_;
  HtmlColor c = ParseColor();
  if (c.IsDefined()) {
    toret = new Value(c);
  } else {
    in_ = oldin;  // no valid color.  rollback.
    toret = ParseAny();
  }
  return toret;
}

// Parses a CSS value.  Could be just about anything.
Value* Parser::ParseAny() {
  Tracer trace(__func__, &in_);
  Value* toret = NULL;

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  const char* oldin = in_;
  switch (*in_) {
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case '.':
      toret = ParseNumber();
      break;
    case '(': case '[': {
      char delim = *in_ == '(' ? ')' : ']';
      SkipPastDelimiter(delim);
      toret = NULL;  // we don't understand this construct.
      break;
    }
    case '"':
      toret = new Value(Value::STRING, ParseString<'"'>());
      break;
    case '\'':
      toret = new Value(Value::STRING, ParseString<'\''>());
      break;
    case '#': {
      HtmlColor color = ParseColor();
      if (color.IsDefined())
        toret = new Value(color);
      else
        toret = NULL;
      break;
    }
    case '+':
      toret = ParseNumber();
      break;
    case '-':
      // ambiguity between a negative number and an identifier starting with -.
      if (in_ < end_ - 1 &&
          ((*(in_ + 1) >= '0' && *(in_ + 1) <= '9') || *(in_ + 1) == '.')) {
        toret = ParseNumber();
        break;
      }
      // fail through
    default: {
      UnicodeText id = ParseIdent();
      if (id.empty()) {
        toret = NULL;
      } else if (*in_ == '(') {
        in_++;
        if (id.utf8_length() == 3
            && memcasecmp("url", id.utf8_data(), 3) == 0) {
          toret = ParseUrl();
        } else if (id.utf8_length() == 7
                   && memcasecmp("counter", id.utf8_data(), 7) == 0) {
          // TODO(yian): parse COUNTER parameters
          toret = new Value(Value::COUNTER, new Values());
        } else if (id.utf8_length() == 8
                   && memcasecmp("counters", id.utf8_data(), 8) == 0) {
          // TODO(yian): parse COUNTERS parameters
          toret = new Value(Value::COUNTER, new Values());
        } else if (id.utf8_length() == 3
                   && memcasecmp("rgb", id.utf8_data(), 3) == 0) {
          toret = ParseRgbColor();
        } else if (id.utf8_length() == 4
                   && memcasecmp("rect", id.utf8_data(), 4) == 0) {
          toret = ParseRect();
        } else {
          // TODO(yian): parse FUNCTION parameters
          toret = new Value(id, new Values());
        }
        SkipPastDelimiter(')');
      } else {
        toret = new Value(Identifier(id));
      }
      break;
    }
  }
  // Deadlock prevention: always make progress even if nothing can be parsed.
  if (toret == NULL && in_ == oldin) ++in_;
  return toret;
}

static bool IsPropExpectingColor(Property::Prop prop) {
  switch (prop) {
    case Property::BORDER_COLOR:
    case Property::BORDER_TOP_COLOR:
    case Property::BORDER_RIGHT_COLOR:
    case Property::BORDER_BOTTOM_COLOR:
    case Property::BORDER_LEFT_COLOR:
    case Property::BORDER:
    case Property::BORDER_TOP:
    case Property::BORDER_RIGHT:
    case Property::BORDER_BOTTOM:
    case Property::BORDER_LEFT:
    case Property::BACKGROUND_COLOR:
    case Property::BACKGROUND:
    case Property::COLOR:
    case Property::OUTLINE_COLOR:
    case Property::OUTLINE:
      return true;
    default:
      return false;
  }
}

// Parse values like "12pt Arial"
// If you make any change to this function, please also update
// ParseBackground, ParseFont and ParseFontFamily accordingly.
Values* Parser::ParseValues(Property::Prop prop) {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return new Values();
  DCHECK_LT(in_, end_);

  // If expecting_color is true, color values are expected.
  bool expecting_color = IsPropExpectingColor(prop);

  scoped_ptr<Values> values(new Values);
  while (SkipToNextToken()) {
    scoped_ptr<Value> v(expecting_color ?
                        ParseAnyExpectingColor() :
                        ParseAny());
    if (v.get() && ValueValidator::Get()->IsValidValue(prop, *v, quirks_mode_))
      values->push_back(v.release());
    else
      return NULL;
  }
  return values.release();
}

// Parse background. It is a shortcut property for individual background
// properties.
//
// The output is a tuple in the following order:
//   "background-color background-image background-repeat background-attachment
//   background-position-x background-position-y"
// or NULL if invalid
//
// The x-y position parsing is somewhat complicated. The following spec is from
// CSS 2.1.
// http://www.w3.org/TR/CSS21/colors.html#propdef-background-position
//
// "If a background image has been specified, this property specifies its
// initial position. If only one value is specified, the second value is
// assumed to be 'center'. If at least one value is not a keyword, then the
// first value represents the horizontal position and the second represents the
// vertical position. Negative <percentage> and <length> values are allowed.
// <percentage> ...
// <length> ...
// top ...
// right ...
// bottom ...
// left ...
// center ..."
//
// In addition, we have some IE specific behavior:
// 1) you can specifiy more than two values, but once both x and y have
//    specified values, further values will be discarded.
// 2) if y is not specified and x has seen two or more values, the last value
//    counts. The same for y.
// 3) [length, left/right] is valid and the length becomes a value for y.
//    [top/bottom, length] is also valid and the length becomes a value for x.
// If you make any change to this function, please also update ParseValues,
// ParseFont and ParseFontFamily if applicable.
bool Parser::ExpandBackground(const Declaration& original_declaration,
                              Declarations* new_declarations) {
  const Values* vals = original_declaration.values();
  bool important = original_declaration.IsImportant();
  DCHECK(vals != NULL);

  Value background_color(Identifier::TRANSPARENT);
  Value background_image(Identifier::NONE);
  Value background_repeat(Identifier::REPEAT);
  Value background_attachment(Identifier::SCROLL);
  scoped_ptr<Value> background_position_x;
  scoped_ptr<Value> background_position_y;

  bool is_first = true;

  // The following flag is used to implement IE quirks #3. When the first
  // positional value is a length or CENTER, it is stored in
  // background-position-x, but the value may actually be used as
  // background-position-y if a keyword LEFT or RIGHT appears later.
  bool first_is_ambiguous = false;  // Value::NUMBER or Identifier::CENTER

  for (Values::const_iterator iter = vals->begin(); iter != vals->end();
       ++iter) {
    const Value* val = *iter;

    // Firefox allows only one value to be set per property, IE need not.
    switch (val->GetLexicalUnitType()) {
      case Value::COLOR:
        // background_color, etc. take ownership of val. We will clear vals
        // at the end to make sure we don't have double ownership.
        background_color = *val;
        break;
      case Value::URI:
        background_image = *val;
        break;
      case Value::NUMBER:
        if (!background_position_x.get()) {
          background_position_x.reset(new Value(*val));
          first_is_ambiguous = true;
        } else if (!background_position_y.get()) {
          background_position_y.reset(new Value(*val));
        }
        break;
      case Value::IDENT:
        switch (val->GetIdentifier().ident()) {
          case Identifier::CENTER:
            if (!background_position_x.get()) {
              background_position_x.reset(new Value(*val));
              first_is_ambiguous = true;
            } else if (!background_position_y.get()) {
              background_position_y.reset(new Value(*val));
            }
            break;
          case Identifier::LEFT:
          case Identifier::RIGHT:
            // This is IE-specific behavior.
            if (!background_position_x.get() || !background_position_y.get()) {
              if (background_position_x.get() && first_is_ambiguous)
                background_position_y.reset(background_position_x.release());
              background_position_x.reset(new Value(*val));
              first_is_ambiguous = false;
            }
            break;
          case Identifier::TOP:
          case Identifier::BOTTOM:
            if (!background_position_x.get() || !background_position_y.get())
              background_position_y.reset(new Value(*val));
            break;
          case Identifier::REPEAT:
          case Identifier::REPEAT_X:
          case Identifier::REPEAT_Y:
          case Identifier::NO_REPEAT:
            background_repeat = *val;
            break;
          case Identifier::SCROLL:
          case Identifier::FIXED:
            background_attachment = *val;
            break;
          case Identifier::TRANSPARENT:
            background_color = *val;
            break;
          case Identifier::NONE:
            background_image = *val;
            break;
          case Identifier::INHERIT:
            // Inherit must be the one and only value.
            if (!(iter == vals->begin() && vals->size() == 1))
              return false;
            // We copy the inherit value into each background_* value.
            background_color = *val;
            background_image = *val;
            background_repeat = *val;
            background_attachment = *val;
            background_position_x.reset(new Value(*val));
            background_position_y.reset(new Value(*val));
            break;
          default:
            return false;
        }
        break;
      default:
        return false;
    }
    is_first = false;
  }
  if (is_first) return false;

  new_declarations->push_back(new Declaration(Property::BACKGROUND_COLOR,
                                              background_color,
                                              important));
  new_declarations->push_back(new Declaration(Property::BACKGROUND_IMAGE,
                                              background_image,
                                              important));
  new_declarations->push_back(new Declaration(Property::BACKGROUND_REPEAT,
                                              background_repeat,
                                              important));
  new_declarations->push_back(new Declaration(Property::BACKGROUND_ATTACHMENT,
                                              background_attachment,
                                              important));

  // Fix up x and y position.
  if (!background_position_x.get() && !background_position_y.get()) {
    background_position_x.reset(new Value(0, Value::PERCENT));
    background_position_y.reset(new Value(0, Value::PERCENT));
  } else if (!background_position_x.get()) {
    background_position_x.reset(new Value(50, Value::PERCENT));
  } else if (!background_position_y.get()) {
    background_position_y.reset(new Value(50, Value::PERCENT));
  }
  new_declarations->push_back(new Declaration(Property::BACKGROUND_POSITION_X,
                                              *background_position_x,
                                              important));
  new_declarations->push_back(new Declaration(Property::BACKGROUND_POSITION_Y,
                                              *background_position_y,
                                              important));

  return true;
}

// Parses font-family. It is special in that it uses commas as delimiters. It
// also concatenates adjacent idents into one name. Strings can be also used
// and they are separate from others even without commas.
// E.g, Courier New, Sans -> "Courier New", "Sans"
//      Arial "MS Times" monospace -> "Arial", "MS Times", "monospace".
// If you make any change to this function, please also update ParseValues,
// ParseBackground and ParseFont if applicable.
bool Parser::ParseFontFamily(Values* values) {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return true;
  DCHECK_LT(in_, end_);

  UnicodeText family;
  while (SkipToNextToken()) {
    if (*in_ == ',') {
      if (!family.empty()) {
        values->push_back(new Value(Identifier(family)));
        family.clear();
      }
      in_++;
    } else {
      scoped_ptr<Value> v(ParseAny());
      if (!v.get()) return false;
      switch (v->GetLexicalUnitType()) {
        case Value::STRING:
          if (!family.empty()) {
            values->push_back(new Value(Identifier(family)));
            family.clear();
          }
          values->push_back(v.release());
          break;
        case Value::IDENT:
          if (!family.empty())
            family.push_back(static_cast<char32>(' '));
          family.append(v->GetIdentifierText());
          break;
        default:
          return false;
      }
    }
  }
  if (!family.empty())
    values->push_back(new Value(Identifier(family)));
  return true;
}

// Parse font. It is special in that it uses a special format (see spec):
//  [ [ <'font-style'> || <'font-variant'> || <'font-weight'> ]?
//     <'font-size'> [ / <'line-height'> ]? <'font-family'> ]
//  | caption | icon | menu | message-box | small-caption | status-bar | inherit
//
// The output is a tuple in the following order:
//   "font-style font-variant font-weight font-size line-height font-family*"
// or NULL if invalid
// IE pecularity: font-family is optional (hence the *).
// If you make any change to this function, please also update ParseValues,
// ParseBackground and ParseFontFamily if applicable.
Values* Parser::ParseFont() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  scoped_ptr<Values> values(new Values);
  scoped_ptr<Value> font_style(new Value(Identifier(Identifier::NORMAL)));
  scoped_ptr<Value> font_variant(new Value(Identifier(Identifier::NORMAL)));
  scoped_ptr<Value> font_weight(new Value(Identifier(Identifier::NORMAL)));
  // The system font size used by actual browers
  scoped_ptr<Value> font_size(new Value(32.0/3, Value::PX));
  scoped_ptr<Value> line_height(new Value(Identifier(Identifier::NORMAL)));
  scoped_ptr<Value> font_family;

  if (!SkipToNextToken())
    return NULL;

  scoped_ptr<Value> v(ParseAny());
  if (!v.get()) return NULL;

  if (v->GetLexicalUnitType() == Value::IDENT) {
    switch (v->GetIdentifier().ident()) {
      case Identifier::CAPTION:
      case Identifier::ICON:
      case Identifier::MENU:
      case Identifier::MESSAGE_BOX:
      case Identifier::SMALL_CAPTION:
      case Identifier::STATUS_BAR:
        font_family.reset(v.release());
        break;
      case Identifier::INHERIT:
        font_style.reset(new Value(*v));
        font_variant.reset(new Value(*v));
        font_weight.reset(new Value(*v));
        font_size.reset(new Value(*v));
        line_height.reset(new Value(*v));
        font_family.reset(v.release());
        break;
      default:
        break;
    }
  }

  if (font_family.get()) {
    // shouldn't seen more tokens if system font name or inherit has been seen
    if (SkipToNextToken()) return NULL;
    values->push_back(font_style.release());
    values->push_back(font_variant.release());
    values->push_back(font_weight.release());
    values->push_back(font_size.release());
    values->push_back(line_height.release());
    values->push_back(font_family.release());
    return values.release();
  }

  // parse style, variant and weight
  while (true) {
    // Firefox allows only one value to be set per property, IE need not.
    if (v->GetLexicalUnitType() == Value::IDENT) {
      switch (v->GetIdentifier().ident()) {
        case Identifier::NORMAL:
          // no-op
          break;
        case Identifier::ITALIC:
        case Identifier::OBLIQUE:
          font_style.reset(v.release());
          break;
        case Identifier::SMALL_CAPS:
          font_variant.reset(v.release());
          break;
        case Identifier::BOLD:
        case Identifier::BOLDER:
        case Identifier::LIGHTER:
          font_weight.reset(v.release());
          break;
        default:
          goto check_fontsize;
      }
    } else if (v->GetLexicalUnitType() == Value::NUMBER &&
               v->GetDimension() == Value::NO_UNIT) {
      switch (v->GetIntegerValue()) {
        // Different browsers handle this quite differently. But there
        // is at least a test that is consistent between IE and
        // firefox: try <span style="font:120 serif"> and <span
        // style="font:100 serif">, the first one treats 120 as
        // font-size, and the second does not.
        case 100: case 200: case 300: case 400:
        case 500: case 600: case 700: case 800:
        case 900:
          font_weight.reset(v.release());
          break;
        default:
          goto check_fontsize;
      }
    } else {
      goto check_fontsize;
    }
    if (!SkipToNextToken())
      return NULL;
    v.reset(ParseAny());
    if (!v.get()) return NULL;
  }

 check_fontsize:
  // parse font-size
  switch (v->GetLexicalUnitType()) {
    case Value::IDENT:
      switch (v->GetIdentifier().ident()) {
        case Identifier::XX_SMALL:
        case Identifier::X_SMALL:
        case Identifier::SMALL:
        case Identifier::MEDIUM:
        case Identifier::LARGE:
        case Identifier::X_LARGE:
        case Identifier::XX_LARGE:
        case Identifier::LARGER:
        case Identifier::SMALLER:
          font_size.reset(v.release());
          break;
        default:
          return NULL;
      }
      break;
    case Value::NUMBER:
      font_size.reset(v.release());
      break;
    default:
      return NULL;
  }

  // parse line-height if '/' is seen, or use the default line-height
  if (SkipToNextToken() && *in_ == '/') {
    in_++;
    if (!SkipToNextToken()) return NULL;
    v.reset(ParseAny());
    if (!v.get()) return NULL;

    switch (v->GetLexicalUnitType()) {
      case Value::IDENT:
        if (v->GetIdentifier().ident() == Identifier::NORMAL)
          break;
        else
          return NULL;
      case Value::NUMBER:
        line_height.reset(v.release());
        break;
      default:
        return NULL;
    }
  }

  values->push_back(font_style.release());
  values->push_back(font_variant.release());
  values->push_back(font_weight.release());
  values->push_back(font_size.release());
  values->push_back(line_height.release());

  if (!ParseFontFamily(values.get()))  // empty is okay.
    return NULL;
  return values.release();
}

static void ExpandShorthandProperties(
    Declarations* declarations, Property prop, Values* vals, bool important) {
  // TODO(yian): We currently store both expanded properties and the original
  // property because only limited expansion is supported. In future, we should
  // discard the original property after expansion.
  declarations->push_back(new Declaration(prop, vals, important));
  switch (prop.prop()) {
    case Property::FONT: {
      CHECK_GE(vals->size(), 5);
      declarations->push_back(
          new Declaration(Property::FONT_STYLE, *vals->get(0), important));
      declarations->push_back(
          new Declaration(Property::FONT_VARIANT, *vals->get(1), important));
      declarations->push_back(
          new Declaration(Property::FONT_WEIGHT, *vals->get(2), important));
      declarations->push_back(
          new Declaration(Property::FONT_SIZE, *vals->get(3), important));
      declarations->push_back(
          new Declaration(Property::LINE_HEIGHT, *vals->get(4), important));
      if (vals->size() > 5) {
        Values* family_vals = new Values;
        for (int i = 5, n = vals->size(); i < n; ++i)
          family_vals->push_back(new Value(*vals->get(i)));
        declarations->push_back(
            new Declaration(Property::FONT_FAMILY, family_vals, important));
      }
    }
      break;
    default:
      // TODO(yian): other shorthand properties:
      // background-position
      // border-color border-style border-width
      // border-top border-right border-bottom border-left
      // border
      // margin padding
      // outline
      break;
  }
}

// Parse declarations like "background: white; color: #333; line-height: 1.3;"
Declarations* Parser::ParseRawDeclarations() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return new Declarations();
  DCHECK_LT(in_, end_);

  Declarations* declarations = new Declarations();
  while (in_ < end_) {
    bool ignore_this_decl = false;
    switch (*in_) {
      case ';':
        in_++;
        break;
      case '}':
        return declarations;
      default: {
        UnicodeText id = ParseIdent();
        if (id.empty()) {
          ignore_this_decl = true;
          break;
        }
        Property prop(id);
        SkipSpace();
        if (Done() || *in_ != ':') {
          ignore_this_decl = true;
          break;
        }
        in_++;

        Values* vals;
        switch (prop.prop()) {
          // TODO(sligocki): stop special-casing.
          case Property::FONT:
            vals = ParseFont();
            break;
          case Property::FONT_FAMILY:
            vals = new Values();
            if (!ParseFontFamily(vals) || vals->empty()) {
              delete vals;
              vals = NULL;
            }
            break;
          default:
            vals = ParseValues(prop.prop());
            break;
        }

        if (vals == NULL) {
          ignore_this_decl = true;
          break;
        }

        bool important = false;
        if (in_ < end_ && *in_ == '!') {
          in_++;
          SkipSpace();
          UnicodeText ident = ParseIdent();
          if (ident.utf8_length() == 9 &&
              !memcasecmp(ident.utf8_data(), "important", 9))
            important = true;
        }
        ExpandShorthandProperties(declarations, prop, vals, important);
      }
    }
    SkipSpace();
    if (ignore_this_decl) {  // on bad syntax, we skip till the next declaration
      while (in_ < end_ && *in_ != ';' && *in_ != '}') {
        // IE (and IE only) ignores {} blocks in quirks mode.
        if (*in_ == '{' && !quirks_mode_) {
          ParseBlock();  // ignore
        } else {
          in_++;
          SkipSpace();
        }
      }
    }
  }
  return declarations;
}

Declarations* Parser::ExpandDeclarations(Declarations* orig_declarations) {
  scoped_ptr<Declarations> new_declarations(new Declarations);
  for (int j = 0; j < orig_declarations->size(); ++j) {
    // new_declarations takes ownership of declaration.
    Declaration* declaration = orig_declarations->at(j);
    orig_declarations->at(j) = NULL;
    new_declarations->push_back(declaration);
    switch (declaration->property().prop()) {
      case Css::Property::BACKGROUND: {
        ExpandBackground(*declaration, new_declarations.get());
        break;
      }
        /*
          case Css::Property::FONT:
          //TODO
          break;
          case Css::Property::FONT_FAMILY:
          //TODO
          break;
        */
      default:
        break;
    }
  }
  return new_declarations.release();
}

Declarations* Parser::ParseDeclarations() {
  scoped_ptr<Declarations> orig_declarations(ParseRawDeclarations());
  return ExpandDeclarations(orig_declarations.get());
}

// Starts from [ and parses to the closing ]
// in [ foo ~= bar ].
// Whitespace is not skipped at beginning or the end.
SimpleSelector* Parser::ParseAttributeSelector() {
  Tracer trace(__func__, &in_);

  DCHECK_LT(in_, end_);
  DCHECK_EQ('[', *in_);
  in_++;
  SkipSpace();

  UnicodeText attr = ParseIdent();
  SkipSpace();
  scoped_ptr<SimpleSelector> newcond;
  if (!attr.empty() && in_ < end_) {
    char oper = *in_;
    switch (*in_) {
      case '~':
      case '|':
      case '^':
      case '$':
      case '*':
        in_++;
        if (Done() || *in_ != '=')
          break;
        // fall through
      case '=': {
        in_++;
        UnicodeText value = ParseStringOrIdent();
        if (!value.empty())
          newcond.reset(SimpleSelector::NewBinaryAttribute(
              SimpleSelector::AttributeTypeFromOperator(oper),
              attr,
              value));
        break;
      }
      default:
        newcond.reset(SimpleSelector::NewExistAttribute(attr));
        break;
    }
  }
  if (SkipPastDelimiter(']'))
    return newcond.release();
  else
    return NULL;
}

SimpleSelector* Parser::ParseSimpleSelector() {
  Tracer trace(__func__, &in_);

  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  switch (*in_) {
    case '#': {
      in_++;
      UnicodeText id = ParseIdent();
      if (!id.empty())
        return SimpleSelector::NewId(id);
      break;
    }
    case '.': {
      in_++;
      UnicodeText classname = ParseIdent();
      if (!classname.empty())
        return SimpleSelector::NewClass(classname);
      break;
    }
    case ':': {
      in_++;
      UnicodeText pseudoclass = ParseIdent();
      // FIXME(yian): skip constructs "(en)" in lang(en) for now.
      if (in_ < end_ && *in_ == '(') {
        in_++;
        SkipSpace();
        ParseIdent();
        if (!SkipPastDelimiter(')'))
          break;
      }
      if (!pseudoclass.empty())
        return SimpleSelector::NewPseudoclass(pseudoclass);
      break;
    }
    case '[': {
      SimpleSelector* newcond = ParseAttributeSelector();
      if (newcond)
        return newcond;
      break;
    }
    case '*':
      in_++;
      return SimpleSelector::NewUniversal();
      break;
    default: {
      UnicodeText ident = ParseIdent();
      if (!ident.empty())
        return SimpleSelector::NewElementType(ident);
      break;
    }
  }
  // We did not parse anything or we parsed something incorrectly.
  return NULL;
}

bool Parser::AtValidSimpleSelectorsTerminator() const {
  if (Done()) return true;
  switch (*in_) {
    case ' ': case '\t': case '\r': case '\n': case '\f':
    case ',': case '{': case '>': case '+':
      return true;
    case '/':
      if (in_ + 1 < end_ && *(in_ + 1) == '*')
        return true;
      break;
  }
  return false;
}

SimpleSelectors* Parser::ParseSimpleSelectors(bool expecting_combinator) {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  SimpleSelectors::Combinator combinator;
  if (!expecting_combinator)
    combinator = SimpleSelectors::NONE;
  else
    switch (*in_) {
      case '>':
        in_++;
        combinator = SimpleSelectors::CHILD;
        break;
      case '+':
        in_++;
        combinator = SimpleSelectors::SIBLING;
        break;
      default:
        combinator = SimpleSelectors::DESCENDANT;
        break;
    }

  scoped_ptr<SimpleSelectors> selectors(
      new SimpleSelectors(combinator));

  SkipSpace();
  if (Done()) return NULL;

  const char* oldin = in_;
  while (SimpleSelector* simpleselector = ParseSimpleSelector()) {
    selectors->push_back(simpleselector);
    oldin = in_;
  }

  if (selectors->size() > 0 &&  // at least one simple selector stored
      in_ == oldin &&         // the last NULL does not make progress
      AtValidSimpleSelectorsTerminator())  // stop at a valid terminator
    return selectors.release();

  return NULL;
}

Selectors* Parser::ParseSelectors() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  // Remember whether anything goes wrong, but continue parsing until the
  // declaration starts or the position comes to the end. Then discard the
  // selectors.
  bool success = true;

  scoped_ptr<Selectors> selectors(new Selectors());
  Selector* selector = new Selector();
  selectors->push_back(selector);

  // The first simple selector sequence in a chain of simple selector
  // sequences does not have a combinator.  ParseSimpleSelectors needs
  // to know this, so we set this to false here and after ',', and
  // true after we see a simple selector sequence.
  bool expecting_combinator = false;
  while (in_ < end_ && *in_ != '{') {
    switch (*in_) {
      case ',':
        if (selector->size() == 0) {
          success = false;
        } else {
          selector = new Selector();
          selectors->push_back(selector);
        }
        in_++;
        expecting_combinator = false;
        break;
      default: {
        const char* oldin = in_;
        SimpleSelectors* simple_selectors
          = ParseSimpleSelectors(expecting_combinator);
        if (!simple_selectors) {
          success = false;
          if (in_ == oldin)
            in_++;
        } else {
          selector->push_back(simple_selectors);
        }

        expecting_combinator = true;
        break;
      }
    }
    SkipSpace();
  }

  if (selector->size() == 0)
    success = false;

  if (success)
    return selectors.release();
  else
    return NULL;
}

Ruleset* Parser::ParseRuleset() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  // Remember whether anything goes wrong, but continue parsing until the
  // closing }. Then discard the whole ruleset if necessary. This allows the
  // parser to make progress anyway.
  bool success = true;

  scoped_ptr<Ruleset> ruleset(new Ruleset());
  scoped_ptr<Selectors> selectors(ParseSelectors());

  if (Done())
    return NULL;

  if (selectors.get() == NULL) {
    // http://www.w3.org/TR/CSS21/syndata.html#rule-sets
    // When a user agent can't parse the selector (i.e., it is not
    // valid CSS 2.1), it must ignore the declaration block as
    // well.
    success = false;
  } else {
    ruleset->set_selectors(selectors.release());
  }

  in_++;  // '{'
  ruleset->set_declarations(ParseRawDeclarations());
  SkipPastDelimiter('}');

  if (success)
    return ruleset.release();
  else
    return NULL;
}

void Parser::ParseMediumList(std::vector<UnicodeText>* media) {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return;
  DCHECK_LT(in_, end_);

  while (in_ < end_) {
    switch (*in_) {
      case ';':
      case '{':
        return;
      case ',':
        in_++;
        break;
      default:
        scoped_ptr<Value> v(ParseAny());
        if (v.get() && v->GetLexicalUnitType() == Value::IDENT)
          media->push_back(v->GetIdentifierText());

        break;
    }
    SkipSpace();
  }
}

// Start after @import is parsed.
Import* Parser::ParseImport() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return NULL;
  DCHECK_LT(in_, end_);

  scoped_ptr<Value> v(ParseAny());
  if (!v.get() || (v->GetLexicalUnitType() != Value::STRING &&
                   v->GetLexicalUnitType() != Value::URI))
    return NULL;

  Import* import = new Import();
  import->link = v->GetStringValue();

  ParseMediumList(&import->media);
  if (in_ < end_ && *in_ == ';') in_++;
  return import;
}

void Parser::ParseAtrule(Stylesheet* stylesheet) {
  Tracer trace(__func__, &in_);

  SkipSpace();
  DCHECK_LT(in_, end_);
  DCHECK_EQ('@', *in_);
  in_++;

  UnicodeText ident = ParseIdent();

  // @import string|uri medium-list ? ;
  if (ident.utf8_length() == 6 &&
      memcasecmp(ident.utf8_data(), "import", 6) == 0) {
    scoped_ptr<Import> import(ParseImport());
    if (import.get() && stylesheet)
      stylesheet->mutable_imports().push_back(import.release());

  // @charset string ;
  } else if (ident.utf8_length() == 7 &&
      memcasecmp(ident.utf8_data(), "charset", 7) == 0) {
    SkipPastDelimiter(';');

  // @media medium-list { ruleset-list }
  } else if (ident.utf8_length() == 5 &&
      memcasecmp(ident.utf8_data(), "media", 5) == 0) {
    std::vector<UnicodeText> media;
    ParseMediumList(&media);
    if (Done() || *in_ != '{')
      return;
    in_++;
    SkipSpace();
    while (in_ < end_ && *in_ != '}') {
      const char* oldin = in_;
      scoped_ptr<Ruleset> ruleset(ParseRuleset());
      if (!ruleset.get() && in_ == oldin)
        in_++;
      if (ruleset.get()) {
        ruleset->set_media(media);
        stylesheet->mutable_rulesets().push_back(ruleset.release());
      }
      SkipSpace();
    }
    if (in_ < end_) in_++;

  // @page pseudo_page? { declaration-list }
  } else if (ident.utf8_length() == 4 &&
      memcasecmp(ident.utf8_data(), "page", 4) == 0) {
    delete ParseRuleset();
  }
}

// TODO(dpeng): What exactly does this code do?
void Parser::ParseBlock() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  DCHECK_LT(in_, end_);
  DCHECK_EQ('{', *in_);
  int depth = 0;
  while (in_ < end_) {
    switch (*in_) {
      case '{':
        in_++;
        depth++;
        break;
      case '@':
        in_++;
        ParseIdent();
        break;
      case ';':
        in_++;
        break;
      case '}':
        in_++;
        depth--;
        if (depth == 0)
          return;
        break;
      default:
        scoped_ptr<Value> v(ParseAny());
        break;
    }
    SkipSpace();
  }
}

Stylesheet* Parser::ParseRawStylesheet() {
  Tracer trace(__func__, &in_);

  SkipSpace();
  if (Done()) return new Stylesheet();
  DCHECK_LT(in_, end_);

  Stylesheet* stylesheet = new Stylesheet();
  while (in_ < end_) {
    switch (*in_) {
      case '<':
        in_++;
        if (end_ - in_ >= 3 && memcmp(in_, "!--", 3) == 0) {
          in_ += 3;
        }
        break;
      case '-':
        in_++;
        if (end_ - in_ >= 2 && memcmp(in_, "->", 2) == 0) {
          in_ += 2;
        }
        break;
      case '@':
        ParseAtrule(stylesheet);
        break;
      default: {
        const char* oldin = in_;
        scoped_ptr<Ruleset> ruleset(ParseRuleset());
        if (!ruleset.get() && oldin == in_)
          in_++;
        if (ruleset.get())
          stylesheet->mutable_rulesets().push_back(ruleset.release());
        break;
      }
    }
    SkipSpace();
  }
  return stylesheet;
}

Stylesheet* Parser::ParseStylesheet() {
  Tracer trace(__func__, &in_);

  Stylesheet* stylesheet = ParseRawStylesheet();

  Rulesets& rulesets = stylesheet->mutable_rulesets();
  for (int i = 0; i < rulesets.size(); ++i) {
    Declarations& orig_declarations = rulesets[i]->mutable_declarations();
    rulesets[i]->set_declarations(ExpandDeclarations(&orig_declarations));
  }

  return stylesheet;
}

//
// Some destructors that need STLDeleteElements() from stl_util-inl.h
//

Declarations::~Declarations() { STLDeleteElements(this); }
Rulesets::~Rulesets() { STLDeleteElements(this); }
Imports::~Imports() { STLDeleteElements(this); }

}  // namespace
