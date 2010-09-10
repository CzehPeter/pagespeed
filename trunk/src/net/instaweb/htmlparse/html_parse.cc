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

#include "net/instaweb/htmlparse/public/html_parse.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <utility>  // for std::pair

#include "base/logging.h"
#include "net/instaweb/htmlparse/html_event.h"
#include "net/instaweb/htmlparse/html_lexer.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_escape.h"
#include "net/instaweb/htmlparse/public/html_filter.h"
#include "net/instaweb/util/public/message_handler.h"
#include <string>
#include "net/instaweb/util/public/timer.h"

namespace net_instaweb {

HtmlParse::HtmlParse(MessageHandler* message_handler)
    : lexer_(NULL),  // Can't initialize here, since "this" should not be used
                     // in the initializer list (it generates an error in
                     // Visual Studio builds).
      sequence_(0),
      current_(queue_.end()),
      deleted_current_(false),
      message_handler_(message_handler),
      line_number_(1),
      need_sanity_check_(false),
      coalesce_characters_(true),
      need_coalesce_characters_(false),
      parse_start_time_us_(0),
      timer_(NULL) {
  lexer_ = new HtmlLexer(this);
  HtmlEscape::Init();
}

HtmlParse::~HtmlParse() {
  delete lexer_;
  ClearElements();
}

void HtmlParse::AddFilter(HtmlFilter* html_filter) {
  filters_.push_back(html_filter);
}

HtmlEventListIterator HtmlParse::Last() {
  HtmlEventListIterator p = queue_.end();
  --p;
  return p;
}

// Checks that the parent provided when creating the event's Node is
// consistent with the position in the list.  An alternative approach
// here is to use this code and remove the explicit specification of
// parents when constructing nodes.
//
// A complexity we will run into with that approach is that the queue_ is
// cleared on a Flush, so we cannot reliably derive the correct parent
// from the queue.  However, the Lexer keeps an element stack across
// flushes, and therefore can keep correct parent pointers.  So we have
// to inject pessimism in this process.
//
// Note that we also have sanity checks that run after each filter.
void HtmlParse::CheckParentFromAddEvent(HtmlEvent* event) {
  HtmlNode* node = event->GetNode();
  if (node != NULL) {
    CHECK(lexer_->Parent() == node->parent());
  }
}

// Testing helper method
void HtmlParse::AddEvent(HtmlEvent* event) {
  CheckParentFromAddEvent(event);
  queue_.push_back(event);
  need_sanity_check_ = true;
  need_coalesce_characters_ = true;

  // If this is a leaf-node event, we need to set the iterator of the
  // corresponding leaf node to point to this event's position in the queue.
  // If this is an element event, then the iterators of the element will get
  // set in HtmlParse::AddElement and HtmlParse::CloseElement, so there's no
  // need to do it here.  If this is some other kind of event, there are no
  // iterators to set.
  HtmlLeafNode* leaf = event->GetLeafNode();
  if (leaf != NULL) {
    leaf->set_iter(Last());
    CHECK(IsRewritable(leaf));
  }
}

// Testing helper method
void HtmlParse::SetCurrent(HtmlNode* node) {
  current_ = node->begin();
}

HtmlCdataNode* HtmlParse::NewCdataNode(HtmlElement* parent,
                                       const StringPiece& contents) {
  HtmlCdataNode* cdata = new HtmlCdataNode(parent, contents, queue_.end());
  nodes_.insert(cdata);
  return cdata;
}

HtmlCharactersNode* HtmlParse::NewCharactersNode(HtmlElement* parent,
                                                 const StringPiece& literal) {
  HtmlCharactersNode* characters =
      new HtmlCharactersNode(parent, literal, queue_.end());
  nodes_.insert(characters);
  return characters;
}

HtmlCommentNode* HtmlParse::NewCommentNode(HtmlElement* parent,
                                           const StringPiece& contents) {
  HtmlCommentNode* comment = new HtmlCommentNode(parent, contents,
                                                 queue_.end());
  nodes_.insert(comment);
  return comment;
}

HtmlDirectiveNode* HtmlParse::NewDirectiveNode(HtmlElement* parent,
                                               const StringPiece& contents) {
  HtmlDirectiveNode* directive = new HtmlDirectiveNode(parent, contents,
                                                       queue_.end());
  nodes_.insert(directive);
  return directive;
}

HtmlElement* HtmlParse::NewElement(HtmlElement* parent, Atom tag) {
  HtmlElement* element = new HtmlElement(parent, tag, queue_.end(),
                                         queue_.end());
  nodes_.insert(element);
  element->set_sequence(sequence_++);
  return element;
}

void HtmlParse::AddElement(HtmlElement* element, int line_number) {
  HtmlStartElementEvent* event =
      new HtmlStartElementEvent(element, line_number);
  AddEvent(event);
  element->set_begin(Last());
  element->set_begin_line_number(line_number);
}

void HtmlParse::StartParse(const StringPiece& url) {
  line_number_ = 1;
  url.CopyToString(&filename_);
  if (timer_ != NULL) {
    parse_start_time_us_ = timer_->NowUs();
    InfoHere("HtmlParse::StartParse");
  }
  AddEvent(new HtmlStartDocumentEvent(line_number_));
  lexer_->StartParse(url);
}

void HtmlParse::ShowProgress(const char* message) {
  if (timer_ != NULL) {
    long delta = static_cast<long>(timer_->NowUs() - parse_start_time_us_);
    InfoHere("%ldus: HtmlParse::%s", delta, message);
  }
}

void HtmlParse::FinishParse() {
  lexer_->FinishParse();
  AddEvent(new HtmlEndDocumentEvent(line_number_));
  Flush();
  ClearElements();
  ShowProgress("FinishParse");
}

void HtmlParse::ParseText(const char* text, int size) {
  lexer_->Parse(text, size);
}

// This is factored out of Flush() for testing purposes.
void HtmlParse::ApplyFilter(HtmlFilter* filter) {
  if (coalesce_characters_ && need_coalesce_characters_) {
    CoalesceAdjacentCharactersNodes();
    need_coalesce_characters_ = false;
  }

  ShowProgress(StrCat("ApplyFilter:", filter->Name()).c_str());
  for (current_ = queue_.begin(); current_ != queue_.end(); ) {
    HtmlEvent* event = *current_;
    line_number_ = event->line_number();
    event->Run(filter);
    deleted_current_ = false;
    ++current_;
  }
  filter->Flush();

  if (need_sanity_check_) {
    SanityCheck();
    need_sanity_check_ = false;
  }
}

void HtmlParse::CoalesceAdjacentCharactersNodes() {
  ShowProgress("CoalesceAdjacentCharactersNodes");
  HtmlCharactersNode* prev = NULL;
  for (current_ = queue_.begin(); current_ != queue_.end(); ) {
    HtmlEvent* event = *current_;
    HtmlCharactersNode* node = event->GetCharactersNode();
    if ((node != NULL) && (prev != NULL)) {
      prev->Append(node->contents());
      current_ = queue_.erase(current_);  // returns element after erased
      delete event;
      node->MarkAsDead(queue_.end());
      need_sanity_check_ = true;
    } else {
      ++current_;
      prev = node;
    }
  }
}

void HtmlParse::CheckEventParent(HtmlEvent* event, HtmlElement* expect,
                                 HtmlElement* actual) {
  if ((expect != NULL) && (actual != expect)) {
    std::string actual_buf, expect_buf, event_buf;
    if (actual != NULL) {
      actual->ToString(&actual_buf);
    } else {
      actual_buf = "(null)";
    }
    expect->ToString(&expect_buf);
    event->ToString(&event_buf);
    FatalErrorHere("HtmlElement Parents of %s do not match:\n"
                   "Actual:   %s\n"
                   "Expected: %s\n",
                   event_buf.c_str(), actual_buf.c_str(), expect_buf.c_str());
  }
}

void HtmlParse::SanityCheck() {
  ShowProgress("SanityCheck");

  // Sanity check that the Node parent-pointers are consistent with the
  // begin/end-element events.  This is done in a second pass to avoid
  // confusion when the filter mutates the event-stream.  Also note that
  // a mid-HTML call to HtmlParse::Flush means that we may pop out beyond
  // the stack we can detect in this event stream.  This is represented
  // here by an empty stack.
  std::vector<HtmlElement*> element_stack;
  HtmlElement* expect_parent = NULL;
  for (current_ = queue_.begin(); current_ != queue_.end(); ++current_) {
    HtmlEvent* event = *current_;

    // Determine whether the event we are looking at is a StartElement,
    // EndElement, or a leaf.  We manipulate our temporary stack when
    // we see StartElement and EndElement, and we always test to make
    // sure the elements have the expected parent based on context, when
    // we can figure out what the expected parent is.
    HtmlElement* start_element = event->GetStartElement();
    HtmlElement* actual_parent = NULL;
    if (start_element != NULL) {
      CheckEventParent(event, expect_parent, start_element->parent());
      CHECK(start_element->begin() == current_);
      CHECK(start_element->live());
      element_stack.push_back(start_element);
      expect_parent = start_element;
    } else {
      HtmlElement* end_element = event->GetEndElement();
      if (end_element != NULL) {
        CHECK(end_element->end() == current_);
        CHECK(end_element->live());
        if (!element_stack.empty()) {
          // The element stack can be empty on End can happen due
          // to this sequence:
          //   <tag1>
          //     FLUSH
          //   </tag1>   <!-- tag1 close seen with empty stack -->
          CHECK(element_stack.back() == end_element);
          element_stack.pop_back();
        }
        actual_parent = end_element->parent();
        expect_parent = element_stack.empty() ? NULL : element_stack.back();
        CheckEventParent(event, expect_parent, end_element->parent());
      } else {
        // We only know for sure what the parents are once we have seen
        // a start_element.
        HtmlLeafNode* leaf_node = event->GetLeafNode();
        if (leaf_node != NULL) {   // Start/EndDocument are not leaf nodes
          CHECK(leaf_node->live());
          CHECK(leaf_node->end() == current_);
          CheckEventParent(event, expect_parent, leaf_node->parent());
        }
      }
    }
  }
}

void HtmlParse::Flush() {
  ShowProgress("Flush");

  for (size_t i = 0; i < filters_.size(); ++i) {
    HtmlFilter* filter = filters_[i];
    ApplyFilter(filter);
  }

  // Detach all the elements from their events, as we are now invalidating
  // the events, but not the elements.
  for (current_ = queue_.begin(); current_ != queue_.end(); ++current_) {
    HtmlEvent* event = *current_;
    line_number_ = event->line_number();
    HtmlElement* element = event->GetStartElement();
    if (element != NULL) {
      element->set_begin(queue_.end());
    } else {
      element = event->GetEndElement();
      if (element != NULL) {
        element->set_end(queue_.end());
      } else {
        HtmlLeafNode* leaf_node = event->GetLeafNode();
        if (leaf_node != NULL) {
          leaf_node->set_iter(queue_.end());
        }
      }
    }
    delete event;
  }
  queue_.clear();
  need_sanity_check_ = false;
  need_coalesce_characters_ = false;
}

bool HtmlParse::InsertElementBeforeElement(const HtmlNode* existing_node,
                                           HtmlNode* new_node) {
  CHECK(existing_node->parent() == new_node->parent());
  return InsertElementBeforeEvent(existing_node->begin(), new_node);
}

bool HtmlParse::InsertElementAfterElement(const HtmlNode* existing_node,
                                          HtmlNode* new_node) {
  CHECK(existing_node->parent() == new_node->parent());
  HtmlEventListIterator event = existing_node->end();
  ++event;
  return InsertElementBeforeEvent(event, new_node);
}

bool HtmlParse::InsertElementBeforeCurrent(HtmlNode* new_node) {
  if (deleted_current_) {
    FatalErrorHere("InsertElementBeforeCurrent after current has been "
                   "deleted.");
  }
  return InsertElementBeforeEvent(current_, new_node);
}

bool HtmlParse::InsertElementBeforeEvent(const HtmlEventListIterator& event,
                                         HtmlNode* new_node) {
  new_node->SynthesizeEvents(event, &queue_);
  need_sanity_check_ = true;
  need_coalesce_characters_ = true;
  // TODO(jmarantz): make this routine return void, as well as the other
  // wrappers around it.
  return true;
}

bool HtmlParse::InsertElementAfterCurrent(HtmlNode* new_node) {
  if (deleted_current_) {
    FatalErrorHere("InsertElementAfterCurrent after current has been "
                   "deleted.");
  }
  if (current_ == queue_.end()) {
    FatalErrorHere("InsertElementAfterCurrent called with queue at end.");
  }
  ++current_;
  bool ret = InsertElementBeforeEvent(current_, new_node);

  // We want to leave current_ pointing to the newly created element.
  --current_;
  CHECK_EQ((*current_)->GetNode(), new_node);
  return ret;
}

bool HtmlParse::AddParentToSequence(HtmlNode* first, HtmlNode* last,
                                    HtmlElement* new_parent) {
  bool added = false;
  HtmlElement* original_parent = first->parent();
  if (IsRewritable(first) && IsRewritable(last) &&
      (last->parent() == original_parent) &&
      (new_parent->begin() == queue_.end()) &&
      (new_parent->end() == queue_.end()) &&
      InsertElementBeforeEvent(first->begin(), new_parent)) {
    // This sequence of checks culminated in inserting the parent's begin
    // and end before 'first'.  Now we must mutate new_parent's end pointer
    // to insert it after the last->end().  list::insert(iter) inserts
    // *before* the iter, so we'll increment last->end().
    HtmlEvent* end_element_event = *new_parent->end();
    queue_.erase(new_parent->end());
    HtmlEventListIterator p = last->end();
    ++p;
    new_parent->set_end(queue_.insert(p, end_element_event));
    FixParents(first->begin(), last->end(), new_parent);
    added = true;
    need_sanity_check_ = true;
    need_coalesce_characters_ = true;
  }
  return added;
}

void HtmlParse::FixParents(const HtmlEventListIterator& begin,
                           const HtmlEventListIterator& end_inclusive,
                           HtmlElement* new_parent) {
  HtmlEvent* event = *begin;
  HtmlNode* first = event->GetNode();
  HtmlElement* original_parent = first->parent();
  // Loop over all the nodes from begin to end, inclusive,
  // and set the parent pointer for the node, if there is one.  A few
  // event types don't have HtmlNodes, such as Comments and IEDirectives.
  CHECK(end_inclusive != queue_.end());
  HtmlEventListIterator end = end_inclusive;
  ++end;
  for (HtmlEventListIterator p = begin; p != end; ++p) {
    HtmlNode* node = (*p)->GetNode();
    if ((node != NULL) && (node->parent() == original_parent)) {
      node->set_parent(new_parent);
    }
  }
}

bool HtmlParse::MoveCurrentInto(HtmlElement* new_parent) {
  bool moved = false;
  HtmlNode* node = (*current_)->GetNode();
  if ((node != NULL) && (node != new_parent) &&
      IsRewritable(node) && IsRewritable(new_parent)) {
    HtmlEventListIterator begin = node->begin();
    HtmlEventListIterator end = node->end();
    ++end;  // splice is non-inclusive for the 'end' iterator.

    // Manipulate current_ so that when Flush() iterates it lands
    // you on object after current_'s original position, rather
    // than re-iterating over the new_parent's EndElement event.
    current_ = end;
    queue_.splice(new_parent->end(), queue_, begin, end);
    --current_;

    // TODO(jmarantz): According to
    // http://www.cplusplus.com/reference/stl/list/splice/
    // the moved iterators are no longer valid, and we
    // are retaining them in the HtmlNode, so we need to fix them.
    //
    // However, in practice they appear to remain valid.  And
    // I can't think of a reason they should be invalidated,
    // as the iterator is a pointer to a node structure with
    // next/prev pointers.  splice can mutate the next/prev pointers
    // in place.
    //
    // See http://stackoverflow.com/questions/143156

    FixParents(node->begin(), node->end(), new_parent);
    moved = true;
    need_sanity_check_ = true;
    need_coalesce_characters_ = true;
  }
  return moved;
}

bool HtmlParse::DeleteElement(HtmlNode* node) {
  bool deleted = false;
  if (IsRewritable(node)) {
    bool done = false;
    // If node is an HtmlLeafNode, then begin() and end() might be the same.
    for (HtmlEventListIterator p = node->begin(); !done; ) {
      // We want to include end, so once p == end we still have to do one more
      // iteration.
      done = (p == node->end());

      // Clean up any nested elements/leaves as we get to their 'end' event.
      HtmlEvent* event = *p;
      HtmlNode* nested_node = event->GetEndElement();
      if (nested_node == NULL) {
        nested_node = event->GetLeafNode();
      }
      if (nested_node != NULL) {
        std::set<HtmlNode*>::iterator iter = nodes_.find(nested_node);
        CHECK(iter != nodes_.end());
        CHECK(nested_node->live());
        nested_node->MarkAsDead(queue_.end());
      }

      // Check if we're about to delete the current event.
      bool move_current = (p == current_);
      p = queue_.erase(p);
      if (move_current) {
        current_ = p;  // p is the event *after* the old current.
        --current_;    // Go to *previous* event so that we don't skip p.
        deleted_current_ = true;
        line_number_ = (*current_)->line_number();
      }
      delete event;
    }

    // Our iteration should have covered the passed-in element as well.
    CHECK(!node->live());
    deleted = true;
    need_sanity_check_ = true;
    need_coalesce_characters_ = true;
  }
  return deleted;
}

bool HtmlParse::DeleteSavingChildren(HtmlElement* element) {
  bool deleted = false;
  if (IsRewritable(element)) {
    HtmlElement* new_parent = element->parent();
    HtmlEventListIterator first = element->begin();
    ++first;
    HtmlEventListIterator last = element->end();
    if (first != last) {
      --last;
      FixParents(first, last, new_parent);
      queue_.splice(element->begin(), queue_, first, element->end());
      need_sanity_check_ = true;
      need_coalesce_characters_ = true;
    }
    deleted = DeleteElement(element);
  }
  return deleted;
}

bool HtmlParse::ReplaceNode(HtmlNode* existing_node, HtmlNode* new_node) {
  bool replaced = false;
  if (IsRewritable(existing_node)) {
    replaced = InsertElementBeforeElement(existing_node, new_node);
    CHECK(replaced);
    replaced = DeleteElement(existing_node);
    CHECK(replaced);
  }
  return replaced;
}

bool HtmlParse::IsRewritable(const HtmlNode* node) const {
  return IsInEventWindow(node->begin()) && IsInEventWindow(node->end());
}

bool HtmlParse::IsInEventWindow(const HtmlEventListIterator& iter) const {
  return iter != queue_.end();
}

void HtmlParse::ClearElements() {
  for (std::set<HtmlNode*>::iterator p = nodes_.begin(),
           e = nodes_.end(); p != e; ++p) {
    HtmlNode* node = *p;
    delete node;
  }
  nodes_.clear();
}

void HtmlParse::DebugPrintQueue() {
  for (HtmlEventList::iterator p = queue_.begin(), e = queue_.end();
       p != e; ++p) {
    std::string buf;
    HtmlEvent* event = *p;
    event->ToString(&buf);
    long node_ptr = reinterpret_cast<long>(event->GetNode());
    if (p == current_) {
      fprintf(stdout, "* %s (0x%lx)\n", buf.c_str(), node_ptr);
    } else {
      fprintf(stdout, "  %s (0x%lx)\n", buf.c_str(), node_ptr);
    }
  }
  fflush(stdout);
}

bool HtmlParse::IsImplicitlyClosedTag(Atom tag) const {
  return lexer_->IsImplicitlyClosedTag(tag);
}

bool HtmlParse::TagAllowsBriefTermination(Atom tag) const {
  return lexer_->TagAllowsBriefTermination(tag);
}


void HtmlParse::InfoV(
    const char* file, int line, const char *msg, va_list args) {
  message_handler_->InfoV(file, line, msg, args);
}

void HtmlParse::WarningV(
    const char* file, int line, const char *msg, va_list args) {
  message_handler_->WarningV(file, line, msg, args);
}

void HtmlParse::ErrorV(
    const char* file, int line, const char *msg, va_list args) {
  message_handler_->ErrorV(file, line, msg, args);
}

void HtmlParse::FatalErrorV(
    const char* file, int line, const char* msg, va_list args) {
  message_handler_->FatalErrorV(file, line, msg, args);
}

void HtmlParse::Info(const char* file, int line, const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  InfoV(file, line, msg, args);
  va_end(args);
}

void HtmlParse::Warning(const char* file, int line, const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  WarningV(file, line, msg, args);
  va_end(args);
}

void HtmlParse::Error(const char* file, int line, const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  ErrorV(file, line, msg, args);
  va_end(args);
}

void HtmlParse::FatalError(const char* file, int line, const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  FatalErrorV(file, line, msg, args);
  va_end(args);
}

void HtmlParse::InfoHere(const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  InfoHereV(msg, args);
  va_end(args);
}

void HtmlParse::WarningHere(const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  WarningHereV(msg, args);
  va_end(args);
}

void HtmlParse::ErrorHere(const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  ErrorHereV(msg, args);
  va_end(args);
}

void HtmlParse::FatalErrorHere(const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  FatalErrorHereV(msg, args);
  va_end(args);
}

void HtmlParse::CloseElement(
    HtmlElement* element, HtmlElement::CloseStyle close_style,
    int line_number) {
  HtmlEndElementEvent* end_event =
      new HtmlEndElementEvent(element, line_number);
  element->set_close_style(close_style);
  AddEvent(end_event);
  element->set_end(Last());
  element->set_end_line_number(line_number);
}

}  // namespace net_instaweb
