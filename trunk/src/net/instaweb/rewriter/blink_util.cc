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

// Author: gagansingh@google.com (Gagan Singh)

#include "net/instaweb/rewriter/public/blink_util.h"

#include <cstddef>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/log_record.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/user_agent_matcher.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/string.h"

namespace net_instaweb {
namespace BlinkUtil {

namespace {

bool IsAllIncludedIn(const StringPieceVector& spec_vector,
                     const StringPieceVector& value_vector) {
  for (int i = 0, m = spec_vector.size(); i < m; ++i) {
    bool found_spec_item = false;
    for (int j = 0, n = value_vector.size(); j < n; ++j) {
      if (StringCaseCompare(value_vector[j], spec_vector[i]) == 0) {
        // The i'th token in spec is there in value.
        found_spec_item = true;
        break;
      }
    }
    if (!found_spec_item) {
      // If a token in spec is not found in value then we can return false.
      return false;
    }
  }
  // Found all in spec in value.
  return true;
}

}  // namespace

bool IsUserAgentAllowedForBlink(AsyncFetch* async_fetch,
                                const RewriteOptions* options,
                                const char* user_agent,
                                const UserAgentMatcher& user_agent_matcher) {
  UserAgentMatcher::BlinkRequestType request_type =
      user_agent_matcher.GetBlinkRequestType(
          user_agent, async_fetch->request_headers());
  BlinkInfo* blink_info =
      async_fetch->log_record()->logging_info()->mutable_blink_info();
  switch (request_type) {
    case UserAgentMatcher::kBlinkWhiteListForDesktop:
      blink_info->set_blink_user_agent(BlinkInfo::BLINK_DESKTOP_WHITELIST);
      return true;
    case UserAgentMatcher::kDoesNotSupportBlink:
      blink_info->set_blink_user_agent(BlinkInfo::NOT_SUPPORT_BLINK);
      return false;
    case UserAgentMatcher::kBlinkBlackListForDesktop:
      blink_info->set_blink_user_agent(BlinkInfo::BLINK_DESKTOP_BLACKLIST);
      return false;
    case UserAgentMatcher::kBlinkMobile:
      blink_info->set_blink_user_agent(BlinkInfo::BLINK_MOBILE);
      // Is mobile request allowed?
      return (options->enable_blink_for_mobile_devices()) ? true : false;
    case UserAgentMatcher::kNullOrEmpty:
      blink_info->set_blink_user_agent(BlinkInfo::NULL_OR_EMPTY);
      return false;
  }
  return false;
}

// TODO(rahulbansal): Add tests for this.
bool IsBlinkRequest(const GoogleUrl& url,
                    AsyncFetch* async_fetch,
                    const RewriteOptions* options,
                    const char* user_agent,
                    const UserAgentMatcher& user_agent_matcher) {
  if (options != NULL &&
      // Is rewriting enabled?
      options->enabled() &&
      // Is Get Request?
      async_fetch->request_headers()->method() == RequestHeaders::kGet &&
      // Is prioritize visible content filter enabled?
      options->Enabled(RewriteOptions::kPrioritizeVisibleContent) &&
      // Is url allowed? (i.e., it is not in black-list.)
      // TODO(sriharis): We also make this check in regular proxy flow
      // (ProxyFetch).  Should we combine these?
      options->IsAllowed(url.Spec()) &&
      // Does url match a cacheable family pattern specified in config?
      options->IsInBlinkCacheableFamily(url) &&
      // Is the user agent allowed to enter the blink flow?
      IsUserAgentAllowedForBlink(async_fetch, options,
                                 user_agent, user_agent_matcher)) {
    return true;
  }
  return false;
}

bool ShouldApplyBlinkFlowCriticalLine(
    const ServerContext* manager,
    const RewriteOptions* options) {
  return options != NULL &&
      // Blink flow critical line is enabled in rewrite options.
      options->enable_blink_critical_line() &&
      manager->blink_critical_line_data_finder() != NULL;
}

bool IsJsonEmpty(const Json::Value& json) {
  const std::vector<std::string>& keys = json.getMemberNames();
  for (Json::ArrayIndex k = 0; k < keys.size(); ++k) {
    const std::string& key = keys[k];
    if (key != kContiguous) {
      return false;
    }
  }
  return true;
}

void ClearArrayIfAllEmpty(Json::Value* json) {
  for (Json::ArrayIndex i = 0; i < json->size(); ++i) {
    if (!IsJsonEmpty((*json)[i])) {
      return;
    }
  }
  json->clear();
}

void EscapeString(GoogleString* str) {
  // TODO(sriharis):  Check whether we need to do any other escaping.  Also
  // change the escaping of '<' and '>' to use standard '\u' mechanism.
  int num_replacements = 0;
  GoogleString tmp;
  const int length = str->length();
  for (int i = 0; i < length; ++i) {
    const unsigned char c = (*str)[i];
    switch (c) {
      case '<': {
        ++num_replacements;
        tmp.append("__psa_lt;");
        break;
      }
      case '>': {
        ++num_replacements;
        tmp.append("__psa_gt;");
        break;
      }
      case 0xe2: {
        if ((i + 2 < length) && ((*str)[i + 1] == '\x80')) {
          if ((*str)[i + 2] == '\xa8') {
            ++num_replacements;
            tmp.append("\\u2028");
            i += 2;
            break;
          } else if ((*str)[i + 2] == '\xa9') {
            ++num_replacements;
            tmp.append("\\u2029");
            i += 2;
            break;
          }
        }
        tmp.push_back(c);
        break;
      }
      default: {
        tmp.push_back(c);
        break;
      }
    }
  }
  if (num_replacements > 0) {
    str->swap(tmp);
  }
}

bool StripTrailingNewline(GoogleString* s) {
  if (!s->empty() && (*s)[s->size() - 1] == '\n') {
    if (s->size() > 1 && (*s)[s->size() - 2] == '\r')
      s->resize(s->size() - 2);
    else
      s->resize(s->size() - 1);
    return true;
  }
  return false;
}

void PopulateAttributeToNonCacheableValuesMap(
    const RewriteOptions* rewrite_options, const GoogleUrl& url,
    AttributesToNonCacheableValuesMap* attribute_non_cacheable_values_map,
    std::vector<int>* panel_number_num_instances) {
  GoogleString non_cacheable_elements_str =
      rewrite_options->GetBlinkNonCacheableElementsFor(url);
  StringPiece non_cacheable_elements(non_cacheable_elements_str);
  // TODO(rahulbansal): Add more error checking.
  StringPieceVector non_cacheable_values;
  SplitStringPieceToVector(non_cacheable_elements,
                           ",", &non_cacheable_values, true);
  for (size_t i = 0; i < non_cacheable_values.size(); ++i) {
    StringPieceVector non_cacheable_values_pair;
    SplitStringPieceToVector(non_cacheable_values[i], "=",
                             &non_cacheable_values_pair, true);
    if (non_cacheable_values_pair.size() != 2) {
      LOG(ERROR) << "Incorrect non cacheable element value "
                 << non_cacheable_values[i];
      return;
    }
    StringPiece attribute_name = non_cacheable_values_pair[0];
    StringPiece attribute_value = non_cacheable_values_pair[1];
    TrimWhitespace(&attribute_name);
    TrimQuote(&attribute_value);
    attribute_non_cacheable_values_map->insert(make_pair(
        attribute_name.as_string(),
        make_pair(attribute_value.as_string(), i)));
    panel_number_num_instances->push_back(0);
  }
}

int GetPanelNumberForNonCacheableElement(
    const AttributesToNonCacheableValuesMap& attribute_non_cacheable_values_map,
    const HtmlElement* element) {
  const HtmlElement::AttributeList& attrs = element->attributes();
  for (HtmlElement::AttributeConstIterator i(attrs.begin());
       i != attrs.end(); ++i) {
    const HtmlElement::Attribute& attribute = *i;
    StringPiece value = attribute.DecodedValueOrNull();
    if (value.empty()) {
      continue;
    }
    // Get all items in the map with matching attribute name.
    // TODO(sriharis):  We need case insensitive compare here.
    typedef AttributesToNonCacheableValuesMap::const_iterator Iterator;
    std::pair<Iterator, Iterator> ret =
        attribute_non_cacheable_values_map.equal_range(
            attribute.name().c_str());

    if (attribute.name().keyword() == HtmlName::kClass) {
      // Split class attribute value on whitespace.
      StringPieceVector value_vector;
      SplitStringPieceToVector(value, " \r\n\t", &value_vector, true);
      for (Iterator it = ret.first; it != ret.second; ++it) {
        StringPieceVector spec_vector;
        SplitStringPieceToVector(it->second.first, " \t", &spec_vector, true);
        // If spec_vector is a subset of value_vector return the index
        // (it->second.second).
        if (IsAllIncludedIn(spec_vector, value_vector)) {
          return it->second.second;
        }
      }
    } else {
      for (Iterator it = ret.first; it != ret.second; ++it) {
        if (value == it->second.first) {
          // Returning the index.
          return it->second.second;
        }
      }
    }
  }
  return -1;
}

GoogleString GetPanelId(int panel_number, int instance_number) {
  return StrCat(BlinkUtil::kPanelId, "-", IntegerToString(panel_number),
                ".", IntegerToString(instance_number));
}

}  // namespace BlinkUtil
}  // namespace net_instaweb
