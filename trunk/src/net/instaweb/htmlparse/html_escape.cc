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

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/htmlparse/public/html_escape.h"
#include <set>

namespace {

struct HtmlEscapeSequence {
  const char* sequence;
  const unsigned char value[3];
};

// TODO(jmarantz): the multi-byte sequences are not working yet.
static HtmlEscapeSequence kHtmlEscapeSequences[] = {
  { "AElig", {0xC6, 0x0} },
  { "Aacute", {0xC1, 0x0} },
  { "Acirc", {0xC2, 0x0} },
  { "Agrave", {0xC0, 0x0} },
  { "Aring", {0xC5, 0x0} },
  { "Atilde", {0xC3, 0x0} },
  { "Auml", {0xC4, 0x0} },
  { "Ccedil", {0xC7, 0x0} },
  { "ETH", {0xD0, 0x0} },
  { "Eacute", {0xC9, 0x0} },
  { "Ecirc", {0xCA, 0x0} },
  { "Egrave", {0xC8, 0x0} },
  { "Euml", {0xCB, 0x0} },
  { "Iacute", {0xCD, 0x0} },
  { "Icirc", {0xCE, 0x0} },
  { "Igrave", {0xCC, 0x0} },
  { "Iuml", {0xCF, 0x0} },
  { "Ntilde", {0xD1, 0x0} },
  { "Oacute", {0xD3, 0x0} },
  { "Ocirc", {0xD4, 0x0} },
  { "Ograve", {0xD2, 0x0} },
  { "Oslash", {0xD8, 0x0} },
  { "Otilde", {0xD5, 0x0} },
  { "Ouml", {0xD6, 0x0} },
  { "THORN", {0xDE, 0x0} },
  { "Uacute", {0xDA, 0x0} },
  { "Ucirc", {0xDB, 0x0} },
  { "Ugrave", {0xD9, 0x0} },
  { "Uuml", {0xDC, 0x0} },
  { "Yacute", {0xDD, 0x0} },
  { "aacute", {0xE1, 0x0} },
  { "acirc", {0xE2, 0x0} },
  { "acute", {0xB4, 0x0} },
  { "aelig", {0xE6, 0x0} },
  { "agrave", {0xE0, 0x0} },
  { "amp", {0x26, 0x0} },
  { "aring", {0xE5, 0x0} },
  { "atilde", {0xE3, 0x0} },
  { "auml", {0xE4, 0x0} },
  { "brvbar", {0xA6, 0x0} },
  { "ccedil", {0xE7, 0x0} },
  { "cedil", {0xB8, 0x0} },
  { "cent", {0xA2, 0x0} },
  { "copy", {0xA9, 0x0} },
  { "curren", {0xA4, 0x0} },
  { "deg", {0xB0, 0x0} },
  { "divide", {0xF7, 0x0} },
  { "eacute", {0xE9, 0x0} },
  { "ecirc", {0xEA, 0x0} },
  { "egrave", {0xE8, 0x0} },
  { "eth", {0xF0, 0x0} },
  { "euml", {0xEB, 0x0} },
  { "frac12", {0xBD, 0x0} },
  { "frac14", {0xBC, 0x0} },
  { "frac34", {0xBE, 0x0} },
  { "gt", {0x3E, 0x0} },
  { "iacute", {0xED, 0x0} },
  { "icirc", {0xEE, 0x0} },
  { "iexcl", {0xA1, 0x0} },
  { "igrave", {0xEC, 0x0} },
  { "iquest", {0xBF, 0x0} },
  { "iuml", {0xEF, 0x0} },
  { "laquo", {0xAB, 0x0} },
  { "lt", {0x3C, 0x0} },
  { "macr", {0xAF, 0x0} },
  { "micro", {0xB5, 0x0} },
  { "middot", {0xB7, 0x0} },
  { "nbsp", {0xA0, 0x0} },
  { "not", {0xAC, 0x0} },
  { "ntilde", {0xF1, 0x0} },
  { "oacute", {0xF3, 0x0} },
  { "ocirc", {0xF4, 0x0} },
  { "ograve", {0xF2, 0x0} },
  { "ordf", {0xAA, 0x0} },
  { "ordm", {0xBA, 0x0} },
  { "oslash", {0xF8, 0x0} },
  { "otilde", {0xF5, 0x0} },
  { "ouml", {0xF6, 0x0} },
  { "para", {0xB6, 0x0} },
  { "plusmn", {0xB1, 0x0} },
  { "pound", {0xA3, 0x0} },
  { "quot", {0x22, 0x0} },
  { "raquo", {0xBB, 0x0} },
  { "reg", {0xAE, 0x0} },
  { "sect", {0xA7, 0x0} },
  { "shy", {0xAD, 0x0} },
  { "sup1", {0xB9, 0x0} },
  { "sup2", {0xB2, 0x0} },
  { "sup3", {0xB3, 0x0} },
  { "szlig", {0xDF, 0x0} },
  { "thorn", {0xFE, 0x0} },
  { "times", {0xD7, 0x0} },
  { "uacute", {0xFA, 0x0} },
  { "ucirc", {0xFB, 0x0} },
  { "ugrave", {0xF9, 0x0} },
  { "uml", {0xA8, 0x0} },
  { "uuml", {0xFC, 0x0} },
  { "yacute", {0xFD, 0x0} },
  { "yen", {0xA5, 0x0} },
  { "yuml", {0xFF, 0x0} }
};

}  // namespace

namespace net_instaweb {

HtmlEscape* HtmlEscape::singleton_ = NULL;

HtmlEscape::HtmlEscape() {
  typedef std::set<std::string, StringCompareInsensitive> CaseInsensitiveSet;
  CaseInsensitiveSet case_sensitive_symbols;
  for (size_t i = 0; i < arraysize(kHtmlEscapeSequences); ++i) {
    // Put all symbols in the case-sensitive map
    const HtmlEscapeSequence& seq = kHtmlEscapeSequences[i];
    unescape_sensitive_map_[seq.sequence] =
        reinterpret_cast<const char*>(seq.value);

    // Don't populate the case-insensitive map for symbols that we've
    // already determined are case-sensitive.
    if (case_sensitive_symbols.find(seq.sequence) ==
        case_sensitive_symbols.end()) {
      // If this symbol is already present in the insensitive map, then it
      // must be case-sensitive.  E.g. &AElig; and &aelig; are distinct.
      StringStringMapInsensitive::iterator p =
          unescape_insensitive_map_.find(seq.sequence);
      if (p != unescape_insensitive_map_.end()) {
        // As this symbol is case-sensitive, we must remove it from the
        // case-insensitive map.  This way we will report an error for
        // &Aelig;, rather than &AElig; or &aelig; unpredictably.  Also,
        // note that this symbol is case-sensitive, and therefore must
        // be not be ent
        unescape_insensitive_map_.erase(p);
        case_sensitive_symbols.insert(seq.sequence);
      } else {
        unescape_insensitive_map_[seq.sequence] =
            reinterpret_cast<const char*>(seq.value);
      }

      // For now, we will only generate symbolic escaped-names for
      // single-byte sequences
      if (strlen(reinterpret_cast<const char*>(seq.value)) == 1) {
        escape_map_[reinterpret_cast<const char*>(seq.value)] = seq.sequence;
      }
    }
  }
}

void HtmlEscape::Init() {
  if (singleton_ == NULL) {
    singleton_ = new HtmlEscape();
  }
}

void HtmlEscape::ShutDown() {
  if (singleton_ != NULL) {
    delete singleton_;
    singleton_ = NULL;
  }
}

bool HtmlEscape::AccumulateHexValue(char c, int* value) {
  *value *= 16;
  bool ret = true;
  if ((c >= '0') && (c <= '9')) {
    *value += c - '0';
  } else if ((c >= 'a') && (c <= 'f')) {
    *value += 10 + c - 'a';
  } else if ((c >= 'A') && (c <= 'F')) {
    *value += 10 + c - 'A';
  } else {
    ret = false;
  }
  return ret;
}

StringPiece HtmlEscape::UnescapeHelper(const StringPiece& escaped,
                                       std::string* buf) const {
  if (escaped.data() == NULL) {
    return escaped;
  }
  buf->clear();

  // Attribute values may have HTML escapes in them, e.g.
  //    href="host.com/path?v1&amp;v2"
  // Un-escape the attribute value here before populating the
  // attribute data structure.
  std::string escape;
  int hex_value = 0;
  bool accumulate_hex_code = false;
  bool in_escape = false;
  for (size_t i = 0; i < escaped.size(); ++i) {
    char ch = escaped[i];
    bool bogus_escape = false;
    if (!in_escape) {
      if (ch == '&') {
        in_escape = true;
        escape.clear();
        hex_value = 0;
        accumulate_hex_code = false;
      } else {
        *buf += ch;
      }
    } else if (escape.empty() && (ch == '#')) {
      escape += ch;
      accumulate_hex_code = true;
    } else if (ch == ';') {
      if (accumulate_hex_code && (escape.size() > 1)) {
        *buf += static_cast<char>(hex_value);
      } else {
        // Some symbols are case-sensitive (AElig vs aelig are different
        // code-points) where as some are case-insensitive (&quot; and
        // &QUOT; both work.  So do the case-sensitive lookup first, and
        // if that fails, do an insensitive lookup.
        StringStringMapSensitive::const_iterator p =
            unescape_sensitive_map_.find(escape);
        if (p != unescape_sensitive_map_.end()) {
          *buf += p->second;
          // TODO(jmarantz): fix this code for multi-byte sequences.
        } else {
          // The sensitive lookup failed, but allow, for example, &QUOT; to work
          // in place of &quot;
          StringStringMapInsensitive::const_iterator q =
              unescape_insensitive_map_.find(escape);
          if (q != unescape_insensitive_map_.end()) {
            *buf += q->second;
          } else {
            bogus_escape = true;
          }
        }
      }
      in_escape = false;
    } else if (accumulate_hex_code && !AccumulateHexValue(ch, &hex_value)) {
      bogus_escape = true;
    } else {
      escape += ch;
    }
    if (bogus_escape) {
      // Error("Invalid escape syntax: %s", escape.c_str());
      *buf += "&";
      *buf += escape;
      in_escape = false;
      *buf += ch;
    }
  }
  if (in_escape) {
    // Error("Unterminated escape: %s", escape.c_str());
    *buf += "&";
    *buf += escape;
  }
  return StringPiece(*buf);
}

StringPiece HtmlEscape::EscapeHelper(const StringPiece& unescaped,
                                     std::string* buf) const {
  if (unescaped.data() == NULL) {
    return unescaped;
  }
  buf->clear();

  std::string char_to_escape;
  for (size_t i = 0; i < unescaped.size(); ++i) {
    int ch = unescaped[i];
    // See http://www.htmlescape.net/htmlescape_tool.html.  Single-quote and
    // semi-colon do not need to be escaped.
    if ((ch > 127) || (ch < 32) || (ch == '"') || (ch == '&') || (ch == '<') ||
        (ch == '>')) {
      char_to_escape.clear();
      char_to_escape += ch;
      StringStringMapSensitive::const_iterator p =
          escape_map_.find(char_to_escape);
      if (p == escape_map_.end()) {
        *buf += StringPrintf("&#%02x;", static_cast<int>(ch));
      } else {
        *buf += '&';
        *buf += p->second;
        *buf += ';';
      }
    } else {
      *buf += unescaped[i];
    }
  }
  return StringPiece(*buf);
}

}  // namespace
