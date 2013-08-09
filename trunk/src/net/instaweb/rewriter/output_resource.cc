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
//         jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/rewriter/public/output_resource.h"

#include <cstddef>

#include "base/logging.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/resource_namer.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/filename_encoder.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/named_lock_manager.h"
#include "net/instaweb/util/public/proto_util.h"
#include "net/instaweb/util/public/queued_worker_pool.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string_writer.h"

namespace net_instaweb {

namespace {

// Helper to allow us to use synchronous caches synchronously even with
// asynchronous interface, until we're changed to be fully asynchronous.
class SyncCallback : public CacheInterface::Callback {
 public:
  SyncCallback() : called_(false), state_(CacheInterface::kNotFound) {
  }

  virtual void Done(CacheInterface::KeyState state) {
    called_ = true;
    state_ = state;
  }

  bool called_;
  CacheInterface::KeyState state_;
};

}  // namespace

OutputResource::OutputResource(ServerContext* server_context,
                               const StringPiece& resolved_base,
                               const StringPiece& unmapped_base,
                               const StringPiece& original_base,
                               const ResourceNamer& full_name,
                               const RewriteOptions* options,
                               OutputResourceKind kind)
    : Resource(server_context, NULL /* no type yet*/),
      writing_complete_(false),
      cached_result_owned_(false),
      cached_result_(NULL),
      resolved_base_(resolved_base.data(), resolved_base.size()),
      unmapped_base_(unmapped_base.data(), unmapped_base.size()),
      original_base_(original_base.data(), original_base.size()),
      rewrite_options_(options),
      kind_(kind) {
  DCHECK(options != NULL);
  full_name_.CopyFrom(full_name);
  CHECK(EndsInSlash(resolved_base)) <<
      "resolved_base must end in a slash, was: " << resolved_base;
  set_enable_cache_purge(options->enable_cache_purge());
  set_proactive_resource_freshening(options->proactive_resource_freshening());
}

OutputResource::~OutputResource() {
  clear_cached_result();
}

void OutputResource::DumpToDisk(MessageHandler* handler) {
  GoogleString file_name = DumpFileName();
  FileSystem* file_system = server_context_->file_system();
  FileSystem::OutputFile* output_file =
      file_system->OpenOutputFile(file_name.c_str(), handler);
  if (output_file == NULL) {
    handler->Message(kWarning, "Unable to open dump file: %s",
                     file_name.c_str());
    return;
  }

  // Serialize headers.
  GoogleString headers;
  StringWriter string_writer(&headers);
  response_headers_.WriteAsHttp(&string_writer, handler);
  bool ok_headers = output_file->Write(headers, handler);

  // Serialize payload.
  bool ok_body = output_file->Write(contents(), handler);

  if (!ok_headers || !ok_body) {
    handler->Message(kWarning,
                     "Error writing dump file: %s", file_name.c_str());
  }

  file_system->Close(output_file, handler);
}

Writer* OutputResource::BeginWrite(MessageHandler* handler) {
  value_.Clear();
  full_name_.ClearHash();
  computed_url_.clear();  // Since dependent on full_name_.
  CHECK(!writing_complete_);
  return &value_;
}

void OutputResource::EndWrite(MessageHandler* handler) {
  CHECK(!writing_complete_);
  value_.SetHeaders(&response_headers_);
  Hasher* hasher = server_context_->hasher();
  full_name_.set_hash(hasher->Hash(contents()));
  computed_url_.clear();  // Since dependent on full_name_.
  writing_complete_ = true;
  DropCreationLock();
}

StringPiece OutputResource::suffix() const {
  CHECK(type_ != NULL);
  return type_->file_extension();
}

GoogleString OutputResource::DumpFileName() const {
  GoogleString filename;
  server_context_->filename_encoder()->Encode(
      server_context_->filename_prefix(), url(), &filename);
  return filename;
}

GoogleString OutputResource::name_key() const {
  GoogleString id_name = full_name_.EncodeIdName();
  GoogleString result;
  CHECK(!resolved_base_.empty());  // Corresponding path in url() is dead code
  result = StrCat(resolved_base_, id_name);
  return result;
}

// TODO(jmarantz): change the name to reflect the fact that it is not
// just an accessor now.
GoogleString OutputResource::url() const {
  // Computing our URL is relatively expensive and it can be set externally,
  // so we compute it the first time we're called and cache the result;
  // computed_url_ is declared mutable.
  if (computed_url_.empty()) {
    computed_url_ = server_context()->url_namer()->Encode(
        rewrite_options_, *this, UrlNamer::kSharded);
  }
  return computed_url_;
}

GoogleString OutputResource::HttpCacheKey() const {
  GoogleString canonical_url =
      server_context()->url_namer()->Encode(rewrite_options_, *this,
                                            UrlNamer::kUnsharded);
  GoogleString mapped_domain_name;
  GoogleUrl resolved_request;
  const DomainLawyer* lawyer = rewrite_options()->domain_lawyer();

  // MapRequestToDomain needs a base URL, which ought to be irrelevant here,
  // as we're already absolute.
  GoogleUrl base(canonical_url);
  if (base.is_valid() &&
      lawyer->MapRequestToDomain(
          base, canonical_url, &mapped_domain_name, &resolved_request,
          server_context()->message_handler())) {
    resolved_request.Spec().CopyToString(&canonical_url);
  }
  return canonical_url;
}

GoogleString OutputResource::UrlEvenIfHashNotSet() {
  GoogleString result;
  if (!has_hash()) {
    full_name_.set_hash("0");
    result = server_context()->url_namer()->Encode(
        rewrite_options_, *this, UrlNamer::kSharded);
    full_name_.ClearHash();
  } else {
    result = url();
  }
  return result;
}

void OutputResource::SetHash(const StringPiece& hash) {
  CHECK(!writing_complete_);
  CHECK(!has_hash());
  full_name_.set_hash(hash);
  computed_url_.clear();  // Since dependent on full_name_.
}

void OutputResource::LoadAndCallback(NotCacheablePolicy not_cacheable_policy,
                                     const RequestContextPtr& request_context,
                                     AsyncCallback* callback) {
  DCHECK(false) << "Output resources shouldn't be loaded via "
                   "LoadAsync, but rather through FetchResource";
  callback->Done(false /* lock_failure */, writing_complete_);
}

GoogleString OutputResource::decoded_base() const {
  GoogleUrl gurl(url());
  GoogleString decoded_url;
  if (server_context()->url_namer()->Decode(gurl, NULL, &decoded_url)) {
    gurl.Reset(decoded_url);
  }
  return gurl.AllExceptLeaf().as_string();
}

void OutputResource::SetType(const ContentType* content_type) {
  Resource::SetType(content_type);
  if (content_type != NULL) {
    // TODO(jmaessen): The addition of 1 below avoids the leading ".";
    // make this convention consistent and fix all code.
    full_name_.set_ext(content_type->file_extension() + 1);
    computed_url_.clear();  // Since dependent on full_name_.
    DCHECK_LE(full_name_.ext().size(),
              static_cast<size_t>(ContentType::MaxProducedExtensionLength()))
        << "OutputResource with extension length > "
           "ContentType::MaxProducedExtensionLength()";
  }
}

NamedLock* OutputResource::CreationLock() {
  if (creation_lock_.get() == NULL) {
    creation_lock_.reset(server_context_->MakeCreationLock(name_key()));
  }
  return creation_lock_.get();
}

bool OutputResource::has_lock() const {
  return ((creation_lock_.get() != NULL) && creation_lock_->Held());
}

bool OutputResource::TryLockForCreation() {
  if (has_lock()) {
    return true;
  } else {
    return server_context_->TryLockForCreation(CreationLock());
  }
}

void OutputResource::LockForCreation(QueuedWorkerPool::Sequence* worker,
                                     Function* callback) {
  if (has_lock()) {
    worker->Add(callback);
  } else {
    server_context_->LockForCreation(CreationLock(), worker, callback);
  }
}

void OutputResource::DropCreationLock() {
  creation_lock_.reset();
}

CachedResult* OutputResource::EnsureCachedResultCreated() {
  if (cached_result_ == NULL) {
    clear_cached_result();
    cached_result_ = new CachedResult();
    cached_result_owned_ = true;
  } else {
    DCHECK(!cached_result_->frozen()) << "Cannot mutate frozen cached result";
  }
  return cached_result_;
}

void OutputResource::UpdateCachedResultPreservingInputInfo(
    CachedResult* to_update) const {
  // TODO(sligocki): Fix this so that the *cached_result() does have inputs set.
  protobuf::RepeatedPtrField<InputInfo> temp;
  temp.Swap(to_update->mutable_input());
  *to_update = *cached_result();
  temp.Swap(to_update->mutable_input());
}

void OutputResource::clear_cached_result() {
  if (cached_result_owned_) {
    delete cached_result_;
    cached_result_owned_ = false;
  }
  cached_result_ = NULL;
}

}  // namespace net_instaweb
