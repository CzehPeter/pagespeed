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

// Author: sligocki@google.com (Shawn Ligocki)
//
// Output resources are created by a ResourceManager. They must be able to
// write contents and return their url (so that it can be href'd on a page).

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_OUTPUT_RESOURCE_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_OUTPUT_RESOURCE_H_

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/file_writer.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_namer.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"

namespace net_instaweb {

class AbstractLock;
class MessageHandler;
class NamedLockManager;

class OutputResource : public Resource {
 public:

  // Construct an OutputResource.  For the moment, we pass in type redundantly
  // even though full_name embeds an extension.  This reflects current code
  // structure rather than a principled stand on anything.
  // TODO(jmaessen): remove redundancy.
  //
  // The 'options' argument can be NULL.  This is done in the Fetch path because
  // that field is only used for domain sharding, and during the fetch, further
  // domain makes no sense.
  OutputResource(RewriteDriver* driver,
                 const StringPiece& resolved_base,
                 const ResourceNamer& resource_id,
                 const ContentType* type,
                 const RewriteOptions* options);
  ~OutputResource();

  virtual bool Load(MessageHandler* message_handler);
  virtual std::string url() const;

  // Attempt to obtain a named lock for the resource.  Return true if we do so.
  // If the resource is expensive to create, this lock should be held during
  // its creation to avoid multiple rewrites happening at once.
  // The lock will be unlocked on destruction or EndWrite (called from
  // ResourceManager::Write)
  bool LockForCreation(const ResourceManager* resource_manager,
                       ResourceManager::BlockingBehavior block);

  // The NameKey describes the source url and rewriter used, without hash and
  // content type information.  This is used to find previously-computed filter
  // results whose output hash and content type is unknown.  The full name of a
  // resource is of the form
  //    path/prefix.encoded_resource_name.hash.extension
  // we know prefix and name, but not the hash, and we don't always even have
  // the extension, which might have changes as the result of, for example image
  // optimization (e.g. gif->png).  But We can "remember" the hash/extension for
  // as long as the origin URL was cacheable.  So we construct this as a key:
  //    path/prefix.encoded_resource_name
  // and use that to map to the hash-code and extension.  If we know the
  // hash-code then we may also be able to look up the contents in the same
  // cache.
  virtual std::string name_key() const;

  // output-specific
  const std::string& resolved_base() const { return resolved_base_; }
  const ResourceNamer& full_name() const { return full_name_; }
  StringPiece name() const { return full_name_.name(); }
  std::string filename() const;
  StringPiece suffix() const;
  StringPiece filter_prefix() const { return full_name_.id(); }

  // In a scalable installation where the sprites must be kept in a
  // database, we cannot serve HTML that references new resources
  // that have not been committed yet, and committing to a database
  // may take too long to block on the HTML rewrite.  So we will want
  // to refactor this to check to see whether the desired resource is
  // already known.  For now we'll assume we can commit to serving the
  // resource during the HTML rewriter.
  bool IsWritten() const;

  // Sets the suffix for an output resource.  This must be called prior
  // to Write if the content_type ctor arg was NULL.  This can happen if
  // we are managing a resource whose content-type is not known to us.
  // CacheExtender is currently the only place where we need this.
  void set_suffix(const StringPiece& ext);

  // Sets the type of the output resource, and thus also its suffix.
  virtual void SetType(const ContentType* type);

  // Whenever output resources are created via ResourceManager
  // (except CreateOutputResourceForFetch) it looks up cached
  // information on any previous creation of that resource, including
  // the full filename and any filter-specific metadata. If such
  // information is available, this method will return non-NULL.
  //
  // Note: cached_result() will also be non-NULL if you explicitly
  // create the result from a filter by calling EnsureCachedResultCreated()
  //
  // The output is const because we do not check that the CachedResult has not
  // been written.  If you want to modify the CachedResult, use
  // EnsureCachedResultCreated instead.
  const CachedResult* cached_result() const { return cached_result_.get(); }

  // If there is no cached output information, creates an empty one,
  // without any information filled in (so no url(), or timestamps).
  //
  // The primary use of this method is to let filters store any metadata they
  // want before calling ResourceManager::Write.
  // This never returns null.
  // We will DCHECK that the cached result has not been written.
  CachedResult* EnsureCachedResultCreated() {
    if (cached_result() == NULL) {
      cached_result_.reset(new CachedResult());
    } else {
      DCHECK(!cached_result_.get()->frozen())
          << "Cannot mutate frozen cached result.";
    }
    return cached_result_.get();
  }

  // Transfers up ownership of any cached result and clears pointer to it.
  CachedResult* ReleaseCachedResult() { return cached_result_.release(); }

  // Resources rewritten via a UrlPartnership will have a resolved
  // base to use in lieu of the legacy UrlPrefix held by the resource
  // manager.
  void set_resolved_base(const StringPiece& base) {
    base.CopyToString(&resolved_base_);
    CHECK(EndsInSlash(base)) << "resolved_base must end in a slash.";
  }

 private:
  friend class ResourceManager;
  friend class ResourceManagerTest;
  friend class ResourceManagerTestingPeer;
  friend class RewriteDriver;

  class OutputWriter {
   public:
    // file may be null if we shouldn't write to the filesystem.
    OutputWriter(FileSystem::OutputFile* file, HTTPValue* http_value)
        : http_value_(http_value) {
      if (file != NULL) {
        file_writer_.reset(new FileWriter(file));
      } else {
        file_writer_.reset(NULL);
      }
    }

    // Adds the given data to our http_value, and, if
    // non-null, our file.
    bool Write(const StringPiece& data, MessageHandler* handler);
   private:
    scoped_ptr<FileWriter> file_writer_;
    HTTPValue* http_value_;
  };

  void SetHash(const StringPiece& hash);
  StringPiece extension() const { return full_name_.ext(); }
  StringPiece hash() const { return full_name_.hash(); }
  bool has_hash() const { return !hash().empty(); }
  void set_written(bool written) { writing_complete_ = true; }
  void set_outlined(bool x) { outlined_ = x; }
  bool outlined() const { return outlined_; }
  std::string TempPrefix() const;

  OutputWriter* BeginWrite(MessageHandler* message_handler);
  bool EndWrite(OutputWriter* writer, MessageHandler* message_handler);

  // Stores the current state of cached_result in the HTTP cache
  // under the given key.
  // Pre-condition: cached_result() != NULL
  void SaveCachedResult(const std::string& key, MessageHandler* handler);

  // Loads the state of cached_result from the given cached key if possible,
  // and syncs our URL and content type with it. If fails, cached_result
  // will be set to NULL.
  void FetchCachedResult(const std::string& key, MessageHandler* handler);

  FileSystem::OutputFile* output_file_;
  bool writing_complete_;

  // The resource was created by outlining from within HTML and was not
  // derived from an input URL (and is hence not named after one). We must
  // regenerate it every time, but the output name will be distinct because
  // it's based on the hash of the content.
  bool outlined_;

  scoped_ptr<CachedResult> cached_result_;

  // The resolved_base_ is the domain as reported by UrlPartnership.
  // It takes into account domain-mapping via
  // ModPagespeedMapRewriteDomain.  However, the resolved base is
  // not affected by sharding.  Shard-selection is done when url() is called,
  // relying on the content hash.
  std::string resolved_base_;
  ResourceNamer full_name_;

  // Lock guarding resource creation.  Lazily initialized by LockForCreation,
  // unlocked on destruction or EndWrite.
  scoped_ptr<AbstractLock> creation_lock_;

  // rewrite_options_ is NULL when we are creating an output resource on
  // behalf of a fetch.  This is because there's no point or need to implement
  // sharding on the fetch -- we are not rewriting a URL, we are just decoding
  // it.  However, when rewriting a resources, we need rewrite_options_ to
  // be non-null.
  const RewriteOptions* rewrite_options_;

  DISALLOW_COPY_AND_ASSIGN(OutputResource);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_OUTPUT_RESOURCE_H_
