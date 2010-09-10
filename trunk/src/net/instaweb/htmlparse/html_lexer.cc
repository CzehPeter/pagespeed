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

#include "net/instaweb/htmlparse/html_lexer.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "net/instaweb/htmlparse/html_event.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/util/public/string_util.h"

namespace {
// These tags can be specified in documents without a brief "/>",
// or an explicit </tag>, according to the Chrome Developer Tools console.
//
// TODO(jmarantz): Check out
// http://www.whatwg.org/specs/web-apps/current-work/multipage/
// syntax.html#optional-tags
//
// TODO(jmarantz): examine doctype for xhtml for implicitly closed tags
const char* kImplicitlyClosedHtmlTags[] = {
  "meta", "input", "link", "br", "img", "area", "hr", "wbr", "param",
  NULL
};

// These tags cannot be closed using the brief syntax; they must
// be closed by using an explicit </TAG>.
const char* kNonBriefTerminatedTags[] = {
  "script", "a", "div", "span", "iframe", "style", "textarea",
  NULL
};

// These tags cause the text inside them to retained literally
// and not interpreted.
const char* kLiteralTags[] = {
  "script", "iframe", "textarea", "style",
  NULL
};

// We start our stack-iterations from 1, because we put a NULL into
// position 0 to reduce special-cases.
const int kStartStack = 1;

}  // namespace

// TODO(jmarantz): support multi-byte encodings
// TODO(jmarantz): emit close-tags immediately for selected html tags,
//   rather than waiting for the next explicit close-tag to force a rebalance.
//   See http://www.whatwg.org/specs/web-apps/current-work/multipage/
//   syntax.html#optional-tags

namespace net_instaweb {

HtmlLexer::HtmlLexer(HtmlParse* html_parse)
    : html_parse_(html_parse),
      state_(START),
      attr_quote_(""),
      has_attr_value_(false),
      element_(NULL),
      line_(1),
      tag_start_line_(-1) {
  for (const char** p = kImplicitlyClosedHtmlTags; *p != NULL; ++p) {
    implicitly_closed_.insert(html_parse->Intern(*p));
  }
  for (const char** p = kNonBriefTerminatedTags; *p != NULL; ++p) {
    non_brief_terminated_tags_.insert(html_parse->Intern(*p));
  }
  for (const char** p = kLiteralTags; *p != NULL; ++p) {
    literal_tags_.insert(html_parse->Intern(*p));
  }
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

// TODO(jmarantz): revisit these predicates based on
// http://www.w3.org/TR/REC-xml/#NT-NameChar .  This
// XML spec may or may not inform of us of what we need to do
// to parse all HTML on the web.
bool HtmlLexer::IsLegalTagChar(char c) {
  return (IsI18nChar(c) ||
          (isalnum(c) || (c == '-') || (c == '#') || (c == '_') || (c == ':')));
}

bool HtmlLexer::IsLegalAttrNameChar(char c) {
  return (IsI18nChar(c) ||
          ((c != '=') && (c != '>') && (c != '/') && (c != '<') &&
           !isspace(c)));
}

bool HtmlLexer::IsLegalAttrValChar(char c) {
  return (IsI18nChar(c) ||
          ((c != '=') && (c != '>') && (c != '/') && (c != '<') &&
           (c != '"') && (c != '\'') && !isspace(c)));
}

// Handle the case where "<" was recently parsed.
void HtmlLexer::EvalTag(char c) {
  if (c == '/') {
    state_ = TAG_CLOSE;
  } else if (IsLegalTagChar(c)) {   // "<x"
    state_ = TAG_OPEN;
    token_ += c;
  } else if (c == '!') {
    state_ = COMMENT_START1;
  } else {
    //  Illegal tag syntax; just pass it through as raw characters
    Warning("Invalid tag syntax: unexpected sequence `<%c'", c);
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
    EmitTagOpen(true);
  } else if (c == '<') {
    // Chrome transforms "<tag<tag>" into "<tag><tag>";
    Warning("Invalid tag syntax: expected close tag before opener");
    EmitTagOpen(true);
    literal_ = "<";  // will be removed by EvalStart.
    EvalStart(c);
  } else if (c == '/') {
    state_ = TAG_BRIEF_CLOSE;
  } else if (isspace(c)) {
    state_ = TAG_ATTRIBUTE;
  } else {
    // Some other punctuation.  Not sure what to do.  Let's run this
    // on the web and see what breaks & decide what to do.  E.g. "<x&"
    Warning("Invalid character `%c` while parsing tag `%s'", c, token_.c_str());
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
    EmitTagOpen(false);
    EmitTagBriefClose();
  } else {
    std::string expected(literal_.data(), literal_.size() - 1);
    Warning("Invalid close tag syntax: expected %s>, got %s",
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
      // accept more whitesapce or a close.
      state_ = TAG_CLOSE_TERMINATE;
    }
  } else if (c == '>') {
    EmitTagClose(HtmlElement::EXPLICIT_CLOSE);
  } else {
    Warning("Invalid tag syntax: expected `>' after `</%s' got `%c'",
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

// Handle the case where "<!" was recently parsed.
void HtmlLexer::EvalCommentStart1(char c) {
  if (c == '-') {
    state_ = COMMENT_START2;
  } else if (c == '[') {
    state_ = CDATA_START1;
  } else if (IsLegalTagChar(c)) {  // "<!DOCTYPE ... >"
    state_ = DIRECTIVE;
    EvalDirective(c);
  } else {
    Warning("Invalid comment syntax");
    EmitLiteral();
    EvalStart(c);
  }
}

// Handle the case where "<!-" was recently parsed.
void HtmlLexer::EvalCommentStart2(char c) {
  if (c == '-') {
    state_ = COMMENT_BODY;
  } else {
    Warning("Invalid comment syntax");
    EmitLiteral();
    EvalStart(c);
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
  if (c == 'C') {
    state_ = CDATA_START2;
  } else {
    Warning("Invalid CDATA syntax");
    EmitLiteral();
    EvalStart(c);
  }
}

// Handle the case where "<![C" was recently parsed.
void HtmlLexer::EvalCdataStart2(char c) {
  if (c == 'D') {
    state_ = CDATA_START3;
  } else {
    Warning("Invalid CDATA syntax");
    EmitLiteral();
    EvalStart(c);
  }
}

// Handle the case where "<![CD" was recently parsed.
void HtmlLexer::EvalCdataStart3(char c) {
  if (c == 'A') {
    state_ = CDATA_START4;
  } else {
    Warning("Invalid CDATA syntax");
    EmitLiteral();
    EvalStart(c);
  }
}

// Handle the case where "<![CDA" was recently parsed.
void HtmlLexer::EvalCdataStart4(char c) {
  if (c == 'T') {
    state_ = CDATA_START5;
  } else {
    Warning("Invalid CDATA syntax");
    EmitLiteral();
    EvalStart(c);
  }
}

// Handle the case where "<![CDAT" was recently parsed.
void HtmlLexer::EvalCdataStart5(char c) {
  if (c == 'A') {
    state_ = CDATA_START6;
  } else {
    Warning("Invalid CDATA syntax");
    EmitLiteral();
    EvalStart(c);
  }
}

// Handle the case where "<![CDATA" was recently parsed.
void HtmlLexer::EvalCdataStart6(char c) {
  if (c == '[') {
    state_ = CDATA_BODY;
  } else {
    Warning("Invalid CDATA syntax");
    EmitLiteral();
    EvalStart(c);
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
    CHECK(literal_close_.size() > 3);  // NOLINT
    int literal_minus_close_size = literal_.size() - literal_close_.size();
    if ((literal_minus_close_size >= 0) &&
        (strcasecmp(literal_.c_str() + literal_minus_close_size,
                    literal_close_.c_str()) == 0)) {
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
  if ((token_.find("[if IE") != std::string::npos) &&
      (token_.find("<![endif]") != std::string::npos)) {
    html_parse_->AddEvent(new HtmlIEDirectiveEvent(token_, tag_start_line_));
  } else {
    html_parse_->AddEvent(new HtmlCommentEvent(
        html_parse_->NewCommentNode(Parent(), token_), tag_start_line_));
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
  literal_.clear();
  MakeElement();
  html_parse_->AddElement(element_, tag_start_line_);
  element_stack_.push_back(element_);
  if (literal_tags_.find(element_->tag()) != literal_tags_.end()) {
    state_ = LITERAL_TAG;
    literal_close_ = "</";
    literal_close_ += element_->tag().c_str();
    literal_close_ += ">";
  } else {
    state_ = START;
  }

  Atom tag = element_->tag();
  if (allow_implicit_close && IsImplicitlyClosedTag(tag)) {
    token_ = tag.c_str();
    EmitTagClose(HtmlElement::IMPLICIT_CLOSE);
  }

  element_ = NULL;
}

void HtmlLexer::EmitTagBriefClose() {
  HtmlElement* element = PopElement();
  html_parse_->CloseElement(element, HtmlElement::BRIEF_CLOSE, line_);
  state_ = START;
}

static void toLower(std::string* str) {
  for (int i = 0, n = str->size(); i < n; ++i) {
    char& c = (*str)[i];
    c = tolower(c);
  }
}

HtmlElement* HtmlLexer::Parent() const {
  CHECK(!element_stack_.empty());
  return element_stack_.back();
}

void HtmlLexer::MakeElement() {
  if (element_ == NULL) {
    if (token_.empty()) {
      Warning("Making element with empty tag name");
    }
    toLower(&token_);
    element_ = html_parse_->NewElement(Parent(), html_parse_->Intern(token_));
    element_->set_begin_line_number(tag_start_line_);
    token_.clear();
  }
}

void HtmlLexer::StartParse(const StringPiece& url) {
  line_ = 1;
  tag_start_line_ = -1;
  url.CopyToString(&filename_);
  has_attr_value_ = false;
  attr_quote_ = "";
  state_ = START;
  element_stack_.clear();
  element_stack_.push_back(NULL);
  element_ = NULL;
  token_.clear();
  attr_name_.clear();
  attr_value_.clear();
  literal_.clear();
  // clear buffers
}

void HtmlLexer::FinishParse() {
  if (!token_.empty()) {
    Warning("End-of-file in mid-token: %s", token_.c_str());
    token_.clear();
  }
  if (!attr_name_.empty()) {
    Warning("End-of-file in mid-attribute-name: %s", attr_name_.c_str());
    attr_name_.clear();
  }
  if (!attr_value_.empty()) {
    Warning("End-of-file in mid-attribute-value: %s", attr_value_.c_str());
    attr_value_.clear();
  }

  if (!literal_.empty()) {
    EmitLiteral();
  }

  // Any unclosed tags?  These should be noted.
  CHECK(!element_stack_.empty());
  CHECK(element_stack_[0] == NULL);
  for (size_t i = kStartStack; i < element_stack_.size(); ++i) {
    HtmlElement* element = element_stack_[i];
    html_parse_->Warning(filename_.c_str(), element->begin_line_number(),
                         "End-of-file with open tag: %s",
                         element->tag().c_str());
  }
  element_stack_.clear();
  element_stack_.push_back(NULL);
  element_ = NULL;
}

void HtmlLexer::MakeAttribute(bool has_value) {
  CHECK(element_ != NULL);
  toLower(&attr_name_);
  Atom name = html_parse_->Intern(attr_name_);
  attr_name_.clear();
  const char* value = NULL;
  CHECK(has_value == has_attr_value_);
  if (has_value) {
    value = attr_value_.c_str();
    has_attr_value_ = false;
  } else {
    CHECK(attr_value_.empty());
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
  } else if (c == '<') {
    FinishAttribute(c, false, false);
  } else if (c == '/') {
    state_ = TAG_BRIEF_CLOSE_ATTR;
  } else if (IsLegalAttrNameChar(c)) {
    attr_name_ += c;
    state_ = TAG_ATTR_NAME;
  } else if (!isspace(c)) {
    Warning("Unexpected char `%c' in attribute list", c);
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
  } else {
    if (state_ == TAG_ATTR_NAME_SPACE) {
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
  } else if ((c == '<') || (c == '>')) {
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

    if (c == '<') {
      // Chrome transforms "<tag a<tag>" into "<tag a><tag>"; we should too.
      Warning("Invalid tag syntax: expected close tag before opener");
      literal_ += '<';
      EvalStart(c);
    }
    has_attr_value_ = false;
  } else {
    // Some other funny character within a tag.  Probably can't
    // trust the tag at all.  Check the web and see when this
    // happens.
    Warning("Unexpected character in attribute: %c", c);
    MakeAttribute(has_value);
    has_attr_value_ = false;
  }
}

void HtmlLexer::EvalAttrEq(char c) {
  if (IsLegalAttrValChar(c)) {
    state_ = TAG_ATTR_VAL;
    attr_quote_ = "";
    EvalAttrVal(c);
  } else if (c == '"') {
    attr_quote_ = "\"";
    state_ = TAG_ATTR_VALDQ;
  } else if (c == '\'') {
    attr_quote_ = "'";
    state_ = TAG_ATTR_VALSQ;
  } else if (isspace(c)) {
    // ignore -- spaces are allowed between "=" and the value
  } else {
    FinishAttribute(c, true, false);
  }
}

void HtmlLexer::EvalAttrVal(char c) {
  if (isspace(c) || (c == '>') || (c == '<')) {
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
  toLower(&token_);
  Atom tag = html_parse_->Intern(token_);
  HtmlElement* element = PopElementMatchingTag(tag);
  if (element != NULL) {
    element->set_end_line_number(line_);
    html_parse_->CloseElement(element, close_style, line_);
  } else {
    Warning("Unexpected close-tag `%s', no tags are open", token_.c_str());
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

bool HtmlLexer::IsImplicitlyClosedTag(Atom tag) const {
  return (implicitly_closed_.find(tag) != implicitly_closed_.end());
}

bool HtmlLexer::TagAllowsBriefTermination(Atom tag) const {
  return (non_brief_terminated_tags_.find(tag) ==
          non_brief_terminated_tags_.end());
}

void HtmlLexer::DebugPrintStack() {
  for (size_t i = kStartStack; i < element_stack_.size(); ++i) {
    std::string buf;
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

HtmlElement* HtmlLexer::PopElementMatchingTag(Atom tag) {
  HtmlElement* element = NULL;

  // Search the stack from top to bottom.
  for (int i = element_stack_.size() - 1; i >= kStartStack; --i) {
    element = element_stack_[i];
    if (element->tag() == tag) {
      // Emit warnings for the tags we are skipping.  We have to do
      // this in reverse order so that we maintain stack discipline.
      for (int j = element_stack_.size() - 1; j > i; --j) {
        HtmlElement* skipped = element_stack_[j];
        // TODO(jmarantz): Should this be a Warning rather than an Error?
        // In fact, should we actually perform this optimization ourselves
        // in a filter to omit closing tags that can be inferred?
        html_parse_->Warning(filename_.c_str(), skipped->begin_line_number(),
                             "Unclosed element `%s'", skipped->tag().c_str());
        // Before closing the skipped element, pop it off the stack.  Otherwise,
        // the parent redundancy check in HtmlParse::AddEvent will fail.
        element_stack_.resize(j);
        html_parse_->CloseElement(skipped, HtmlElement::UNCLOSED, line_);
      }
      element_stack_.resize(i);
      break;
    }
    element = NULL;
  }
  return element;
}

void HtmlLexer::Warning(const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  html_parse_->WarningV(filename_.c_str(), line_, msg, args);
  va_end(args);
}

}  // namespace net_instaweb
