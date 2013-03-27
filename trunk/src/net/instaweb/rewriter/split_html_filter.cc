/*
 * Copyright 2012 Google Inc.
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

// Author: rahulbansal@google.com (Rahul Bansal)

#include "net/instaweb/rewriter/public/split_html_filter.h"

#include <map>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/htmlparse/public/html_writer_filter.h"
#include "net/instaweb/http/public/device_properties.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/http/public/logging_proto_impl.h"
#include "net/instaweb/rewriter/critical_line_info.pb.h"
#include "net/instaweb/rewriter/public/blink_util.h"
#include "net/instaweb/rewriter/public/lazyload_images_filter.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/json_writer.h"
#include "net/instaweb/util/public/re2.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/stl_util.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/writer.h"

namespace net_instaweb {

const char SplitHtmlFilter::kSplitInit[] =
    "<script type=\"text/javascript\">"
    "pagespeed.splitOnload = function() {"
    "pagespeed.num_high_res_images_loaded++;"
    "if (pagespeed.panelLoader && pagespeed.num_high_res_images_loaded == "
    "pagespeed.num_low_res_images_inlined) {"
    "pagespeed.panelLoader.loadData(null);"
    "}};"
    "pagespeed.num_high_res_images_loaded=0;"
    "</script>";
const char SplitHtmlFilter::kPagespeedFunc[] =
    "<script type=\"text/javascript\">"
    "window[\"pagespeed\"] = window[\"pagespeed\"] || {};"
    "var pagespeed = window[\"pagespeed\"];</script>";

// TODO(rahulbansal): We are sending an extra close body and close html tag.
// Fix that.
const char SplitHtmlFilter::kSplitSuffixJsFormatString[] =
    "<script type=\"text/javascript\">"
    "pagespeed.num_low_res_images_inlined=%d;</script>"
    "<script type=\"text/javascript\" src=\"%s\"></script>"
    "<script type=\"text/javascript\">"
      "pagespeed.panelLoaderInit();"
      "pagespeed.panelLoader.invokedFromSplit();"
      "pagespeed.panelLoader.loadCriticalData({});"
      "pagespeed.panelLoader.bufferNonCriticalData(%s, %s);"
    "</script>\n</body></html>\n";

// At StartElement, if element is panel instance push a new json to capture
// contents of instance to the json stack.
// All the emitBytes are captured into the top json until a new panel
// instance is found or the current panel instance ends.
SplitHtmlFilter::SplitHtmlFilter(RewriteDriver* rewrite_driver)
    : SuppressPreheadFilter(rewrite_driver),
      rewrite_driver_(rewrite_driver),
      options_(rewrite_driver->options()),
      current_panel_parent_element_(NULL),
      static_asset_manager_(NULL) {
}

SplitHtmlFilter::~SplitHtmlFilter() {
}

void SplitHtmlFilter::StartDocument() {
  flush_head_enabled_ = options_->Enabled(RewriteOptions::kFlushSubresources);
  disable_filter_ = !rewrite_driver_->device_properties()->SupportsSplitHtml(
      rewrite_driver_->options()->enable_aggressive_rewriters_for_mobile());
  static_asset_manager_ =
      rewrite_driver_->server_context()->static_asset_manager();
  if (disable_filter_) {
    InvokeBaseHtmlFilterStartDocument();
    return;
  }

  panel_id_to_spec_.clear();
  xpath_map_.clear();
  element_json_stack_.clear();
  xpath_units_.clear();
  num_children_stack_.clear();
  json_writer_.reset(new JsonWriter(rewrite_driver_->writer(),
                                    &element_json_stack_));
  original_writer_ = rewrite_driver_->writer();
  current_panel_id_.clear();
  url_ = rewrite_driver_->google_url().Spec();
  script_written_ = false;
  send_lazyload_script_ = false;
  num_low_res_images_inlined_ = 0;
  current_panel_parent_element_ = NULL;

  // Push the base panel.
  StartPanelInstance(static_cast<HtmlElement*>(NULL));
  // StartPanelInstance sets the json writer. For the base panel, we don't want
  // the writer to be set.
  set_writer(original_writer_);
  ProcessCriticalLineConfig();

  InvokeBaseHtmlFilterStartDocument();
}

void SplitHtmlFilter::Cleanup() {
  // Delete the root object pushed in StartDocument;
  delete element_json_stack_[0].second;
  element_json_stack_.pop_back();
  STLDeleteContainerPairSecondPointers(xpath_map_.begin(), xpath_map_.end());
}

void SplitHtmlFilter::EndDocument() {
  InvokeBaseHtmlFilterEndDocument();

  if (disable_filter_) {
    return;
  }

  // Remove critical html since it should already have been sent out by now.
  element_json_stack_[0].second->removeMember(BlinkUtil::kInstanceHtml);

  Json::Value json = Json::arrayValue;
  json.append(*(element_json_stack_[0].second));

  ServeNonCriticalPanelContents(json[0]);
  Cleanup();

  rewrite_driver_->UpdatePropertyValueInDomCohort(
      LazyloadImagesFilter::kIsLazyloadScriptInsertedPropertyName,
      send_lazyload_script_ ? "1" : "0");
}

void SplitHtmlFilter::WriteString(const StringPiece& str) {
  rewrite_driver_->writer()->Write(str, rewrite_driver_->message_handler());
}

void SplitHtmlFilter::ServeNonCriticalPanelContents(const Json::Value& json) {
  GoogleString non_critical_json = fast_writer_.write(json);
  BlinkUtil::StripTrailingNewline(&non_critical_json);
  BlinkUtil::EscapeString(&non_critical_json);
  WriteString(StringPrintf(
      kSplitSuffixJsFormatString,
      num_low_res_images_inlined_,
      GetBlinkJsUrl(options_, static_asset_manager_).c_str(),
      non_critical_json.c_str(),
      rewrite_driver_->flushing_cached_html() ? "true" : "false"));
  if (!json.empty()) {
    rewrite_driver_->log_record()->SetRewriterLoggingStatus(
        RewriteOptions::FilterId(RewriteOptions::kSplitHtml),
        RewriterInfo::APPLIED_OK);
    ScopedMutex lock(rewrite_driver_->log_record()->mutex());
    rewrite_driver_->log_record()->logging_info()->mutable_split_html_info()
        ->set_json_size(non_critical_json.size());
  }
  HtmlWriterFilter::Flush();
}

void SplitHtmlFilter::ProcessCriticalLineConfig() {
  const GoogleString& critical_line_config_from_options =
       options_->critical_line_config();
  if (rewrite_driver_->critical_line_info() == NULL &&
      !critical_line_config_from_options.empty()) {
    CriticalLineInfo* critical_line_info = new CriticalLineInfo;
    StringPieceVector xpaths;
    SplitStringPieceToVector(critical_line_config_from_options, ",",
                             &xpaths, true);
    for (int i = 0, n = xpaths.size(); i < n; i++) {
      StringPieceVector xpath_pair;
      SplitStringPieceToVector(xpaths[i], ":", &xpath_pair, true);
      Panel* panel = critical_line_info->add_panels();
      panel->set_start_xpath(xpath_pair[0].data(), xpath_pair[0].length());
      if (xpath_pair.size() == 2) {
        panel->set_end_marker_xpath(
            xpath_pair[1].data(), xpath_pair[1].length());
      }
    }
    rewrite_driver_->set_critical_line_info(critical_line_info);
  }
  critical_line_info_ = rewrite_driver_->critical_line_info();
  if (critical_line_info_ != NULL) {
    ComputePanels(*critical_line_info_, &panel_id_to_spec_);
    PopulateXpathMap(*critical_line_info_);
  }
}

void SplitHtmlFilter::ComputePanels(
    const CriticalLineInfo& critical_line_info,
    PanelIdToSpecMap* panel_id_to_spec) {
  for (int i = 0; i < critical_line_info.panels_size(); ++i) {
    const Panel& panel = critical_line_info.panels(i);
    const GoogleString panel_id =
        StrCat(BlinkUtil::kPanelId, ".", IntegerToString(i));
    (*panel_id_to_spec)[panel_id] = &panel;
  }
}

void SplitHtmlFilter::PopulateXpathMap(
    const CriticalLineInfo& critical_line_info) {
  for (int i = 0; i < critical_line_info.panels_size(); ++i) {
    const Panel& panel = critical_line_info.panels(i);
    PopulateXpathMap(panel.start_xpath());
    if (panel.has_end_marker_xpath()) {
      PopulateXpathMap(panel.end_marker_xpath());
    }
  }
}

void SplitHtmlFilter::PopulateXpathMap(const GoogleString& xpath) {
  if (xpath_map_.find(xpath) == xpath_map_.end()) {
    XpathUnits* xpath_units = new XpathUnits();
    ParseXpath(xpath, xpath_units);
    xpath_map_[xpath] = xpath_units;
  }
}

bool SplitHtmlFilter::IsElementSiblingOfCurrentPanel(HtmlElement* element) {
  return current_panel_parent_element_ != NULL &&
      current_panel_parent_element_ == element->parent();
}

bool SplitHtmlFilter::IsElementParentOfCurrentPanel(HtmlElement* element) {
  return current_panel_parent_element_ != NULL &&
      current_panel_parent_element_ == element;
}

void SplitHtmlFilter::EndPanelInstance() {
  json_writer_->UpdateDictionary();

  ElementJsonPair element_json_pair = element_json_stack_.back();
  scoped_ptr<Json::Value> dictionary(element_json_pair.second);
  element_json_stack_.pop_back();
  Json::Value* parent_dictionary = element_json_stack_.back().second;
  AppendJsonData(&((*parent_dictionary)[current_panel_id_]), *dictionary);
  current_panel_parent_element_ = NULL;
  current_panel_id_ = "";
  set_writer(original_writer_);
}

void SplitHtmlFilter::StartPanelInstance(HtmlElement* element) {
  if (element_json_stack_.size() != 0) {
    json_writer_->UpdateDictionary();
  }

  Json::Value* new_json = new Json::Value(Json::objectValue);
  // Push new Json
  element_json_stack_.push_back(std::make_pair(element, new_json));
  if (element != NULL) {
    current_panel_parent_element_ = element->parent();
    current_panel_id_ = GetPanelIdForInstance(element);
  }
  original_writer_ = rewrite_driver_->writer();
  set_writer(json_writer_.get());
}

void SplitHtmlFilter::InsertPanelStub(HtmlElement* element,
                                      const GoogleString& panel_id) {
  HtmlCommentNode* comment = rewrite_driver_->NewCommentNode(
      element->parent(),
      StrCat(RewriteOptions::kPanelCommentPrefix, " begin ", panel_id));
  rewrite_driver_->InsertElementBeforeCurrent(comment);
  Comment(comment);
  // Append end stub to json.
  comment = rewrite_driver_->NewCommentNode(
      element->parent(),
      StrCat(RewriteOptions::kPanelCommentPrefix, " end ", panel_id));
  rewrite_driver_->InsertElementBeforeCurrent(comment);
  Comment(comment);
}

void SplitHtmlFilter::InsertSplitInitScripts(HtmlElement* element) {
  // TODO(rahulbansal): Enable AddHead filter and this code can be made simpler.
  bool include_head = (element->keyword() != HtmlName::kHead);
  GoogleString defer_js_with_blink = "";
  if (include_head) {
    StrAppend(&defer_js_with_blink, "<head>");
  }

  // TODO(rahulbansal): It is sub-optimal to send lazyload script in the head.
  // Figure out a better way to do it.
  send_lazyload_script_ =
      LazyloadImagesFilter::ShouldApply(rewrite_driver_) &&
      options_->Enabled(RewriteOptions::kLazyloadImages);

  if (send_lazyload_script_ &&
      !rewrite_driver_->is_lazyload_script_flushed()) {
    GoogleString lazyload_js = LazyloadImagesFilter::GetLazyloadJsSnippet(
        options_, static_asset_manager_);
    StrAppend(&defer_js_with_blink, "<script type=\"text/javascript\">",
              lazyload_js, "</script>");
  }

  if (!send_lazyload_script_) {
    StrAppend(&defer_js_with_blink, kPagespeedFunc);
  }
  StrAppend(&defer_js_with_blink, kSplitInit);
  if (include_head) {
    StrAppend(&defer_js_with_blink, "</head>");
  }

  HtmlCharactersNode* blink_script_node = rewrite_driver_->NewCharactersNode(
      element, defer_js_with_blink);
  Characters(blink_script_node);
  script_written_ = true;
}

void SplitHtmlFilter::StartElement(HtmlElement* element) {
  if (disable_filter_) {
    InvokeBaseHtmlFilterStartElement(element);
    return;
  }

  if (!num_children_stack_.empty()) {
    // Ignore some of the non-rendered tags for numbering the children. This
    // helps avoid mismatches due to combine_javascript combining differently
    // and creating different numbers of script nodes in different rewrites.
    // This also helps when combine_css combines link tags or styles differently
    // in different rewrites.
    if (element->keyword() != HtmlName::kScript &&
        element->keyword() != HtmlName::kNoscript &&
        element->keyword() != HtmlName::kStyle &&
        element->keyword() != HtmlName::kLink) {
      num_children_stack_.back()++;;
    }
    num_children_stack_.push_back(0);
  } else if (element->keyword() == HtmlName::kBody) {
    // Start the stack only once body is encountered.
    num_children_stack_.push_back(0);
  }

  if (element->keyword() == HtmlName::kBody && !script_written_) {
    InsertSplitInitScripts(element);
  }

  if (IsEndMarkerForCurrentPanel(element)) {
    EndPanelInstance();
  }

  GoogleString panel_id = MatchPanelIdForElement(element);
  // if panel_id is empty, then element didn't match with any start xpath of
  // panel specs
  if (!panel_id.empty()) {
    InsertPanelStub(element, panel_id);
    MarkElementWithPanelId(element, panel_id);
    StartPanelInstance(element);
  } else if (IsElementSiblingOfCurrentPanel(element)) {
    MarkElementWithPanelId(element, current_panel_id_);
  }
  if (element_json_stack_.size() > 1) {
    // Suppress these bytes since they belong to a panel.
    HtmlWriterFilter::StartElement(element);
  } else {
    if (element->keyword() == HtmlName::kImg) {
      HtmlElement::Attribute* pagespeed_high_res_src_attr =
          element->FindAttribute(HtmlName::HtmlName::kPagespeedHighResSrc);
      HtmlElement::Attribute* onload =
          element->FindAttribute(HtmlName::kOnload);
      if (pagespeed_high_res_src_attr != NULL &&
          pagespeed_high_res_src_attr->DecodedValueOrNull() != NULL &&
          onload != NULL && onload->DecodedValueOrNull() != NULL) {
        num_low_res_images_inlined_++;
        GoogleString overridden_onload = StrCat("pagespeed.splitOnload();",
            onload->DecodedValueOrNull());
        onload->SetValue(overridden_onload);
      }
    }
    InvokeBaseHtmlFilterStartElement(element);
  }
}

void SplitHtmlFilter::EndElement(HtmlElement* element) {
  if (disable_filter_) {
    InvokeBaseHtmlFilterEndElement(element);
    return;
  }

  if (!num_children_stack_.empty()) {
    num_children_stack_.pop_back();
  }
  if (IsElementParentOfCurrentPanel(element) ||
      (element->parent() == NULL &&
       element_json_stack_.back().first == element)) {
    EndPanelInstance();
  }

  if (element->keyword() == HtmlName::kHead && !script_written_) {
    InsertSplitInitScripts(element);
  }

  if (element_json_stack_.size() > 1) {
    // Suppress these bytes since they belong to a panel.
    HtmlWriterFilter::EndElement(element);
  } else {
    InvokeBaseHtmlFilterEndElement(element);
  }
}

void SplitHtmlFilter::AppendJsonData(Json::Value* dictionary,
                                 const Json::Value& dict) {
  if (!dictionary->isArray()) {
    *dictionary = Json::arrayValue;
  }
  dictionary->append(dict);
}

GoogleString SplitHtmlFilter::MatchPanelIdForElement(HtmlElement* element) {
  if (critical_line_info_ == NULL) {
    return "";
  }
  for (int i = 0; i < critical_line_info_->panels_size(); i++) {
    const Panel& panel = critical_line_info_->panels(i);
    if (ElementMatchesXpath(element, *(xpath_map_[panel.start_xpath()]))) {
      return StrCat(BlinkUtil::kPanelId, ".", IntegerToString(i));
    }
  }
  return "";
}

bool SplitHtmlFilter::IsEndMarkerForCurrentPanel(HtmlElement* element) {
  if (current_panel_parent_element_ == NULL) {
    return false;
  }

  if (panel_id_to_spec_.find(current_panel_id_) == panel_id_to_spec_.end()) {
    LOG(DFATAL) << "Invalid Panelid: "
                << current_panel_id_ << " for url " << url_;
    return false;
  }
  const Panel& panel = *(panel_id_to_spec_[current_panel_id_]);
  return panel.has_end_marker_xpath() ?
      ElementMatchesXpath(element, *(xpath_map_[panel.end_marker_xpath()])) :
      false;
}

void SplitHtmlFilter::MarkElementWithPanelId(HtmlElement* element,
                                         const GoogleString& panel_id) {
  element->AddAttribute(rewrite_driver_->MakeName(BlinkUtil::kPanelId),
                        panel_id, HtmlElement::DOUBLE_QUOTE);
}

GoogleString SplitHtmlFilter::GetPanelIdForInstance(HtmlElement* element) {
  GoogleString panel_id_value;
  StringPiece panel_id_attr_name = BlinkUtil::kPanelId;
  const HtmlElement::AttributeList& attrs = element->attributes();
  for (HtmlElement::AttributeConstIterator i(attrs.begin());
         i != attrs.end(); ++i) {
      const HtmlElement::Attribute& attribute = *i;
    if ((panel_id_attr_name == attribute.name().c_str()) &&
        (attribute.DecodedValueOrNull() != NULL)) {
      panel_id_value = attribute.DecodedValueOrNull();
      break;
    }
  }
  return panel_id_value;
}

bool SplitHtmlFilter::ParseXpath(const GoogleString& xpath,
                                 std::vector<XpathUnit>* xpath_units) {
  static const char* kXpathWithChildNumber = "(\\w+)(\\[(\\d+)\\])";
  static const char* kXpathWithId = "(\\w+)(\\[@(\\w+)\\s*=\\s*\"(.*)\"\\])";
  StringPieceVector list;
  net_instaweb::SplitStringUsingSubstr(xpath, "/", &list);
  for (int j = 0, n = list.size(); j < n; j++) {
    XpathUnit unit;
    GoogleString str;
    StringPiece match = list[j];
    if (!RE2::FullMatch(StringPieceToRe2(match), kXpathWithChildNumber,
                        &unit.tag_name, &str, &unit.child_number)) {
      GoogleString str1;
      RE2::FullMatch(StringPieceToRe2(match), kXpathWithId, &unit.tag_name,
                     &str, &str1, &unit.attribute_value);
    }
    xpath_units->push_back(unit);
  }
  return true;
}

bool SplitHtmlFilter::ElementMatchesXpath(
    const HtmlElement* element, const std::vector<XpathUnit>& xpath_units) {
  int j = xpath_units.size() - 1, k = num_children_stack_.size() - 2;
  for (; j >= 0 && k >= 0; j--, k--, element = element->parent()) {
    if (element->name_str() !=  xpath_units[j].tag_name) {
      return false;
    }
    if (!xpath_units[j].attribute_value.empty()) {
      return (element->AttributeValue(HtmlName::kId) != NULL &&
          element->AttributeValue(HtmlName::kId) ==
              xpath_units[j].attribute_value);
    } else if (xpath_units[j].child_number == num_children_stack_[k]) {
      continue;
    } else {
      return false;
    }
  }

  if (j < 0 && k < 0) {
    return true;
  }
  return false;
}

const GoogleString& SplitHtmlFilter::GetBlinkJsUrl(
      const RewriteOptions* options,
      StaticAssetManager* static_asset_manager) {
  return static_asset_manager->GetAssetUrl(StaticAssetManager::kBlinkJs,
                                           options);
}

// TODO(rahulbansal): Refactor this pattern.
void SplitHtmlFilter::InvokeBaseHtmlFilterStartDocument() {
  if (flush_head_enabled_) {
    SuppressPreheadFilter::StartDocument();
  } else {
    HtmlWriterFilter::StartDocument();
  }
}

void SplitHtmlFilter::InvokeBaseHtmlFilterStartElement(HtmlElement* element) {
  if (flush_head_enabled_) {
    SuppressPreheadFilter::StartElement(element);
  } else {
    HtmlWriterFilter::StartElement(element);
  }
}

void SplitHtmlFilter::InvokeBaseHtmlFilterEndElement(HtmlElement* element) {
  if (flush_head_enabled_) {
    SuppressPreheadFilter::EndElement(element);
  } else {
    HtmlWriterFilter::EndElement(element);
  }
}

void SplitHtmlFilter::InvokeBaseHtmlFilterEndDocument() {
  if (flush_head_enabled_) {
    SuppressPreheadFilter::EndDocument();
  } else {
    HtmlWriterFilter::EndDocument();
  }
}

}  // namespace net_instaweb
