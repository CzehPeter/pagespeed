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

// Author: sligocki@google.com (Shawn Ligocki)

#include "net/instaweb/rewriter/public/css_minify.h"

#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/writer.h"
#include "webutil/css/parser.h"

namespace net_instaweb {

namespace {

// Escape [(), \t\r\n\\'"]
std::string CSSEscapeString(const StringPiece& src) {
  const int dest_length = src.size() * 2 + 1;  // Maximum possible expansion
  scoped_array<char> dest(new char[dest_length]);

  const char* src_end = src.data() + src.size();
  int used = 0;

  for (const char* p = src.data(); p < src_end; p++) {
    switch (*p) {
      case '\n': dest[used++] = '\\'; dest[used++] = 'n';  break;
      case '\r': dest[used++] = '\\'; dest[used++] = 'r';  break;
      case '\t': dest[used++] = '\\'; dest[used++] = 't';  break;
      case '\"': case '\'': case '\\': case ',': case '(': case ')':
          dest[used++] = '\\';
          dest[used++] = *p;
          break;
      default: dest[used++] = *p; break;
    }
  }

  return std::string(dest.get(), used);
}

std::string CSSEscapeString(const UnicodeText& src) {
  return CSSEscapeString(StringPiece(src.utf8_data(), src.utf8_length()));
}

}  // namespace

bool CssMinify::Stylesheet(const Css::Stylesheet& stylesheet,
                           Writer* writer,
                           MessageHandler* handler) {
  // Get an object to encapsulate writing.
  CssMinify minifier(writer, handler);
  minifier.Minify(stylesheet);
  return minifier.ok_;
}

CssMinify::CssMinify(Writer* writer, MessageHandler* handler)
    : writer_(writer), handler_(handler), ok_(true) {
}

CssMinify::~CssMinify() {
}

// Write if we have not encountered write error yet.
void CssMinify::Write(const StringPiece& str) {
  if (ok_) {
    ok_ &= writer_->Write(str, handler_);
  }
}

// Write out each element of vector using supplied function seperated by sep.
template<typename Container>
void CssMinify::JoinWrite(const Container& container, const StringPiece& sep) {
  for (typename Container::const_iterator iter = container.begin();
       iter != container.end(); ++iter) {
    if (iter != container.begin()) {
      Write(sep);
    }
    Minify(**iter);
  }
}

template<typename Container>
void CssMinify::JoinMediaWrite(const Container& container,
                               const StringPiece& sep) {
  for (typename Container::const_iterator iter = container.begin();
       iter != container.end(); ++iter) {
    if (iter != container.begin()) {
      Write(sep);
    }
    Write(CSSEscapeString(*iter));
  }
}


// Write the minified versions of each type.
//   Adapted from webutil/css/tostring.cc

void CssMinify::Minify(const Css::Stylesheet& stylesheet) {
  // We might want to add in unnecessary newlines between rules and imports
  // so that some readability is preserved.
  JoinWrite(stylesheet.imports(), "");
  JoinWrite(stylesheet.rulesets(), "");
}

void CssMinify::Minify(const Css::Import& import) {
  Write("@import url(");
  // TODO(sligocki): Make a URL printer method that absolutifies and prints.
  Write(CSSEscapeString(import.link));
  Write(") ");
  JoinMediaWrite(import.media, ",");
  Write(";");
}

void CssMinify::Minify(const Css::Ruleset& ruleset) {
  if (!ruleset.media().empty()) {
    Write("@media ");
    JoinMediaWrite(ruleset.media(), ",");
    Write("{");
  }

  JoinWrite(ruleset.selectors(), ",");
  Write("{");
  JoinWrite(ruleset.declarations(), ";");
  Write("}");

  if (!ruleset.media().empty()) {
    Write("}");
  }
}

void CssMinify::Minify(const Css::Selector& selector) {
  // Note Css::Selector == std::vector<Css::SimpleSelectors*>
  JoinWrite(selector, " ");
}

void CssMinify::Minify(const Css::SimpleSelectors& sselectors) {
  if (sselectors.combinator() == Css::SimpleSelectors::CHILD) {
    Write("> ");
  } else if (sselectors.combinator() == Css::SimpleSelectors::SIBLING) {
    Write("+ ");
  }
  // Note Css::SimpleSelectors == std::vector<Css::SimpleSelector*>
  JoinWrite(sselectors, "");
}

void CssMinify::Minify(const Css::SimpleSelector& sselector) {
  // SimpleSelector::ToString is already basically minified.
  Write(sselector.ToString());
}

namespace {

// TODO(sligocki): Either make this an accessible function in
// webutil/css/tostring or specialize it for minifier.
//
// Note that currently the style is terrible and it will crash the program if
// we have >= 5 args.
std::string FontToString(const Css::Values& font_values) {
  CHECK_LE(5U, font_values.size());
  std::string tmp, result;

  tmp = font_values.get(0)->ToString();
  if (tmp != "normal") result += tmp + " ";
  tmp = font_values.get(1)->ToString();
  if (tmp != "normal") result += tmp + " ";
  tmp = font_values.get(2)->ToString();
  if (tmp != "normal") result += tmp + " ";
  result += font_values.get(3)->ToString();
  tmp = font_values.get(4)->ToString();
  if (tmp != "normal") result += "/" + tmp;
  for (int i = 5, n = font_values.size(); i < n; ++i)
    result += (i == 5 ? " " : ",") + font_values.get(i)->ToString();

  return result;
}

}  // namespace

void CssMinify::Minify(const Css::Declaration& declaration) {
  Write(declaration.prop_text());
  Write(":");
  switch (declaration.prop()) {
    case Css::Property::FONT_FAMILY:
      JoinWrite(*declaration.values(), ",");
      break;
    case Css::Property::FONT:
      Write(FontToString(*declaration.values()));
      break;
    default:
      JoinWrite(*declaration.values(), " ");
      break;
  }
  if (declaration.IsImportant()) {
    Write(" !important");
  }
}

void CssMinify::Minify(const Css::Value& value) {
  switch (value.GetLexicalUnitType()) {
    case Css::Value::NUMBER:
      // TODO(sligocki): Minify number
      // TODO(sligocki): Check that exponential notation is appropriate.
      Write(StringPrintf("%g%s",
                         value.GetFloatValue(),
                         value.GetDimensionUnitText().c_str()));
      break;
    case Css::Value::URI:
      // TODO(sligocki): Make a URL printer method that absolutifies and prints.
      Write("url(");
      Write(CSSEscapeString(value.GetStringValue()));
      Write(")");
      break;
    case Css::Value::COUNTER:
      Write("counter(");
      Write(value.GetParameters()->ToString());
      Write(")");
      break;
    case Css::Value::FUNCTION:
      Write(CSSEscapeString(value.GetFunctionName()));
      Write("(");
      Write(value.GetParameters()->ToString());
      Write(")");
      break;
    case Css::Value::RECT:
      Write("rect(");
      Write(value.GetParameters()->ToString());
      Write(")");
      break;
    case Css::Value::COLOR:
      // TODO(sligocki): Can we assert, or might this happen in the wild?
      CHECK(value.GetColorValue().IsDefined());
      Write(HtmlColorUtils::MaybeConvertToCssShorthand(
          value.GetColorValue()));
      break;
    case Css::Value::STRING:
      Write("\"");
      Write(CSSEscapeString(value.GetStringValue()));
      Write("\"");
      break;
    case Css::Value::IDENT:
      Write(CSSEscapeString(value.GetIdentifierText()));
      break;
    case Css::Value::UNKNOWN:
      handler_->Message(kError, "Unknown attribute");
      ok_ = false;
      break;
    case Css::Value::DEFAULT:
      break;
  }
}

}  // namespace net_instaweb
