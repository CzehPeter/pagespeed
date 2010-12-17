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

// Author: sligocki@google.com (Shawn Ligocki)
//
// Output resources are created by a ResourceManager. They must be able to
// write contents and return their url (so that it can be href'd on a page).

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_OUTPUT_RESOURCE_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_OUTPUT_RESOURCE_H_

#include "base/basictypes.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/file_writer.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_namer.h"

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
  OutputResource(ResourceManager* manager,
                 const StringPiece& resolved_base,
                 const ResourceNamer& resource_id,
                 const ContentType* type);
  ~OutputResource();

  virtual bool Load(MessageHandler* message_handler);
  virtual std::string url() const;

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
  // The hash_ext describes the hash and content type of the resource;
  // to index already-computed resources we lookup name_key() and obtain
  // the corresponding hash_ext().
  virtual std::string hash_ext() const;

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

  // Determines whether the output resource has a valid URL.  If so,
  // we don't need to actually load the output-resource content from
  // cache during the Rewriting process -- we can immediately rewrite
  // the href to it.
  //
  // Note that when serving content, we must actually load it, but
  // when rewriting it we can, in some cases, exploit a URL swap.
  bool HasValidUrl() const { return has_hash(); }

  // Resources rewritten via a UrlPartnership will have a resolved
  // base to use in lieu of the legacy UrlPrefix held by the resource
  // manager.
  void set_resolved_base(const StringPiece& base) {
    base.CopyToString(&resolved_base_);
    CHECK(EndsInSlash(base)) << "resolved_base must end in a slash.";
  }

 private:
  friend class ResourceManager;
  friend class ResourceManagerTestingPeer;
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
  StringPiece hash() const { return full_name_.hash(); }
  bool has_hash() const { return !hash().empty(); }
  void set_written(bool written) { writing_complete_ = true; }
  void set_generated(bool x) { generated_ = x; }
  bool generated() const { return generated_; }
  std::string TempPrefix() const;

  OutputWriter* BeginWrite(MessageHandler* message_handler);
  bool EndWrite(OutputWriter* writer, MessageHandler* message_handler);
  // Attempt to obtain a named lock for the resource.  Return true if we do so.
  bool LockForCreation(const ResourceManager* resource_manager,
                       ResourceManager::BlockingBehavior block);

  FileSystem::OutputFile* output_file_;
  bool writing_complete_;

  // Generated via ResourceManager::CreateGeneratedOutputResource,
  // meaning that it does not have a name that is derived from an
  // input URL.  We must regenerate it every time, but the output name
  // will be distinct because it's based on the hash of the content.
  bool generated_;

  // If this output url was created via a partnership then this field
  // will be non-empty, and we will not need to use the resource manager's
  // prefix.
  std::string resolved_base_;
  ResourceNamer full_name_;

  // Lock guarding resource creation.  Lazily initialized by LockForCreation,
  // unlocked on destruction or EndWrite.
  scoped_ptr<AbstractLock> creation_lock_;

  DISALLOW_COPY_AND_ASSIGN(OutputResource);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_OUTPUT_RESOURCE_H_
