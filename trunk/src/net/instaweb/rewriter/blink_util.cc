// Copyright 2011 Google Inc. All Rights Reserved.
// Author: gagansingh@google.com (Gagan Singh)

#include "net/instaweb/rewriter/public/blink_util.h"

#include "net/instaweb/rewriter/panel_config.pb.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/wildcard.h"

namespace net_instaweb {
namespace BlinkUtil {

const Layout* FindLayout(const PublisherConfig& config,
                         const GoogleUrl& request_url) {
  for (int i = 0; i < config.layout_size(); ++i) {  // Typically 3-4 layouts.
    const Layout& layout = config.layout(i);
    if (layout.reference_page_url_path() == request_url.PathAndLeaf()) {
      return &layout;
    }
    for (int j = 0; j < layout.relative_url_patterns_size(); ++j) {
      Wildcard wildcard(layout.relative_url_patterns(j));
      if (wildcard.Match(request_url.PathAndLeaf())) {
        return &layout;
      }
    }
  }

  return NULL;
}

void SplitCritical(const Json::Value& complete_json,
                   const PanelIdToSpecMap& panel_id_to_spec,
                   GoogleString* critical_json_str,
                   GoogleString* non_critical_json_str,
                   GoogleString* pushed_images_str) {
  Json::Value critical_json(Json::arrayValue);
  Json::Value non_cacheable_critical_json(Json::arrayValue);
  Json::Value non_critical_json(Json::arrayValue);
  Json::Value pushed_images(Json::objectValue);

  Json::Value panel_json(complete_json);
  panel_json[0].removeMember(kInstanceHtml);

  SplitCriticalArray(panel_json, panel_id_to_spec, &critical_json,
                     &non_cacheable_critical_json, &non_critical_json,
                     true, 1, &pushed_images);
  critical_json = critical_json.empty() ? Json::objectValue : critical_json[0];

  Json::FastWriter fast_writer;
  *critical_json_str = fast_writer.write(critical_json);
  BlinkUtil::StripTrailingNewline(critical_json_str);

  DeleteImagesFromJson(&non_critical_json);
  non_critical_json =
      non_critical_json.empty() ? Json::objectValue : non_critical_json[0];
  *non_critical_json_str = fast_writer.write(non_critical_json);
  BlinkUtil::StripTrailingNewline(non_critical_json_str);

  *pushed_images_str = fast_writer.write(pushed_images);
  BlinkUtil::StripTrailingNewline(pushed_images_str);
}

// complete_json = [panel1, panel2 ... ]
// panel = {
//   "instanceHtml": "html of panel",
//   "images": {"img1:<lowres>", "img2:<lowres>"} (images inside instanceHtml)
//   "panel-id.0": <complete_json>,
//   "panel-id.1": <complete_json>,
// }
//
// CRITICAL = [panel1]
// NON-CACHEABLE = [Empty panel, panel2]
// NON-CRITICAL = [Empty panel, Empty panel, panel3]
//
// TODO(ksimbili): Support images inling for non_cacheable too.
void SplitCriticalArray(const Json::Value& complete_json,
                        const PanelIdToSpecMap& panel_id_to_spec,
                        Json::Value* critical_json,
                        Json::Value* critical_non_cacheable_json,
                        Json::Value* non_critical_json,
                        bool panel_cacheable,
                        int num_critical_instances,
                        Json::Value* pushed_images) {
  DCHECK(pushed_images);
  num_critical_instances = std::min(num_critical_instances,
                                    static_cast<int>(complete_json.size()));

  for (int i = 0; i < num_critical_instances; ++i) {
    Json::Value instance_critical(Json::objectValue);
    Json::Value instance_non_cacheable_critical(Json::objectValue);
    Json::Value instance_non_critical(Json::objectValue);

    SplitCriticalObj(complete_json[i], panel_id_to_spec, &instance_critical,
                     &instance_non_cacheable_critical,
                     &instance_non_critical,
                     panel_cacheable,
                     pushed_images);
    critical_json->append(instance_critical);
    critical_non_cacheable_json->append(instance_non_cacheable_critical);
    non_critical_json->append(instance_non_critical);
  }

  for (Json::ArrayIndex i = num_critical_instances; i < complete_json.size();
      ++i) {
    non_critical_json->append(complete_json[i]);
  }

  ClearArrayIfAllEmpty(critical_json);
  ClearArrayIfAllEmpty(critical_non_cacheable_json);
  ClearArrayIfAllEmpty(non_critical_json);
}

void SplitCriticalObj(const Json::Value& json_obj,
                      const PanelIdToSpecMap& panel_id_to_spec,
                      Json::Value* critical_obj,
                      Json::Value* non_cacheable_obj,
                      Json::Value* non_critical_obj,
                      bool panel_cacheable,
                      Json::Value* pushed_images) {
  const std::vector<std::string>& keys = json_obj.getMemberNames();
  for (Json::ArrayIndex j = 0; j < keys.size(); ++j) {
    const std::string& key = keys[j];

    if (key == kContiguous) {
      (*critical_obj)[kContiguous] = json_obj[key];
      (*non_cacheable_obj)[kContiguous] = json_obj[key];
      (*non_critical_obj)[kContiguous] = json_obj[key];
      continue;
    }

    if (key == kInstanceHtml) {
      if (panel_cacheable) {
        (*critical_obj)[kInstanceHtml] = json_obj[key];
      } else {
        (*non_cacheable_obj)[kInstanceHtml] = json_obj[key];
      }
      continue;
    }

    if (key == kImages) {
      if (panel_cacheable) {
        const Json::Value& image_obj = json_obj[key];
        const std::vector<std::string>& image_keys = image_obj.getMemberNames();
        for (Json::ArrayIndex k = 0; k < image_keys.size(); ++k) {
          const std::string& image_url = image_keys[k];
          (*pushed_images)[image_url] = image_obj[image_url];
        }
      }
      continue;
    }

    if (panel_id_to_spec.find(key) == panel_id_to_spec.end()) {
      LOG(DFATAL) << "SplitCritical called with invalid Panelid: " << key;
      continue;
    }
    const Panel& child_panel = *((panel_id_to_spec.find(key))->second);

    Json::Value child_critical(Json::arrayValue);
    Json::Value child_non_cacheable_critical(Json::arrayValue);
    Json::Value child_non_critical(Json::arrayValue);
    bool child_panel_cacheable = panel_cacheable &&
        (child_panel.cacheability_in_minutes() != 0);
    SplitCriticalArray(json_obj[key],
                       panel_id_to_spec,
                       &child_critical,
                       &child_non_cacheable_critical,
                       &child_non_critical,
                       child_panel_cacheable,
                       child_panel.num_critical_instances(),
                       pushed_images);

    if (!child_critical.empty()) {
      (*critical_obj)[key] = child_critical;
    }
    if (!child_non_cacheable_critical.empty()) {
      (*non_cacheable_obj)[key] = child_non_cacheable_critical;
    }
    if (!child_non_critical.empty()) {
      (*non_critical_obj)[key] = child_non_critical;
    }
  }
}

void ClearArrayIfAllEmpty(Json::Value* json) {
  for (Json::ArrayIndex i = 0; i < json->size(); ++i) {
    if (!(*json)[i].isMember(kContiguous)) {
      LOG(DFATAL) << "No Contiguous member in Json";
      return;
    }
    // 'contiguous' is added to every json by default
    if ((*json)[i].size() > 1) {
      return;
    }
  }
  json->clear();
}

void DeleteImagesFromJson(Json::Value* complete_json) {
  for (Json::ArrayIndex i = 0; i < complete_json->size(); ++i) {
    const std::vector<std::string>& keys = (*complete_json)[i].getMemberNames();
    for (Json::ArrayIndex j = 0; j < keys.size(); ++j) {
      const std::string& key = keys[j];
      if (key == kImages) {
        (*complete_json)[i].removeMember(key);
      } else if (key != kInstanceHtml) {
        DeleteImagesFromJson(&(*complete_json)[i][key]);
      }
    }
  }
}

bool ComputePanels(const PanelSet* panel_set_,
                   PanelIdToSpecMap* panel_id_to_spec) {
  bool non_cacheable_present = false;
  for (int i = 0; i < panel_set_->panels_size(); ++i) {
    const Panel& panel = panel_set_->panels(i);
    const GoogleString panel_id = StrCat(kPanelId, ".", IntegerToString(i));
    non_cacheable_present |= (panel.cacheability_in_minutes() == 0);
    (*panel_id_to_spec)[panel_id] = &panel;
  }
  return non_cacheable_present;
}

void EscapeString(GoogleString* str) {
  *str = BlinkUtil::StringReplace(*str, "<", "__psa_lt;", true);
  *str = BlinkUtil::StringReplace(*str, ">", "__psa_gt;", true);
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

GoogleString StringReplace(const StringPiece& s, const StringPiece& oldsub,
                     const StringPiece& newsub, bool replace_all) {
  GoogleString ret;
  BlinkUtil::StringReplace(s, oldsub, newsub, replace_all, &ret);
  return ret;
}

void StringReplace(const StringPiece& s, const StringPiece& oldsub,
                   const StringPiece& newsub, bool replace_all,
                   GoogleString* res) {
  if (oldsub.empty()) {
    res->append(s.data(), s.length());  // If empty, append the given string.
    return;
  }

  StringPiece::size_type start_pos = 0;
  StringPiece::size_type pos;
  do {
    pos = s.find(oldsub, start_pos);
    if (pos == StringPiece::npos) {
      break;
    }
    res->append(s.data() + start_pos, pos - start_pos);
    res->append(newsub.data(), newsub.length());
    // Start searching again after the "old".
    start_pos = pos + oldsub.length();
  } while (replace_all);
  res->append(s.data() + start_pos, s.length() - start_pos);
}

}  // namespace PanelUtil
}  // namespace net_instaweb
