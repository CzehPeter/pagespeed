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

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_value.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/response_headers_parser.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_namer.h"
#include "net/instaweb/rewriter/public/url_namer.h"
#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/file_writer.h"
#include "net/instaweb/util/public/filename_encoder.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/hasher.h"
#include "net/instaweb/util/public/named_lock_manager.h"
#include "net/instaweb/util/public/proto_util.h"
#include "net/instaweb/util/public/queued_worker_pool.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/stack_buffer.h"

namespace net_instaweb {
class MessageHandler;

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

OutputResource::OutputResource(ResourceManager* resource_manager,
                               const StringPiece& resolved_base,
                               const StringPiece& unmapped_base,
                               const StringPiece& original_base,
                               const ResourceNamer& full_name,
                               const ContentType* type,
                               const RewriteOptions* options,
                               OutputResourceKind kind)
    : Resource(resource_manager, type),
      output_file_(NULL),
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
  if (type == NULL) {
    GoogleString ext_with_dot = StrCat(".", full_name.ext());
    type_ = NameExtensionToContentType(ext_with_dot);
  } else {
    // This if + check used to be a 1-liner, but it was failing and this
    // yields debuggable output.
    // TODO(jmaessen): The addition of 1 below avoids the leading ".";
    // make this convention consistent and fix all code.
    CHECK_EQ((type->file_extension() + 1), full_name.ext());
  }
  CHECK(EndsInSlash(resolved_base)) <<
      "resolved_base must end in a slash, was: " << resolved_base;
}

OutputResource::~OutputResource() {
  clear_cached_result();
}

bool OutputResource::OutputWriter::Write(const StringPiece& data,
                                         MessageHandler* handler) {
  bool ret = http_value_->Write(data, handler);
  if (file_writer_.get() != NULL) {
    ret &= file_writer_->Write(data, handler);
  }
  return ret;
}

OutputResource::OutputWriter* OutputResource::BeginWrite(
    MessageHandler* handler) {
  value_.Clear();
  full_name_.ClearHash();
  computed_url_.clear();  // Since dependent on full_name_.
  CHECK(!writing_complete_);
  CHECK(output_file_ == NULL);
  if (resource_manager_->store_outputs_in_file_system()) {
    FileSystem* file_system = resource_manager_->file_system();
    // Always write to a tempfile, so that if we get interrupted in the middle
    // we won't leave a half-baked file in the serving path.
    GoogleString temp_prefix = TempPrefix();
    output_file_ = file_system->OpenTempFile(temp_prefix, handler);
    bool success = (output_file_ != NULL);
    if (success) {
      GoogleString header;
      StringWriter string_writer(&header);
      // Serialize headers.
      response_headers_.WriteAsHttp(&string_writer, handler);
      // It does not make sense to have the headers in the hash.
      // call output_file_->Write directly, rather than going through
      // OutputWriter.
      //
      // TODO(jmarantz): consider refactoring to split out the header-file
      // writing in a different way, e.g. to a separate file.
      success &= output_file_->Write(header, handler);
    }
    OutputWriter* writer = NULL;
    if (success) {
      writer = new OutputWriter(output_file_, &value_);
    }
    return writer;
  } else {
    return new OutputWriter(NULL, &value_);
  };
}

bool OutputResource::EndWrite(OutputWriter* writer, MessageHandler* handler) {
  CHECK(!writing_complete_);
  value_.SetHeaders(&response_headers_);
  Hasher* hasher = resource_manager_->hasher();
  full_name_.set_hash(hasher->Hash(contents()));
  computed_url_.clear();  // Since dependent on full_name_.
  writing_complete_ = true;
  bool ret = true;
  if (output_file_ != NULL) {
    FileSystem* file_system = resource_manager_->file_system();
    CHECK(file_system != NULL);
    GoogleString temp_filename = output_file_->filename();
    ret = file_system->Close(output_file_, handler);

    // Now that we are done writing, we can rename to the filename we
    // really want.
    if (ret) {
      ret = file_system->RenameFile(temp_filename.c_str(), filename().c_str(),
                                    handler);
    }

    // TODO(jmarantz): Consider writing to the HTTP cache as we write the
    // file, so same-process write-then-read never has to read from disk.
    // This is moderately inconvenient because HTTPCache lacks a streaming
    // Put interface.

    output_file_ = NULL;
  }
  DropCreationLock();
  return ret;
}

// Called by FilenameOutputResource::BeginWrite to determine how
// to start writing the tmpfile.
GoogleString OutputResource::TempPrefix() const {
  return StrCat(resource_manager_->filename_prefix(), "temp_");
}

StringPiece OutputResource::suffix() const {
  CHECK(type_ != NULL);
  return type_->file_extension();
}

void OutputResource::set_suffix(const StringPiece& ext) {
  type_ = NameExtensionToContentType(ext);
  if (type_ != NULL) {
    // TODO(jmaessen): The addition of 1 below avoids the leading ".";
    // make this convention consistent and fix all code.
    full_name_.set_ext(type_->file_extension() + 1);
  } else {
    full_name_.set_ext(ext.substr(1));
  }
  computed_url_.clear();  // Since dependent on full_name_.
}

GoogleString OutputResource::filename() const {
  GoogleString filename;
  resource_manager_->filename_encoder()->Encode(
      resource_manager_->filename_prefix(), url(), &filename);
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
    computed_url_ = resource_manager()->url_namer()->Encode(rewrite_options_,
                                                            *this);
  }
  return computed_url_;
}

GoogleString OutputResource::UrlEvenIfLeafInvalid() {
  GoogleString result;
  if (!has_hash()) {
    full_name_.set_hash("0");
    result = resource_manager()->url_namer()->Encode(rewrite_options_, *this);
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

bool OutputResource::Load(MessageHandler* handler) {
  if (!writing_complete_ && resource_manager_->store_outputs_in_file_system() &&
      (kind_ != kOnTheFlyResource)) {
    FileSystem* file_system = resource_manager_->file_system();
    FileSystem::InputFile* file = file_system->OpenInputFile(
        filename().c_str(), handler);
    if (file != NULL) {
      char buf[kStackBufferSize];
      int nread = 0, num_consumed = 0;
      // TODO(jmarantz): this logic is duplicated in util/wget_url_fetcher.cc,
      // consider a refactor to merge it.
      response_headers_.Clear();
      value_.Clear();

      // TODO(jmarantz): convert to binary headers
      ResponseHeadersParser parser(&response_headers_);
      while (!parser.headers_complete() &&
             ((nread = file->Read(buf, sizeof(buf), handler)) != 0)) {
        num_consumed = parser.ParseChunk(StringPiece(buf, nread), handler);
      }
      value_.SetHeaders(&response_headers_);
      writing_complete_ = value_.Write(
          StringPiece(buf + num_consumed, nread - num_consumed),
          handler);
      while (writing_complete_ &&
             ((nread = file->Read(buf, sizeof(buf), handler)) != 0)) {
        writing_complete_ = value_.Write(StringPiece(buf, nread), handler);
      }
      file_system->Close(file, handler);
    }
  }
  return writing_complete_;
}

GoogleString OutputResource::decoded_base() const {
  GoogleUrl gurl(url());
  GoogleString decoded_url;
  if (resource_manager()->url_namer()->Decode(gurl, NULL, &decoded_url)) {
    gurl.Reset(decoded_url);
  }
  return gurl.AllExceptLeaf().as_string();
}

bool OutputResource::IsWritten() const {
  return writing_complete_;
}

void OutputResource::SetType(const ContentType* content_type) {
  Resource::SetType(content_type);
  // TODO(jmaessen): The addition of 1 below avoids the leading ".";
  // make this convention consistent and fix all code.
  full_name_.set_ext(content_type->file_extension() + 1);
  computed_url_.clear();  // Since dependent on full_name_.
}

NamedLock* OutputResource::CreationLock() {
  if (creation_lock_.get() == NULL) {
    creation_lock_.reset(resource_manager_->MakeCreationLock(name_key()));
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
    return resource_manager_->TryLockForCreation(CreationLock());
  }
}

void OutputResource::LockForCreation(QueuedWorkerPool::Sequence* worker,
                                     Function* callback) {
  if (has_lock()) {
    worker->Add(callback);
  } else {
    resource_manager_->LockForCreation(CreationLock(), worker, callback);
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
