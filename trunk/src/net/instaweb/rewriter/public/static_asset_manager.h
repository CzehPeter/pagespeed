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

// Author: guptaa@google.com (Ashish Gupta)

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_STATIC_ASSET_MANAGER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_STATIC_ASSET_MANAGER_H_

#include <map>
#include <vector>

#include "net/instaweb/rewriter/static_asset_enum.pb.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class Hasher;
class HtmlElement;
class MessageHandler;
class RewriteDriver;
class RewriteOptions;
struct ContentType;

// Composes URLs for the javascript files injected by the various PSA filters.
// TODO(ksimbili): Refactor out the common base class to serve the static files
// of type css, images or html etc.
// TODO(xqyin): Refactor out StaticAssetManager to have shared infrastructure
// used by both RewriteStaticAssetManager and SystemStaticAssetManager. Now the
// JS files in system/ are done directly in AdminSite.
class StaticAssetManager {
 public:
  static const char kGStaticBase[];
  static const char kDefaultLibraryUrlPrefix[];

  StaticAssetManager(const GoogleString& static_asset_base,
                     Hasher* hasher,
                     MessageHandler* message_handler);

  ~StaticAssetManager();

  // Returns the url based on the value of debug filter and the value of
  // serve_asset_from_gstatic flag.
  const GoogleString& GetAssetUrl(StaticAssetEnum::StaticAsset module,
                                  const RewriteOptions* options) const;

  // Returns the contents of the asset.
  const char* GetAsset(StaticAssetEnum::StaticAsset module,
                       const RewriteOptions* options) const;

  // Get the asset to be served as external file for the file names file_name.
  // The snippet is returned as 'content' and cache-control headers is set into
  // cache_header. If the hash matches, then ttl is set to 1 year, or else set
  // to 'private max-age=300'.
  // Returns true iff the content for filename is found.
  bool GetAsset(StringPiece file_name, StringPiece* content,
                ContentType* content_type, StringPiece* cache_header) const;

  // Add a CharacterNode to an already created script element, properly escaping
  // the text with CDATA tags is necessary. The script element should be added
  // already, say with a call to InsertNodeBeforeNode.
  void AddJsToElement(StringPiece js, HtmlElement* script,
                      RewriteDriver* driver) const;


  // If set_serve_asset_from_gstatic is true, update the URL for module to use
  // gstatic.
  void set_gstatic_hash(StaticAssetEnum::StaticAsset module,
                        const GoogleString& gstatic_base,
                        const GoogleString& hash);

  // Set serve_asset_from_gstatic_ to serve the files from gstatic. Note that
  // files won't actually get served from gstatic until you also call
  // set_gstatic_hash for the URL that you'd like served from gstatic.
  // set_gstatic_hash should be called after calling
  // set_server_asset_from_gstatic(true).
  void set_serve_asset_from_gstatic(bool serve_asset_from_gstatic) {
    serve_asset_from_gstatic_ = serve_asset_from_gstatic;
  }

  // Set the prefix for the URLs of assets.
  void set_library_url_prefix(const StringPiece& url_prefix) {
    url_prefix.CopyToString(&library_url_prefix_);
    InitializeAssetUrls();
  }

  void set_static_asset_base(const StringPiece& x) {
    x.CopyToString(&static_asset_base_);
    InitializeAssetUrls();
  }

 private:
  class Asset;

  typedef std::map<GoogleString, StaticAssetEnum::StaticAsset>
      FileNameToModuleMap;

  void InitializeAssetStrings();
  void InitializeAssetUrls();

  GoogleString static_asset_base_;
  // Set in the constructor, this class does not own the following objects.
  Hasher* hasher_;
  MessageHandler* message_handler_;

  std::vector<Asset*> assets_;
  FileNameToModuleMap file_name_to_module_map_;

  bool serve_asset_from_gstatic_;
  GoogleString library_url_prefix_;
  GoogleString cache_header_with_long_ttl_;
  GoogleString cache_header_with_private_ttl_;

  DISALLOW_COPY_AND_ASSIGN(StaticAssetManager);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_STATIC_ASSET_MANAGER_H_
