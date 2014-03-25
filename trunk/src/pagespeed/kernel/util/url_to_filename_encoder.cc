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
// Author: mbelshe@google.com (Mike Belshe)
//         jmarantz@google.com (Joshua Marantz)

#include "pagespeed/kernel/util/url_to_filename_encoder.h"

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// The escape character choice is made here -- all code and tests in this
// directory are based off of this constant.  However, lots of tests
// have dependencies on this, so it cannot be changed without re-running those
// tests and fixing them.
const char UrlToFilenameEncoder::kEscapeChar = ',';
const char UrlToFilenameEncoder::kTruncationChar = '-';
const size_t UrlToFilenameEncoder::kMaximumSubdirectoryLength = 128;

void UrlToFilenameEncoder::AppendSegment(GoogleString* segment,
                                         GoogleString* dest) {
  CHECK(!segment->empty());
  if ((*segment == ".") || (*segment == "..")) {
    dest->push_back(kEscapeChar);
    dest->append(*segment);
    segment->clear();
  } else {
    size_t segment_size = segment->size();
    if (segment_size > kMaximumSubdirectoryLength) {
      // We need to inject ",-" at the end of the segment to signify that
      // we are inserting an artificial '/'.  This means we have to chop
      // off at least two characters to make room.
      segment_size = kMaximumSubdirectoryLength - 2;

      // But we don't want to break up an escape sequence that happens to lie at
      // the end.  Escape sequences are at most 2 characters.
      if ((*segment)[segment_size - 1] == kEscapeChar) {
        segment_size -= 1;
      } else if ((*segment)[segment_size - 2] == kEscapeChar) {
        segment_size -= 2;
      }
      dest->append(segment->data(), segment_size);
      dest->push_back(kEscapeChar);
      dest->push_back(kTruncationChar);
      segment->erase(0, segment_size);

      // At this point, if we had segment_size=3, and segment="abcd",
      // then after this erase, we will have written "abc,-" and set segment="d"
    } else {
      dest->append(*segment);
      segment->clear();
    }
  }
}

void UrlToFilenameEncoder::EncodeSegment(const StringPiece& filename_prefix,
                                         const StringPiece& escaped_ending,
                                         char dir_separator,
                                         GoogleString* encoded_filename) {
  GoogleString filename_ending = Unescape(escaped_ending);

  char encoded[3];
  int encoded_len;
  GoogleString segment;

  // TODO(jmarantz): This code would be a bit simpler if we disallowed
  // Instaweb allowing filename_prefix to not end in "/".  We could
  // then change the is routine to just take one input string.
  size_t start_of_segment = filename_prefix.find_last_of(dir_separator);
  if (start_of_segment == GoogleString::npos) {
    filename_prefix.CopyToString(&segment);
  } else {
    filename_prefix.substr(start_of_segment + 1).CopyToString(&segment);
    filename_prefix.substr(0, start_of_segment + 1).CopyToString(
        encoded_filename);
  }

  size_t index = 0;
  // Special case the first / to avoid adding a leading kEscapeChar.
  if (!filename_ending.empty() && (filename_ending[0] == dir_separator)) {
    encoded_filename->append(segment);
    segment.clear();
    encoded_filename->push_back(dir_separator);
    ++index;
  }

  for (; index < filename_ending.length(); ++index) {
    unsigned char ch = static_cast<unsigned char>(filename_ending[index]);

    // Note: instead of outputing an empty segment, we let the second slash
    // be escaped below.
    if ((ch == dir_separator) && !segment.empty()) {
      AppendSegment(&segment, encoded_filename);
      encoded_filename->push_back(dir_separator);
      segment.clear();
    } else {
      // After removing unsafe chars the only safe ones are _.=+- and alphanums.
      if ((ch == '_') || (ch == '.') || (ch == '=') || (ch == '+') ||
          (ch == '-') || (('0' <= ch) && (ch <= '9')) ||
          (('A' <= ch) && (ch <= 'Z')) || (('a' <= ch) && (ch <= 'z'))) {
        encoded[0] = ch;
        encoded_len = 1;
      } else {
        encoded[0] = kEscapeChar;
        encoded[1] = ch / 16;
        encoded[1] += (encoded[1] >= 10) ? 'A' - 10 : '0';
        encoded[2] = ch % 16;
        encoded[2] += (encoded[2] >= 10) ? 'A' - 10 : '0';
        encoded_len = 3;
      }
      segment.append(encoded, encoded_len);

      // If segment is too big, we must chop it into chunks.
      if (segment.size() > kMaximumSubdirectoryLength) {
        AppendSegment(&segment, encoded_filename);
        encoded_filename->push_back(dir_separator);
      }
    }
  }

  // Append "," to the leaf filename so the leaf can also be a branch., e.g.
  // allow http://a/b/c and http://a/b/c/d to co-exist as files "/a/b/c," and
  // /a/b/c/d".  So we will rename the "d" here to "d,".  If doing that pushed
  // us over the 128 char limit, then we will need to append "/" and the
  // remaining chars.
  segment += kEscapeChar;
  AppendSegment(&segment, encoded_filename);
  if (!segment.empty()) {
    // The last overflow segment is special, because we appended in
    // kEscapeChar above.  We won't need to check it again for size
    // or further escaping.
    encoded_filename->push_back(dir_separator);
    encoded_filename->append(segment);
  }
}

namespace {

// Parsing states for UrlToFilenameEncoder::Unescape
enum UnescapeState {
  NORMAL,   // We are not in the middle of parsing an escape.
  ESCAPE1,  // We just parsed % .
  ESCAPE2   // We just parsed %X for some hex digit X.
};

int HexStringToInt(const GoogleString& value) {
  uint32 good_val = 0;
  for (int c = 0, n = value.size(); c < n; ++c) {
    bool ok = AccumulateHexValue(value[c], &good_val);
    if (!ok) {
      return -1;
    }
  }
  return static_cast<int>(good_val);
}

}  // namespace

GoogleString UrlToFilenameEncoder::Unescape(const StringPiece& escaped_url) {
  GoogleString unescaped_url, escape_text;
  unsigned char escape_value;
  UnescapeState state = NORMAL;
  int iter = 0;
  int n = escaped_url.size();
  while (iter < n) {
    char c = escaped_url[iter];
    switch (state) {
      case NORMAL:
        if (c == '%') {
          escape_text.clear();
          state = ESCAPE1;
        } else {
          unescaped_url.push_back(c);
        }
        ++iter;
        break;
      case ESCAPE1:
        if (IsHexDigit(c)) {
          escape_text.push_back(c);
          state = ESCAPE2;
          ++iter;
        } else {
          // Unexpected, % followed by non-hex chars, pass it through.
          unescaped_url.push_back('%');
          state = NORMAL;
        }
        break;
      case ESCAPE2:
        if (IsHexDigit(c)) {
          escape_text.push_back(c);
          escape_value = HexStringToInt(escape_text);
          unescaped_url.push_back(escape_value);
          state = NORMAL;
          ++iter;
        } else {
          // Unexpected, % followed by non-hex chars, pass it through.
          unescaped_url.push_back('%');
          unescaped_url.append(escape_text);
          state = NORMAL;
        }
        break;
    }
  }
  // Unexpected, % followed by end of string, pass it through.
  if (state == ESCAPE1 || state == ESCAPE2) {
    unescaped_url.push_back('%');
    unescaped_url.append(escape_text);
  }
  return unescaped_url;
}

}  // namespace net_instaweb
