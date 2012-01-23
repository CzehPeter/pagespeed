/*
 * Copyright 2011 Google Inc.
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

// Author: nikhilmadan@google.com (Nikhil Madan)

#ifndef NET_INSTAWEB_AUTOMATIC_PUBLIC_BLINK_FLOW_H_
#define NET_INSTAWEB_AUTOMATIC_PUBLIC_BLINK_FLOW_H_

#include "net/instaweb/rewriter/public/blink_util.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class AsyncFetch;
class Layout;
class ProxyFetchFactory;
class ResponseHeaders;
class RewriteOptions;

// This class manages the blink flow for looking up the json in cache,
// modifying the options for passthru and triggering asynchronous lookups to
// compute the json and insert it into cache.
class BlinkFlow {
 public:
  static void Start(const GoogleString& url,
                    AsyncFetch* base_fetch,
                    const Layout* layout,
                    RewriteOptions* options,
                    ProxyFetchFactory* factory,
                    ResourceManager* manager);

  virtual ~BlinkFlow();

  static void Initialize(Statistics* statistics);

  // Statistics variable names.
  static const char kNumBackgroundFetchesStarted[];
  static const char kNumBackgroundFetchesComplete[];

 private:
  class JsonFindCallback;
  friend class JsonFindCallback;

  // TODO(rahulbansal): Move this to cc file.
  BlinkFlow(const GoogleString& url,
            AsyncFetch* base_fetch,
            const Layout* layout,
            RewriteOptions* options,
            ProxyFetchFactory* factory,
            ResourceManager* manager)
      : url_(url),
        base_fetch_(base_fetch),
        layout_(layout),
        options_(options),
        factory_(factory),
        manager_(manager),
        request_start_time_ms_(-1),
        time_to_start_blink_flow_ms_(-1),
        time_to_json_lookup_done_ms_(-1),
        time_to_split_critical_ms_(-1),
        num_background_fetches_started_(NULL) {
    Statistics* stats = manager_->statistics();
    if (stats != NULL) {
      num_background_fetches_started_ = stats->GetTimedVariable(
          kNumBackgroundFetchesStarted);
    }
  }

  void InitiateJsonLookup();

  void JsonCacheHit(const StringPiece& content,
                    const ResponseHeaders& headers);

  void JsonCacheMiss();

  void TriggerProxyFetch(bool layout_found);

  void ComputeJsonInBackground();

  void ServeAllPanelContents(const Json::Value& json,
                             const PanelIdToSpecMap& panel_id_to_spec);

  void ServeCriticalPanelContents(const Json::Value& json,
                                  const PanelIdToSpecMap& panel_id_to_spec);

  void SendLayout(const StringPiece& str);

  void SendCriticalJson(GoogleString* critical_json_str);

  void SendInlineImagesJson(GoogleString* pushed_images_str);

  void SendNonCriticalJson(GoogleString* non_critical_json_str);

  void EscapeString(GoogleString* str) const;

  void WriteString(const StringPiece& str);

  void Flush();

  int64 GetTimeElapsedFromStartRequest();

  GoogleString GetAddTimingScriptString(const GoogleString& timing_str,
                                        int64 time_ms);

  GoogleString url_;
  AsyncFetch* base_fetch_;
  const Layout* layout_;
  RewriteOptions* options_;
  ProxyFetchFactory* factory_;
  ResourceManager* manager_;
  GoogleString json_url_;
  int64 request_start_time_ms_;
  int64 time_to_start_blink_flow_ms_;
  int64 time_to_json_lookup_done_ms_;
  int64 time_to_split_critical_ms_;
  TimedVariable* num_background_fetches_started_;

  DISALLOW_COPY_AND_ASSIGN(BlinkFlow);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_AUTOMATIC_PUBLIC_BLINK_FLOW_H_
