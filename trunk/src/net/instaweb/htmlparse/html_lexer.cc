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

#include "net/instaweb/htmlparse/html_lexer.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstddef>  // for size_t
#include <cstdio>
#include <utility>  // for pair

#include "base/logging.h"
#include "net/instaweb/htmlparse/html_event.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_keywords.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/htmlparse/public/html_parser_types.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

namespace {

// TODO(jmarantz): consider making these sorted-lists be an enum field
// in the table in html_name.gperf.  I'm not sure if that would make things
// noticably faster or not.

// These tags can be specified in documents without a brief "/>",
// or an explicit </tag>, according to the Chrome Developer Tools console.  See:
//
// http://www.whatwg.org/specs/web-apps/current-work/multipage/
// syntax.html#void-elements
const HtmlName::Keyword kImplicitlyClosedHtmlTags[] = {
  HtmlName::kXml,
  HtmlName::kArea,
  HtmlName::kBase,
  HtmlName::kBr,
  HtmlName::kCol,
  HtmlName::kEmbed,
  HtmlName::kHr,
  HtmlName::kImg,
  HtmlName::kInput,
  HtmlName::kKeygen,
  HtmlName::kLink,
  HtmlName::kMeta,
  HtmlName::kParam,
  HtmlName::kSource,
  HtmlName::kTrack,
  HtmlName::kWbr,
};

// These tags cannot be closed using the brief syntax; they must
// be closed by using an explicit </TAG>.
const HtmlName::Keyword kNonBriefTerminatedTags[] = {
  HtmlName::kA,
  HtmlName::kDiv,
  HtmlName::kIframe,
  HtmlName::kScript,
  HtmlName::kSpan,
  HtmlName::kStyle,
  HtmlName::kTextarea,
  HtmlName::kXmp,
};

// These tags cause the text inside them to retained literally
// and not interpreted.
const HtmlName::Keyword kLiteralTags[] = {
  HtmlName::kIframe,
  HtmlName::kScript,
  HtmlName::kStyle,
  HtmlName::kTextarea,
  HtmlName::kXmp,
};

// We start our stack-iterations from 1, because we put a NULL into
// position 0 to reduce special-cases.
const int kStartStack = 1;

#ifndef NDEBUG
#define CHECK_KEYWORD_SET_ORDERING(keywords) \
    CheckKeywordSetOrdering(keywords, arraysize(keywords))
void CheckKeywordSetOrdering(const HtmlName::Keyword* keywords, int num) {
  for (int i = 1; i < num; ++i) {
    DCHECK_GT(keywords[i], keywords[i - 1]);
  }
}
#endif

bool IsInSet(const HtmlName::Keyword* keywords, int num,
             HtmlName::Keyword keyword) {
  const HtmlName::Keyword* end = keywords + num;
  return std::binary_search(keywords, end, keyword);
}

#define IS_IN_SET(keywords, keyword) \
    IsInSet(keywords, arraysize(keywords), keyword)

}  // namespace

// TODO(jmarantz): support multi-byte encodings
// TODO(jmarantz): emit close-tags immediately for selected html tags,
//   rather than waiting for the next explicit close-tag to force a rebalance.
//   See http://www.whatwg.org/specs/web-apps/current-work/multipage/
//   syntax.html#optional-tags

HtmlLexer::HtmlLexer(HtmlParse* html_parse)
    : html_parse_(html_parse),
      state_(START),
      attr_quote_(""),
      has_attr_value_(false),
      element_(NULL),
      line_(1),
      tag_start_line_(-1) {
#ifndef NDEBUG
  CHECK_KEYWORD_SET_ORDERING(kImplicitlyClosedHtmlTags);
  CHECK_KEYWORD_SET_ORDERING(kNonBriefTerminatedTags);
  CHECK_KEYWORD_SET_ORDERING(kLiteralTags);
#endif
}

HtmlLexer::~HtmlLexer() {
}

void HtmlLexer::EvalStart(char c) {
  if (c == '<') {
    literal_.resize(literal_.size() - 1);
    EmitLiteral();
    literal_ += c;
    state_ = TAG;
    tag_start_line_ = line_;
  } else {
    state_ = START;
  }
}

// Browsers appear to only allow letters for first char in tag name,
// plus ? for <?xml version="1.0" encoding="UTF-8"?>
bool HtmlLexer::IsLegalTagFirstChar(char c) {
  return isalpha(c) || (c == '?');
}

// ... and letters, digits, unicode and some symbols for subsequent chars.
// Based on a test of Firefox and Chrome.
//
// TODO(jmarantz): revisit these predicates based on
// http://www.w3.org/TR/REC-xml/#NT-NameChar .  This
// XML spec may or may not inform of us of what we need to do
// to parse all HTML on the web.
bool HtmlLexer::IsLegalTagChar(char c) {
  return (IsI18nChar(c) ||
          (isalnum(c) || (c == '<') || (c == '-') || (c == '#') ||
          (c == '_') || (c == ':')));
}

bool HtmlLexer::IsLegalAttrNameChar(char c) {
  return (IsI18nChar(c) ||
          ((c != '=') && (c != '>') && (c != '/') && !isspace(c)));
}

// Handle the case where "<" was recently parsed.
void HtmlLexer::EvalTag(char c) {
  if (c == '/') {
    state_ = TAG_CLOSE;
  } else if (IsLegalTagFirstChar(c)) {   // "<x"
    state_ = TAG_OPEN;
    token_ += c;
  } else if (c == '!') {
    state_ = COMMENT_START1;
  } else {
    //  Illegal tag syntax; just pass it through as raw characters
    SyntaxError("Invalid tag syntax: unexpected sequence `<%c'", c);
    EvalStart(c);
  }
}

// Handle the case where "<x" was recently parsed.  We will stay in this
// state as long as we keep seeing legal tag characters, appending to
// token_ for each character.
void HtmlLexer::EvalTagOpen(char c) {
  if (IsLegalTagChar(c)) {
    token_ += c;
  } else if (c == '>') {
    MakeElement();
    EmitTagOpen(true);
  } else if (c == '/') {
    state_ = TAG_BRIEF_CLOSE;
  } else if (isspace(c)) {
    state_ = TAG_ATTRIBUTE;
  } else {
    // Some other punctuation.  Not sure what to do.  Let's run this
    // on the web and see what breaks & decide what to do.  E.g. "<x&"
    SyntaxError("Invalid character `%c` while parsing tag `%s'",
                c, token_.c_str());
    token_.clear();
    state_ = START;
  }
}

// Handle several cases of seeing "/" in the middle of a tag, but after
// the identifier has been completed.  Examples: "<x /" or "<x y/" or "x y=/z".
void HtmlLexer::EvalTagBriefCloseAttr(char c) {
  if (c == '>') {
    FinishAttribute(c, has_attr_value_, true);
  } else if (isspace(c)) {
    // "<x y/ ".  This can lead to "<x y/ z" where z would be
    // a new attribute, or "<x y/ >" where the tag would be
    // closed without adding a new attribute.  In either case,
    // we will be completing this attribute.
    //
    // TODO(jmarantz): what about "<x y/ =z>"?  I am not sure
    // sure if this matters, because testing that would require
    // a browser that could react to a named attribute with a
    // slash in the name (not the value).  But should we wind
    // up with 1 attributes or 2 for this case?  There are probably
    // more important questions, but if we ever need to answer that
    // one, this is the place.
    if (!attr_name_.empty()) {
      if (has_attr_value_) {
        // The "/" should be interpreted as the last character in
        // the attribute, so we must tack it on before making it.
        attr_value_ += '/';
      }
      MakeAttribute(has_attr_value_);
    }
  } else {
    // Slurped www.google.com has
    //   <a href=/advanced_search?hl=en>Advanced Search</a>
    // So when we first see the "/" it looks like it might
    // be a brief-close, .e.g. <a href=/>.  But when we see
    // that what follows the '/' is not '>' then we know it's
    // just part off the attribute name or value.  So there's
    // no need to even warn.
    if (has_attr_value_) {
      attr_value_ += '/';
      state_ = TAG_ATTR_VAL;
      EvalAttrVal(c);
      // we know it's not the double-quoted or single-quoted versions
      // because then we wouldn't have let the '/' get us into the
      // brief-close state.
    } else {
      attr_name_ += '/';
      state_ = TAG_ATTR_NAME;
      EvalAttrName(c);
    }
  }
}

// Handle the case where "<x/" was recently parsed, where "x" can
// be any length tag identifier.  Note that we if we anything other
// than a ">" after this, we will just consider the "/" to be part
// of the tag identifier, and go back to the TAG_OPEN state.
void HtmlLexer::EvalTagBriefClose(char c) {
  if (c == '>') {
    MakeElement();
    EmitTagOpen(false);
    EmitTagBriefClose();
  } else {
    GoogleString expected(literal_.data(), literal_.size() - 1);
    SyntaxError("Invalid close tag syntax: expected %s>, got %s",
                expected.c_str(), literal_.c_str());
    // Recover by returning to the mode from whence we came.
    if (element_ != NULL) {
      token_ += '/';
      state_ = TAG_OPEN;
      EvalTagOpen(c);
    } else {
      // E.g. "<R/A", see testdata/invalid_brief.html.
      state_ = START;
      token_.clear();
    }
  }
}

// Handle the case where "</" was recently parsed.  This function
// is also called for "</a ", in which case state will be TAG_CLOSE_TERMINATE.
// We distinguish that case to report an error on "</a b>".
void HtmlLexer::EvalTagClose(char c) {
  if ((state_ != TAG_CLOSE_TERMINATE) && IsLegalTagChar(c)) {  // "</x"
    token_ += c;
  } else if (isspace(c)) {
    if (token_.empty()) {  // e.g. "</ a>"
      // just ignore the whitespace.  Wait for
      // the tag-name to begin.
    } else {
      // "</a ".  Now we are in a state where we can only
      // accept more whitespace or a close.
      state_ = TAG_CLOSE_TERMINATE;
    }
  } else if (c == '>') {
    EmitTagClose(HtmlElement::EXPLICIT_CLOSE);
  } else {
    SyntaxError("Invalid tag syntax: expected `>' after `</%s' got `%c'",
                token_.c_str(), c);
    token_.clear();
    EvalStart(c);
  }
}

// Handle the case where "<!x" was recently parsed, where x
// is any illegal tag identifier.  We stay in this state until
// we see the ">", accumulating the directive in token_.
void HtmlLexer::EvalDirective(char c) {
  if (c == '>') {
    EmitDirective();
  } else {
    token_ += c;
  }
}

// After a partial match of a multi-character lexical sequence, a mismatched
// character needs to temporarily removed from the retained literal_ before
// being emitted.  Then re-inserted for so that EvalStart can attempt to
// re-evaluate this character as potentialy starting a new lexical token.
void HtmlLexer::Restart(char c) {
  CHECK_LE(1U, literal_.size());
  CHECK_EQ(c, literal_[literal_.size() - 1]);
  literal_.resize(literal_.size() - 1);
  EmitLiteral();
  literal_ += c;
  EvalStart(c);
}

// Handle the case where "<!" was recently parsed.
void HtmlLexer::EvalCommentStart1(char c) {
  if (c == '-') {
    state_ = COMMENT_START2;
  } else if (c == '[') {
    state_ = CDATA_START1;
  } else if (IsLegalTagChar(c) && (c != '<')) {  // "<!DOCTYPE ... >"
    state_ = DIRECTIVE;
    EvalDirective(c);
  } else {
    SyntaxError("Invalid comment syntax");
    Restart(c);
  }
}

// Handle the case where "<!-" was recently parsed.
void HtmlLexer::EvalCommentStart2(char c) {
  if (c == '-') {
    state_ = COMMENT_BODY;
  } else {
    SyntaxError("Invalid comment syntax");
    Restart(c);
  }
}

// Handle the case where "<!--" was recently parsed.  We will stay in
// this state until we see "-".  And even after that we may go back to
// this state if the "-" is not followed by "->".
void HtmlLexer::EvalCommentBody(char c) {
  if (c == '-') {
    state_ = COMMENT_END1;
  } else {
    token_ += c;
  }
}

// Handle the case where "-" has been parsed from a comment.  If we
// see another "-" then we go to CommentEnd2, otherwise we go back
// to the comment state.
void HtmlLexer::EvalCommentEnd1(char c) {
  if (c == '-') {
    state_ = COMMENT_END2;
  } else {
    // thought we were ending a comment cause we saw '-', but
    // now we changed our minds.   No worries mate.  That
    // fake-out dash was just part of the comment.
    token_ += '-';
    token_ += c;
    state_ = COMMENT_BODY;
  }
}

// Handle the case where "--" has been parsed from a comment.
void HtmlLexer::EvalCommentEnd2(char c) {
  if (c == '>') {
    EmitComment();
    state_ = START;
  } else if (c == '-') {
    // There could be an arbitrarily long stream of dashes before
    // we see the >.  Keep looking.
    token_ += "-";
  } else {
    // thought we were ending a comment cause we saw '--', but
    // now we changed our minds.   No worries mate.  Those
    // fake-out dashes were just part of the comment.
    token_ += "--";
    token_ += c;
    state_ = COMMENT_BODY;
  }
}

// Handle the case where "<![" was recently parsed.
void HtmlLexer::EvalCdataStart1(char c) {
  // TODO(mdsteele): What about IE downlevel-revealed conditional comments?
  //   Those look like e.g. <![if foo]> and <![endif]>.  This will treat those
  //   as syntax errors and emit them verbatim (which is usually harmless), but
  //   ideally we'd identify them as HtmlIEDirectiveEvents.
  //   See http://msdn.microsoft.com/en-us/library/ms537512(VS.85).aspx
  if (c == 'C') {
    state_ = CDATA_START2;
  } else {
    SyntaxError("Invalid CDATA syntax");
    Restart(c);
  }
}

// Handle the case where "<![C" was recently parsed.
void HtmlLexer::EvalCdataStart2(char c) {
  if (c == 'D') {
    state_ = CDATA_START3;
  } else {
    SyntaxError("Invalid CDATA syntax");
    Restart(c);
  }
}

// Handle the case where "<![CD" was recently parsed.
void HtmlLexer::EvalCdataStart3(char c) {
  if (c == 'A') {
    state_ = CDATA_START4;
  } else {
    SyntaxError("Invalid CDATA syntax");
    Restart(c);
  }
}

// Handle the case where "<![CDA" was recently parsed.
void HtmlLexer::EvalCdataStart4(char c) {
  if (c == 'T') {
    state_ = CDATA_START5;
  } else {
    SyntaxError("Invalid CDATA syntax");
    Restart(c);
  }
}

// Handle the case where "<![CDAT" was recently parsed.
void HtmlLexer::EvalCdataStart5(char c) {
  if (c == 'A') {
    state_ = CDATA_START6;
  } else {
    SyntaxError("Invalid CDATA syntax");
    Restart(c);
  }
}

// Handle the case where "<![CDATA" was recently parsed.
void HtmlLexer::EvalCdataStart6(char c) {
  if (c == '[') {
    state_ = CDATA_BODY;
  } else {
    SyntaxError("Invalid CDATA syntax");
    Restart(c);
  }
}

// Handle the case where "<![CDATA[" was recently parsed.  We will stay in
// this state until we see "]".  And even after that we may go back to
// this state if the "]" is not followed by "]>".
void HtmlLexer::EvalCdataBody(char c) {
  if (c == ']') {
    state_ = CDATA_END1;
  } else {
    token_ += c;
  }
}

// Handle the case where "]" has been parsed from a cdata.  If we
// see another "]" then we go to CdataEnd2, otherwise we go back
// to the cdata state.
void HtmlLexer::EvalCdataEnd1(char c) {
  if (c == ']') {
    state_ = CDATA_END2;
  } else {
    // thought we were ending a cdata cause we saw ']', but
    // now we changed our minds.   No worries mate.  That
    // fake-out bracket was just part of the cdata.
    token_ += ']';
    token_ += c;
    state_ = CDATA_BODY;
  }
}

// Handle the case where "]]" has been parsed from a cdata.
void HtmlLexer::EvalCdataEnd2(char c) {
  if (c == '>') {
    EmitCdata();
    state_ = START;
  } else {
    // thought we were ending a cdata cause we saw ']]', but
    // now we changed our minds.   No worries mate.  Those
    // fake-out brackets were just part of the cdata.
    token_ += "]]";
    token_ += c;
    state_ = CDATA_BODY;
  }
}

// Handle the case where a literal tag (script, iframe) was started.
// This is of lexical significance because we ignore all the special
// characters until we see "</script>" or "</iframe>".
void HtmlLexer::EvalLiteralTag(char c) {
  // Look explicitly for </script> in the literal buffer.
  // TODO(jmarantz): check for whitespace in unexpected places.
  if (c == '>') {
    // expecting "</x>" for tag x.
    html_parse_->message_handler()->Check(
        literal_close_.size() > 3, "literal_close_.size() <= 3");  // NOLINT
    int literal_minus_close_size = literal_.size() - literal_close_.size();
    if ((literal_minus_close_size >= 0) &&
        StringCaseEqual(literal_.c_str() + literal_minus_close_size,
                        literal_close_)) {
      // The literal actually starts after the "<script>", and we will
      // also let it finish before, so chop it off.
      literal_.resize(literal_minus_close_size);
      EmitLiteral();
      token_.clear();
      // Transform "</script>" into "script" to form close tag.
      token_.append(literal_close_.c_str() + 2, literal_close_.size() - 3);
      EmitTagClose(HtmlElement::EXPLICIT_CLOSE);
    }
  }
}

// Emits raw uninterpreted characters.
void HtmlLexer::EmitLiteral() {
  if (!literal_.empty()) {
    html_parse_->AddEvent(new HtmlCharactersEvent(
        html_parse_->NewCharactersNode(Parent(), literal_), tag_start_line_));
    literal_.clear();
  }
  state_ = START;
}

void HtmlLexer::EmitComment() {
  literal_.clear();
  // The precise syntax of IE conditional comments (for example, exactly where
  // is whitespace tolerated?) doesn't seem to be specified anywhere, but my
  // brief experiments suggest that this heuristic is okay.  (mdsteele)
  // See http://en.wikipedia.org/wiki/Conditional_comment
  if ((token_.find("[if") != GoogleString::npos) ||
      (token_.find("[endif]") != GoogleString::npos)) {
    HtmlIEDirectiveNode* node =
        html_parse_->NewIEDirectiveNode(Parent(), token_);
    html_parse_->AddEvent(new HtmlIEDirectiveEvent(node, tag_start_line_));
  } else {
    HtmlCommentNode* node = html_parse_->NewCommentNode(Parent(), token_);
    html_parse_->AddEvent(new HtmlCommentEvent(node, tag_start_line_));
  }
  token_.clear();
  state_ = START;
}

void HtmlLexer::EmitCdata() {
  literal_.clear();
  html_parse_->AddEvent(new HtmlCdataEvent(
      html_parse_->NewCdataNode(Parent(), token_), tag_start_line_));
  token_.clear();
  state_ = START;
}

// If allow_implicit_close is true, and the element type is one which
// does not require an explicit termination in HTML, then we will
// automatically emit a matching 'element close' event.
void HtmlLexer::EmitTagOpen(bool allow_implicit_close) {
  DCHECK(element_ != NULL);
  DCHECK(token_.empty());
  HtmlName next_tag = element_->name();

  // Look for elements that are implicitly closed by an open for this type.
  HtmlName::Keyword next_keyword = next_tag.keyword();

  // Continue popping off auto-close elements as needed to handle cases like
  // IClosedByOpenTr in html_parse_test.cc: "<tr><i>a<tr>b".  The first the <i>
  // needs to be auto-closed, then the <tr>.
  for (HtmlElement* open_element = Parent(); open_element != NULL; ) {
    // TODO(jmarantz): this is a hack -- we should make a more elegant
    // structure of open/new tag combinations that we should auto-close.
    HtmlName::Keyword open_keyword = open_element->keyword();
    if (HtmlKeywords::IsAutoClose(open_keyword, next_keyword)) {
      element_stack_.pop_back();
      html_parse_->CloseElement(open_element, HtmlElement::AUTO_CLOSE, line_);

      // Having automatically closed the element that was open on the stack,
      // we must recompute the open element from whatever is now on top of
      // the stack.  We must also correct the current element's parent to
      // maintain DOM consistency with the event stream.
      DCHECK_EQ(element_->parent(), open_element);
      open_element = Parent();
      element_->set_parent(open_element);
    } else {
      break;
    }
  }

  literal_.clear();
  html_parse_->AddElement(element_, tag_start_line_);
  element_stack_.push_back(element_);
  if (IS_IN_SET(kLiteralTags, element_->keyword())) {
    state_ = LITERAL_TAG;
    literal_close_ = "</";
    literal_close_ += element_->name_str();
    literal_close_ += ">";
  } else {
    state_ = START;
  }

  if (allow_implicit_close && IsImplicitlyClosedTag(element_->keyword())) {
    token_ = element_->name_str();
    EmitTagClose(HtmlElement::IMPLICIT_CLOSE);
  }

  element_ = NULL;
}

void HtmlLexer::EmitTagBriefClose() {
  HtmlElement* element = PopElement();
  html_parse_->CloseElement(element, HtmlElement::BRIEF_CLOSE, line_);
  state_ = START;
}

HtmlElement* HtmlLexer::Parent() const {
  html_parse_->message_handler()->Check(!element_stack_.empty(),
                                        "element_stack_.empty()");
  return element_stack_.back();
}

void HtmlLexer::MakeElement() {
  if (element_ == NULL) {
    if (token_.empty()) {
      SyntaxError("Making element with empty tag name");
    }
    element_ = html_parse_->NewElement(Parent(), token_);
    element_->set_begin_line_number(tag_start_line_);
    token_.clear();
  }
}

void HtmlLexer::StartParse(const StringPiece& id,
                           const ContentType& content_type) {
  line_ = 1;
  tag_start_line_ = -1;
  id.CopyToString(&id_);
  content_type_ = content_type;
  has_attr_value_ = false;
  attr_quote_ = "";
  state_ = START;
  element_stack_.clear();
  element_stack_.push_back(static_cast<HtmlElement*>(0));
  element_ = NULL;
  token_.clear();
  attr_name_.clear();
  attr_value_.clear();
  literal_.clear();
  missing_close_tag_bag_.clear();
  // clear buffers
}

void HtmlLexer::FinishParse() {
  if (!token_.empty()) {
    SyntaxError("End-of-file in mid-token: %s", token_.c_str());
    token_.clear();
  }
  if (!attr_name_.empty()) {
    SyntaxError("End-of-file in mid-attribute-name: %s", attr_name_.c_str());
    attr_name_.clear();
  }
  if (!attr_value_.empty()) {
    SyntaxError("End-of-file in mid-attribute-value: %s", attr_value_.c_str());
    attr_value_.clear();
  }

  if (!literal_.empty()) {
    EmitLiteral();
  }

  // Any unclosed tags?  These should be noted.
  html_parse_->message_handler()->Check(!element_stack_.empty(),
                                        "element_stack_.empty()");
  html_parse_->message_handler()->Check(element_stack_[0] == NULL,
                                        "element_stack_[0] != NULL");

  for (int i = element_stack_.size() - 1; i > 0; --i) {
    HtmlElement* element = element_stack_.back();
    token_ = element->name_str();
    EmitTagClose(HtmlElement::UNCLOSED);
    if (!HtmlKeywords::IsOptionallyClosedTag(element->keyword())) {
      html_parse_->Info(id_.c_str(), element->begin_line_number(),
                        "End-of-file with open tag: %s", element->name_str());
    }
  }
  DCHECK_EQ(1U, element_stack_.size());
  DCHECK_EQ(static_cast<HtmlElement*>(0), element_stack_[0]);
  element_ = NULL;
}

void HtmlLexer::MakeAttribute(bool has_value) {
  html_parse_->message_handler()->Check(element_ != NULL, "element_ == NULL");
  HtmlName name = html_parse_->MakeName(attr_name_);
  attr_name_.clear();
  const char* value = NULL;
  html_parse_->message_handler()->Check(has_value == has_attr_value_,
                                        "has_value != has_attr_value_");
  if (has_value) {
    value = attr_value_.c_str();
    has_attr_value_ = false;
  } else {
    html_parse_->message_handler()->Check(attr_value_.empty(),
                                          "!attr_value_.empty()");
  }
  element_->AddEscapedAttribute(name, value, attr_quote_);
  attr_value_.clear();
  attr_quote_ = "";
  state_ = TAG_ATTRIBUTE;
}

void HtmlLexer::EvalAttribute(char c) {
  MakeElement();
  attr_name_.clear();
  attr_value_.clear();
  if (c == '>') {
    EmitTagOpen(true);
  } else if (c == '/') {
    state_ = TAG_BRIEF_CLOSE_ATTR;
  } else if (IsLegalAttrNameChar(c)) {
    attr_name_ += c;
    state_ = TAG_ATTR_NAME;
  } else if (!isspace(c)) {
    SyntaxError("Unexpected char `%c' in attribute list", c);
  }
}

// "<x y" or  "<x y ".
void HtmlLexer::EvalAttrName(char c) {
  if (c == '=') {
    state_ = TAG_ATTR_EQ;
    has_attr_value_ = true;
  } else if (IsLegalAttrNameChar(c) && (state_ != TAG_ATTR_NAME_SPACE)) {
    attr_name_ += c;
  } else if (isspace(c)) {
    state_ = TAG_ATTR_NAME_SPACE;
  } else if (c == '>') {
    MakeAttribute(false);
    EmitTagOpen(true);
  } else if (state_ == TAG_ATTR_NAME_SPACE) {
    // "<x y z".  Now that we see the 'z', we need
    // to finish 'y' as an attribute, then queue up
    // 'z' (c) as the start of a new attribute.
    MakeAttribute(false);
    state_ = TAG_ATTR_NAME;
    attr_name_ += c;
  } else {
    FinishAttribute(c, false, false);
  }
}

void HtmlLexer::FinishAttribute(char c, bool has_value, bool brief_close) {
  if (isspace(c)) {
    MakeAttribute(has_value);
    state_ = TAG_ATTRIBUTE;
  } else if (c == '/') {
    // If / was seen terminating an attribute, without
    // the closing quote or whitespace, it might just be
    // part of a syntactically dubious attribute.  We'll
    // hold off completing the attribute till we see the
    // next character.
    state_ = TAG_BRIEF_CLOSE_ATTR;
  } else if (c == '>') {
    if (!attr_name_.empty()) {
      if (!brief_close &&
          (strcmp(attr_name_.c_str(), "/") == 0) && !has_value) {
        brief_close = true;
        attr_name_.clear();
        attr_value_.clear();
      } else {
        MakeAttribute(has_value);
      }
    }
    EmitTagOpen(!brief_close);
    if (brief_close) {
      EmitTagBriefClose();
    }

    has_attr_value_ = false;
  } else {
    // Some other funny character within a tag.  Probably can't
    // trust the tag at all.  Check the web and see when this
    // happens.
    SyntaxError("Unexpected character in attribute: %c", c);
    MakeAttribute(has_value);
    has_attr_value_ = false;
  }
}

void HtmlLexer::EvalAttrEq(char c) {
  if (c == '"') {
    attr_quote_ = "\"";
    state_ = TAG_ATTR_VALDQ;
  } else if (c == '\'') {
    attr_quote_ = "'";
    state_ = TAG_ATTR_VALSQ;
  } else if (isspace(c)) {
    // ignore -- spaces are allowed between "=" and the value
  } else if (c == '>') {
    FinishAttribute(c, true, false);
  } else {
    state_ = TAG_ATTR_VAL;
    attr_quote_ = "";
    EvalAttrVal(c);
  }
}

void HtmlLexer::EvalAttrVal(char c) {
  if (isspace(c) || (c == '>')) {
    FinishAttribute(c, true, false);
  } else {
    attr_value_ += c;
  }
}

void HtmlLexer::EvalAttrValDq(char c) {
  if (c == '"') {
    MakeAttribute(true);
  } else {
    attr_value_ += c;
  }
}

void HtmlLexer::EvalAttrValSq(char c) {
  if (c == '\'') {
    MakeAttribute(true);
  } else {
    attr_value_ += c;
  }
}

void HtmlLexer::EmitTagClose(HtmlElement::CloseStyle close_style) {
  TagBag::iterator p = missing_close_tag_bag_.find(token_);
  bool emit_fake_close_as_a_characters_literal = false;
  if (p != missing_close_tag_bag_.end()) {
    int implicit_closes_for_this_tag = p->second - 1;
    if (implicit_closes_for_this_tag == 0) {
      missing_close_tag_bag_.erase(p);
    } else {
      p->second = implicit_closes_for_this_tag;
    }
    emit_fake_close_as_a_characters_literal = true;

    SyntaxError("Close-tag `%s', appears to be misplaced", token_.c_str());
  } else {
    HtmlElement* element = PopElementMatchingTag(token_);
    if (element != NULL) {
      DCHECK(StringCaseEqual(token_, element->name_str()));
      element->set_end_line_number(line_);
      html_parse_->CloseElement(element, close_style, line_);
    } else {
      SyntaxError("Unexpected close-tag `%s', no tags are open",
                  token_.c_str());
      emit_fake_close_as_a_characters_literal = true;
    }
  }

  if (emit_fake_close_as_a_characters_literal) {
    // Structurally the close-tag we just parsed is not open.  This
    // might happen because the HTML structure constraint forced this
    // tag to be closed already, but now we finally see a literal
    // close.  Note that the earlier close will be structural in the
    // API, but invisible because it will be an AUTO_CLOSE.  Now that
    // we see the *real* close, we don't want to eat it because we
    // want to be byte-accurate to the input.  So we emit the "</tag>"
    // as a Characters literal.
    EmitLiteral();
  }

  literal_.clear();
  token_.clear();
  state_ = START;
}

void HtmlLexer::EmitDirective() {
  literal_.clear();
  html_parse_->AddEvent(new HtmlDirectiveEvent(
      html_parse_->NewDirectiveNode(Parent(), token_), line_));
  // Update the doctype; note that if this is not a doctype directive, Parse()
  // will return false and not alter doctype_.
  doctype_.Parse(token_, content_type_);
  token_.clear();
  state_ = START;
}

void HtmlLexer::Parse(const char* text, int size) {
  for (int i = 0; i < size; ++i) {
    char c = text[i];
    if (c == '\n') {
      ++line_;
    }

    // By default we keep track of every byte as it comes in.
    // If we can't accurately parse it, we transmit it as
    // raw characters to be re-serialized without interpretation,
    // and good luck to the browser.  When we do successfully
    // parse something, we remove it from the literal.
    literal_ += c;

    switch (state_) {
      case START:                 EvalStart(c);               break;
      case TAG:                   EvalTag(c);                 break;
      case TAG_OPEN:              EvalTagOpen(c);             break;
      case TAG_CLOSE:             EvalTagClose(c);            break;
      case TAG_CLOSE_TERMINATE:   EvalTagClose(c);            break;
      case TAG_BRIEF_CLOSE:       EvalTagBriefClose(c);       break;
      case TAG_BRIEF_CLOSE_ATTR:  EvalTagBriefCloseAttr(c);   break;
      case COMMENT_START1:        EvalCommentStart1(c);       break;
      case COMMENT_START2:        EvalCommentStart2(c);       break;
      case COMMENT_BODY:          EvalCommentBody(c);         break;
      case COMMENT_END1:          EvalCommentEnd1(c);         break;
      case COMMENT_END2:          EvalCommentEnd2(c);         break;
      case CDATA_START1:          EvalCdataStart1(c);         break;
      case CDATA_START2:          EvalCdataStart2(c);         break;
      case CDATA_START3:          EvalCdataStart3(c);         break;
      case CDATA_START4:          EvalCdataStart4(c);         break;
      case CDATA_START5:          EvalCdataStart5(c);         break;
      case CDATA_START6:          EvalCdataStart6(c);         break;
      case CDATA_BODY:            EvalCdataBody(c);           break;
      case CDATA_END1:            EvalCdataEnd1(c);           break;
      case CDATA_END2:            EvalCdataEnd2(c);           break;
      case TAG_ATTRIBUTE:         EvalAttribute(c);           break;
      case TAG_ATTR_NAME:         EvalAttrName(c);            break;
      case TAG_ATTR_NAME_SPACE:   EvalAttrName(c);            break;
      case TAG_ATTR_EQ:           EvalAttrEq(c);              break;
      case TAG_ATTR_VAL:          EvalAttrVal(c);             break;
      case TAG_ATTR_VALDQ:        EvalAttrValDq(c);           break;
      case TAG_ATTR_VALSQ:        EvalAttrValSq(c);           break;
      case LITERAL_TAG:           EvalLiteralTag(c);          break;
      case DIRECTIVE:             EvalDirective(c);           break;
    }
  }
}

// The HTML-input sloppiness in these three methods is applied independent
// of whether we think the document is XHTML, either via doctype or
// mime-type.  The internet is full of lies.  See Issue 252:
//   http://code.google.com/p/modpagespeed/issues/detail?id=252

bool HtmlLexer::IsImplicitlyClosedTag(HtmlName::Keyword keyword) const {
  return IS_IN_SET(kImplicitlyClosedHtmlTags, keyword);
}

bool HtmlLexer::TagAllowsBriefTermination(HtmlName::Keyword keyword) const {
  return (!IS_IN_SET(kNonBriefTerminatedTags, keyword) &&
          !IsImplicitlyClosedTag(keyword));
}

bool HtmlLexer::IsOptionallyClosedTag(HtmlName::Keyword keyword) const {
  return HtmlKeywords::IsOptionallyClosedTag(keyword);
}

void HtmlLexer::DebugPrintStack() {
  for (size_t i = kStartStack; i < element_stack_.size(); ++i) {
    GoogleString buf;
    element_stack_[i]->ToString(&buf);
    fprintf(stdout, "%s\n", buf.c_str());
  }
  fflush(stdout);
}

HtmlElement* HtmlLexer::PopElement() {
  HtmlElement* element = NULL;
  if (!element_stack_.empty()) {
    element = element_stack_.back();
    element_stack_.pop_back();
  }
  return element;
}

HtmlElement* HtmlLexer::PopElementMatchingTag(const StringPiece& tag) {
  HtmlElement* element = NULL;

  HtmlName::Keyword keyword = HtmlName::Lookup(tag);
  int close_index = element_stack_.size();

  // Search the stack from top to bottom.
  for (int i = element_stack_.size() - 1; i >= kStartStack; --i) {
    element = element_stack_[i];

    if (StringCaseEqual(element->name_str(), tag)) {
      // In tag-matching we will do case-insensitive comparisons, despite
      // the fact that we have a keywords enum.  Note that the symbol
      // table is case sensitive.
      close_index = i;
      break;
    } else if (HtmlKeywords::IsContained(keyword, element->keyword())) {
      // Stop when we get to an 'owner' of this element.  Consider
      // <tr><table></tr></table>.  When hitting the </tr> we start
      // looking for a matching <tr> to close.  We need to stop when
      // we get an IsContained match (e.g. tr,table).  But at at this
      // point the appropriate response is to give up -- there is no
      // matching open-tag for the </tr> inside the <table>.  See
      // HtmlAnnotationTest.StrayCloseTrInTable in html_parse_test.cc.
      missing_close_tag_bag_.clear();
      return NULL;
    }
  }

  if (close_index == static_cast<int>(element_stack_.size())) {
    element = NULL;
  } else {
    element = element_stack_[close_index];

    // Emit warnings for the tags we are skipping.  We have to do
    // this in reverse order so that we maintain stack discipline.
    //
    // Note that the element at close_index does not get closed here,
    // but gets returned and closed at the call-site.
    for (int j = element_stack_.size() - 1; j > close_index; --j) {
      HtmlElement* skipped = element_stack_[j];
      // In fact, should we actually perform this optimization ourselves
      // in a filter to omit closing tags that can be inferred?
      if (!HtmlKeywords::IsOptionallyClosedTag(skipped->keyword())) {
        html_parse_->Info(id_.c_str(), skipped->begin_line_number(),
                          "Unclosed element `%s'", skipped->name_str());
        ++missing_close_tag_bag_[skipped->name_str()];
      }
      // Before closing the skipped element, pop it off the stack.  Otherwise,
      // the parent redundancy check in HtmlParse::AddEvent will fail.
      element_stack_.resize(j);
      html_parse_->CloseElement(skipped, HtmlElement::UNCLOSED, line_);
    }
    element_stack_.resize(close_index);
  }
  return element;
}

void HtmlLexer::SyntaxError(const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  html_parse_->InfoV(id_.c_str(), line_, msg, args);
  va_end(args);
}

}  // namespace net_instaweb
