// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// TODO(jmarantz): Avoid initiating fetches for resources already in flight.
// The challenge is that we would want to call all the callbacks that indicated
// interest in a particular URL once the callback completed.  Alternatively,
// this could be done in a level above the URL fetcher.

#include "net/instaweb/apache/serf_url_async_fetcher.h"

#include <algorithm>
#include <string>
#include <vector>

#include "apr_atomic.h"
#include "apr_strings.h"
#include "apr_thread_proc.h"
#include "apr_version.h"
#include "net/instaweb/util/public/basictypes.h"
#include "base/scoped_ptr.h"
#include "base/stl_util-inl.h"
#include "net/instaweb/apache/apr_condvar.h"
#include "net/instaweb/apache/apr_mutex.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/response_headers_parser.h"
#include "net/instaweb/public/version.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/pool.h"
#include "net/instaweb/util/public/pool_element.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/public/writer.h"
#include "third_party/serf/src/serf.h"
#include "third_party/serf/src/serf_bucket_util.h"

// Until this fetcher has some mileage on it, it is useful to keep around
// an easy way to turn on lots of debug messages.  But they do get a bit chatty
// when things are working well.
#define SERF_DEBUG(x)

namespace {
const int kBufferSize = 2048;
const char kFetchMethod[] = "GET";
}  // namespace

extern "C" {
  // Declares new functions added to
  // src/third_party/serf/instaweb_context.c
serf_bucket_t* serf_request_bucket_request_create_for_host(
    serf_request_t *request,
    const char *method,
    const char *uri,
    serf_bucket_t *body,
    serf_bucket_alloc_t *allocator, const char* host);

int serf_connection_is_in_error_state(serf_connection_t* connection);

}  // extern "C"

namespace net_instaweb {

const char SerfStats::kSerfFetchRequestCount[] = "serf_fetch_request_count";
const char SerfStats::kSerfFetchByteCount[] = "serf_fetch_bytes_count";
const char SerfStats::kSerfFetchTimeDurationMs[] =
    "serf_fetch_time_duration_ms";
const char SerfStats::kSerfFetchCancelCount[] = "serf_fetch_cancel_count";
const char SerfStats::kSerfFetchActiveCount[] =
    "serf_fetch_active_count";
const char SerfStats::kSerfFetchTimeoutCount[] = "serf_fetch_timeout_count";

std::string GetAprErrorString(apr_status_t status) {
  char error_str[1024];
  apr_strerror(status, error_str, sizeof(error_str));
  return error_str;
}

// TODO(lsong): Move this to a separate file. Necessary?
class SerfFetch : public PoolElement<SerfFetch> {
 public:
  // TODO(lsong): make use of request_headers.
  SerfFetch(const std::string& url,
            const RequestHeaders& request_headers,
            ResponseHeaders* response_headers,
            Writer* fetched_content_writer,
            MessageHandler* message_handler,
            UrlAsyncFetcher::Callback* callback,
            Timer* timer)
      : fetcher_(NULL),
        timer_(timer),
        str_url_(url),
        response_headers_(response_headers),
        parser_(response_headers),
        fetched_content_writer_(fetched_content_writer),
        message_handler_(message_handler),
        callback_(callback),
        pool_(NULL),  // filled in once assigned to a thread, to use its pool.
        bucket_alloc_(NULL),
        connection_(NULL),
        bytes_received_(0),
        fetch_start_ms_(0),
        fetch_end_ms_(0) {
    request_headers_.CopyFrom(request_headers);
  }

  ~SerfFetch() {
    if (connection_ != NULL) {
      serf_connection_close(connection_);
    }
    if (pool_ != NULL) {
      apr_pool_destroy(pool_);
    }
  }

  // Start the fetch. It returns immediately.  This can only be run when
  // locked with fetcher->mutex_.
  bool Start(SerfUrlAsyncFetcher* fetcher);

  const char* str_url() { return str_url_.c_str(); }

  // This must be called while holding SerfUrlAsyncFetcher's mutex_.
  void Cancel() {
    CallCallback(false);
  }

  // Calls the callback supplied by the user.  This needs to happen
  // exactly once.  In some error cases it appears that Serf calls
  // HandleResponse multiple times on the same object.
  //
  // This must be called while holding SerfUrlAsyncFetcher's mutex_.
  void CallCallback(bool success) {
    if (callback_ == NULL) {
      LOG(FATAL) << "BUG: Serf callback more than once on same fetch "
                 << str_url() << " (" << this << ").  Please report this "
                 << "at http://code.google.com/p/modpagespeed/issues/";
    } else {
      UrlAsyncFetcher::Callback* callback = callback_;
      callback_ = NULL;
      response_headers_ = NULL;
      fetched_content_writer_ = NULL;
      callback->Done(success);
      fetch_end_ms_ = timer_->NowMs();
      fetcher_->FetchComplete(this);
    }
  }

  // If last poll of this fetch's connection resulted in an error, clean it up.
  // Must be called after serf_context_run, with fetcher's mutex_ held.
  void CleanupIfError() {
    if ((connection_ != NULL) &&
        serf_connection_is_in_error_state(connection_)) {
      message_handler_->Message(
          kInfo, "Serf cleanup for error'd fetch of: %s", str_url());

      // Close the errant connection here immediately to remove it from
      // the poll set immediately so that other jobs can proceed w/o trouble,
      // rather than waiting for ~SerfFetch.
      serf_connection_close(connection_);
      connection_ = NULL;

      // Do the rest of normal cleanup, including calling Done(false);
      Cancel();
    }
  }

  int64 TimeDuration() const {
    if ((fetch_start_ms_ != 0) && (fetch_end_ms_ != 0)) {
      return fetch_end_ms_ - fetch_start_ms_;
    } else {
      return 0;
    }
  }
  int64 fetch_start_ms() const { return fetch_start_ms_; }

  size_t bytes_received() const { return bytes_received_; }
  MessageHandler* message_handler() { return message_handler_; }

 private:

  // Static functions used in callbacks.
  static apr_status_t ConnectionSetup(
      apr_socket_t* socket, serf_bucket_t **read_bkt, serf_bucket_t **write_bkt,
      void* setup_baton, apr_pool_t* pool) {
    // TODO(morlovich): the serf tests do SSL setup in their equivalent.
    SerfFetch* fetch = static_cast<SerfFetch*>(setup_baton);
    *read_bkt = serf_bucket_socket_create(socket, fetch->bucket_alloc_);
    return APR_SUCCESS;
  }

  static void ClosedConnection(serf_connection_t* conn,
                               void* closed_baton,
                               apr_status_t why,
                               apr_pool_t* pool) {
    SerfFetch* fetch = static_cast<SerfFetch*>(closed_baton);
    if (why != APR_SUCCESS) {
      fetch->message_handler_->Warning(
          fetch->str_url_.c_str(), 0, "Connection close (code=%d %s).",
          why, GetAprErrorString(why).c_str());
    }
    // Connection is closed.
    fetch->connection_ = NULL;
  }

  static serf_bucket_t* AcceptResponse(serf_request_t* request,
                                       serf_bucket_t* stream,
                                       void* acceptor_baton,
                                       apr_pool_t* pool) {
    // Get the per-request bucket allocator.
    serf_bucket_alloc_t* bucket_alloc = serf_request_get_alloc(request);
    // Create a barrier so the response doesn't eat us!
    // From the comment in Serf:
    // ### the stream does not have a barrier, this callback should generally
    // ### add a barrier around the stream before incorporating it into a
    // ### response bucket stack.
    // ... i.e. the passed bucket becomes owned rather than
    // ### borrowed.
    serf_bucket_t* bucket = serf_bucket_barrier_create(stream, bucket_alloc);
    return serf_bucket_response_create(bucket, bucket_alloc);
  }

  static apr_status_t HandleResponse(serf_request_t* request,
                                     serf_bucket_t* response,
                                     void* handler_baton,
                                     apr_pool_t* pool) {
    SerfFetch* fetch = static_cast<SerfFetch*>(handler_baton);
    return fetch->HandleResponse(request, response);
  }

  // The handler MUST process data from the response bucket until the
  // bucket's read function states it would block (APR_STATUS_IS_EAGAIN).
  // The handler is invoked only when new data arrives. If no further data
  // arrives, and the handler does not process all available data, then the
  // system can result in a deadlock around the unprocessed, but read, data.
  apr_status_t HandleResponse(serf_request_t* request,
                              serf_bucket_t* response) {
    apr_status_t status = APR_EGENERAL;

    serf_status_line status_line;
    if ((response != NULL) &&
        ((status = serf_bucket_response_status(response, &status_line))
         == APR_SUCCESS)) {
      if (response_headers_ != NULL) {
        response_headers_->SetStatusAndReason(
            static_cast<HttpStatus::Code>(status_line.code));
        response_headers_->set_major_version(status_line.version / 1000);
        response_headers_->set_minor_version(status_line.version % 1000);
      } else {
        // TODO(jmaessen): Do we ever see duplicate header drops for a single
        // url?  Are we re-parsing headers on re-entry?  If the latter is
        // happening we ought to protect against it.
        LOG(INFO) << "Dropping headers and content for " <<
            str_url() << "(" << this << ") due to request timeout";
      }
      const char* data = NULL;
      apr_size_t len = 0;
      while ((status = serf_bucket_read(response, kBufferSize, &data, &len))
             == APR_SUCCESS || APR_STATUS_IS_EOF(status) ||
             APR_STATUS_IS_EAGAIN(status)) {
        bytes_received_ += len;
        if (len > 0 && fetched_content_writer_ != NULL &&
            !fetched_content_writer_->Write(
                StringPiece(data, len), message_handler_)) {
          status = APR_EGENERAL;
          break;
        }
        if (status != APR_SUCCESS) {
          break;
        }
      }
      // We could read the headers earlier, but then we have to check if we
      // have received the headers.  At EOF of response, we have the headers
      // already. Read them.
      if (APR_STATUS_IS_EOF(status)) {
        status = ReadHeaders(response);
      }
    }
    if (!APR_STATUS_IS_EAGAIN(status) && response_headers_ != NULL) {
      bool success = APR_STATUS_IS_EOF(status);
      CallCallback(success);
    }
    return status;
  }

  apr_status_t ReadHeaders(serf_bucket_t* response) {
    apr_status_t status = APR_SUCCESS;
    serf_bucket_t* headers = serf_bucket_response_get_headers(response);
    const char* data = NULL;
    apr_size_t num_bytes = 0;
    while ((status = serf_bucket_read(headers, kBufferSize, &data, &num_bytes))
           == APR_SUCCESS || APR_STATUS_IS_EOF(status) ||
           APR_STATUS_IS_EAGAIN(status)) {
      if (response_headers_ == NULL) {
        // Don't attempt to parse the headers, as the parser will push data into
        // a deallocated data structure.
      } else if (parser_.headers_complete()) {
        status = APR_EGENERAL;
        message_handler_->Info(str_url_.c_str(), 0,
                               "headers complete but more data coming");
      } else {
        StringPiece str_piece(data, num_bytes);
        apr_size_t parsed_len = parser_.ParseChunk(str_piece, message_handler_);
        if (parsed_len != num_bytes) {
          status = APR_EGENERAL;
          message_handler_->Error(str_url_.c_str(), 0,
                                  "unexpected bytes at end of header");
        }
      }
      if (status != APR_SUCCESS) {
        break;
      }
    }
    if (response_headers_ != NULL &&
        APR_STATUS_IS_EOF(status) && !parser_.headers_complete()) {
      message_handler_->Error(str_url_.c_str(), 0,
                              "eof on incomplete headers code=%d %s",
                              status, GetAprErrorString(status).c_str());
      status = APR_EGENERAL;
    }
    return status;
  }

  // Ensures that a user-agent string is included, and that the mod_pagespeed
  // version is appended.
  void FixUserAgent() {
    // Supply a default user-agent if none is present, and in any case
    // append on a 'serf' suffix.
    GoogleString user_agent;
    StringStarVector v;
    if (request_headers_.Lookup(HttpAttributes::kUserAgent, &v)) {
      for (int i = 0, n = v.size(); i < n; ++i) {
        if (i != 0) {
          user_agent += " ";
        }
        if (v[i] != NULL) {
          user_agent += *(v[i]);
        }
      }
      request_headers_.RemoveAll(HttpAttributes::kUserAgent);
    }
    if (user_agent.empty()) {
      user_agent += "Serf/" SERF_VERSION_STRING;
    }
    StringPiece version(" mod_pagespeed/" MOD_PAGESPEED_VERSION_STRING "-"
                        LASTCHANGE_STRING);
    if (!StringPiece(user_agent).ends_with(version)) {
      user_agent.append(version.data(), version.size());
    }
    request_headers_.Add(HttpAttributes::kUserAgent, user_agent);
  }

  static apr_status_t SetupRequest(serf_request_t* request,
                                   void* setup_baton,
                                   serf_bucket_t** req_bkt,
                                   serf_response_acceptor_t* acceptor,
                                   void** acceptor_baton,
                                   serf_response_handler_t* handler,
                                   void** handler_baton,
                                   apr_pool_t* pool) {
    SerfFetch* fetch = static_cast<SerfFetch*>(setup_baton);
    const char* url_path = apr_uri_unparse(pool, &fetch->url_,
                                           APR_URI_UNP_OMITSITEPART);

    // If there is an explicit Host header, then override the
    // host field in the Serf structure, as we will not be able
    // to override it after it is created; only append to it.
    //
    // Serf automatically populates the Host field based on the
    // URL, and provides no mechanism to override it, except
    // by hacking source.  We hacked source.
    //
    // See src/third_party/serf/src/instaweb_context.c
    StringStarVector v;
    const char* host = NULL;
    if (fetch->request_headers_.Lookup(HttpAttributes::kHost, &v) &&
        (v.size() == 1) && (v[0] != NULL)) {
      host = v[0]->c_str();
    }

    fetch->FixUserAgent();

    *req_bkt = serf_request_bucket_request_create_for_host(
        request, kFetchMethod,
        url_path, NULL,
        serf_request_get_alloc(request), host);
    serf_bucket_t* hdrs_bkt = serf_bucket_request_get_headers(*req_bkt);

    for (int i = 0; i < fetch->request_headers_.NumAttributes(); ++i) {
      const GoogleString& name = fetch->request_headers_.Name(i);
      const GoogleString& value = fetch->request_headers_.Value(i);
      if ((StringCaseEqual(name, HttpAttributes::kUserAgent)) ||
          (StringCaseEqual(name, HttpAttributes::kAcceptEncoding)) ||
          (StringCaseEqual(name, HttpAttributes::kReferer))) {
        serf_bucket_headers_setn(hdrs_bkt, name.c_str(), value.c_str());
      }
    }

    // TODO(jmarantz): add accept-encoding:gzip even if not requested by
    // the caller, but then decompress in the output handler.

    *acceptor = SerfFetch::AcceptResponse;
    *acceptor_baton = fetch;
    *handler = SerfFetch::HandleResponse;
    *handler_baton = fetch;
    return APR_SUCCESS;
  }

  bool ParseUrl() {
    apr_status_t status = 0;
    status = apr_uri_parse(pool_, str_url_.c_str(), &url_);
    if (status != APR_SUCCESS) {
      return false;  // Failed to parse URL.
    }

    // TODO(lsong): We do not handle HTTPS for now. HTTPS needs authentication
    // verifying certificates, etc.
    if (StringCaseEqual(url_.scheme, "https")) {
      return false;
    }
    if (!url_.port) {
      url_.port = apr_uri_port_of_scheme(url_.scheme);
    }
    if (!url_.path) {
      url_.path = apr_pstrdup(pool_, "/");
    }
    return true;
  }

  SerfUrlAsyncFetcher* fetcher_;
  Timer* timer_;
  const std::string str_url_;
  RequestHeaders request_headers_;
  ResponseHeaders* response_headers_;
  ResponseHeadersParser parser_;
  Writer* fetched_content_writer_;
  MessageHandler* message_handler_;
  UrlAsyncFetcher::Callback* callback_;

  apr_pool_t* pool_;
  serf_bucket_alloc_t* bucket_alloc_;
  apr_uri_t url_;
  serf_connection_t* connection_;
  size_t bytes_received_;
  int64 fetch_start_ms_;
  int64 fetch_end_ms_;

  DISALLOW_COPY_AND_ASSIGN(SerfFetch);
};

class SerfThreadedFetcher : public SerfUrlAsyncFetcher {
 public:
  SerfThreadedFetcher(SerfUrlAsyncFetcher* parent, const char* proxy) :
      SerfUrlAsyncFetcher(parent, proxy),
      initiate_mutex_(pool_),
      initiate_fetches_(new SerfFetchPool()),
      initiate_fetches_nonempty_(&initiate_mutex_),
      thread_finish_(false) {
    CHECK_EQ(APR_SUCCESS,
             apr_thread_create(&thread_id_, NULL, SerfThreadFn, this, pool_));
  }

  ~SerfThreadedFetcher() {
    // Let the thread terminate naturally by telling it to unblock,
    // then waiting for it to finish its next active Poll operation.
    {
      // Indicate termination and unblock the worker thread so it can clean up.
      ScopedMutex lock(&initiate_mutex_);
      thread_finish_ = true;
      initiate_fetches_nonempty_.Signal();
    }

    LOG(INFO) << "Waiting for threaded serf fetcher to terminate";
    apr_status_t ignored_retval;
    apr_thread_join(&ignored_retval, thread_id_);

    // Under normal circumstances there shouldn't be any active fetches at
    // this point.  However, in practice we may have some lingering fetches that
    // have timed out, and we need to clean those up properly before we can
    // exit.  We try to do this gracefully, but fall back to graceless cleanup
    // if that fails.

    // Before we can clean up, we must make sure we haven't initiated any
    // fetches that haven't moved to the active pool yet.  This should not
    // happen, but we're exercising undue caution here.  We do this by just
    // moving them across.  From this point, calls to InitiateFetch(...) are
    // illegal, but we should be invoking this destructor from the only thread
    // that could have called InitiateFetch anyhow.
    TransferFetchesAndCheckDone(false);
    // Although Cancel will be called in the base class destructor, we
    // want to call it here as well, as it will make it easier for the
    // thread to terminate.
    CancelActiveFetches();
    completed_fetches_.DeleteAll();
    initiate_fetches_->DeleteAll();
  }

  // Called from mainline to queue up a fetch for the thread.  If the
  // thread is idle then we can unlock it.
  void InitiateFetch(SerfFetch* fetch) {
    ScopedMutex lock(&initiate_mutex_);
    // TODO(jmaessen): Consider adding an awaiting_nonempty_ flag to avoid
    // spurious calls to Signal().
    bool signal = initiate_fetches_->empty();
    initiate_fetches_->Add(fetch);
    if (signal) {
      initiate_fetches_nonempty_.Signal();
    }
  }

 protected:
  bool AnyPendingFetches() {
    ScopedMutex lock(&initiate_mutex_);
    // NOTE: We must hold both mutexes to avoid the case where we miss a fetch
    // in transit.
    return !initiate_fetches_->empty() ||
        SerfUrlAsyncFetcher::AnyPendingFetches();
  }

 private:
  static void* SerfThreadFn(apr_thread_t* thread_id, void* context) {
    SerfThreadedFetcher* stc = static_cast<SerfThreadedFetcher*>(context);
    CHECK_EQ(thread_id, stc->thread_id_);
    stc->SerfThread();
    return NULL;
  }

  // Transfer fetches from initiate_fetches_ to active_fetches_.  If there's no
  // new fetches to initiate, check whether the Apache thread is trying to shut
  // down the worker thread, and return true to indicate "done".  Doesn't do any
  // work if initiate_fetches_ is empty, but in that case if block_on_empty is
  // true it will perform a bounded wait for initiate_fetches_nonempty_.  Called
  // by worker thread and during thread cleanup.
  bool TransferFetchesAndCheckDone(bool block_on_empty) {
    // Use a temp to minimize the amount of time we hold the
    // initiate_mutex_ lock, so that the parent thread doesn't get
    // blocked trying to initiate fetches.
    scoped_ptr<SerfFetchPool> xfer_fetches(NULL);
    {
      ScopedMutex lock(&initiate_mutex_);
      // We must do this checking under the initiate_mutex_ lock.
      if (initiate_fetches_->empty()) {
        // No new work to do now.
        if (!block_on_empty || thread_finish_) {
          return thread_finish_;
        } else {
          // Wait until some work shows up.  Note that after the wait we still
          // must actually check that there's some work to be done.
          initiate_fetches_nonempty_.TimedWait(Timer::kSecondMs);
          if (initiate_fetches_->empty()) {
            // On timeout / false wakeup, return control to caller; we might be
            // finished or have other things to attend to.
            return thread_finish_;
          }
        }
      }
      xfer_fetches.reset(new SerfFetchPool());

      // Take mutex_ before relinquishing initiate_mutex_.  This guarantees that
      // AnyPendingFetches cannot see us in the time between emptying
      // initiate_fetches_ and inserting into active_fetches_.  At that time, it
      // can look as though no fetch work is occurring.  Note that we obtain
      // mutex_ before performing the swap (but after creating the new pool)
      // because additional fetches might arrive in the mean time.  This was
      // causing problems with timeout in TestThreeThreaded under valgrind,
      // because we'd block waiting for mutex_ after a single fetch had been
      // initiated, but not obtain mutex_ until after several more fetches
      // arrived (at which point we'd go into the poll loop without initiating
      // all available fetches).
      mutex_->Lock();
      xfer_fetches.swap(initiate_fetches_);
    }

    // Now that we've unblocked the parent thread, we can leisurely
    // queue up the fetches, employing the proper lock for the active_fetches_
    // set.  Actually we expect we wll never have contention on this mutex
    // from the thread.
    int num_started = 0;
    while (!xfer_fetches->empty()) {
      SerfFetch* fetch = xfer_fetches->RemoveOldest();
      if (fetch->Start(this)) {
        SERF_DEBUG(LOG(INFO) << "Adding threaded fetch to url "
                   << fetch->str_url()
                   << " (" << active_fetches_.size() << ")");
        active_fetches_.Add(fetch);
        ++num_started;
      } else {
        delete fetch;
      }
    }
    if ((num_started != 0) && (active_count_ != NULL)) {
      // Note that we could do this after mutex_->Unlock(), but then in the
      // actual tests we see cases where we complete the fetch and decrement
      // num_started before we perform the Add here (because we're shutting down
      // and the main thread is trying to help us along by calling Poll(...) on
      // our behalf).
      active_count_->Add(num_started);
    }
    mutex_->Unlock();
    return false;
  }

  void SerfThread() {
    // Initially there's no active fetch work to be done.
    int num_active_fetches = 0;
    while (!TransferFetchesAndCheckDone(num_active_fetches == 0)) {
      // If initiate_fetches is empty, and there's no current active fetch
      // work to do, we'll block in the above call.  Otherwise the call will
      // start initiated fetches (if any) without blocking.

      // We set the poll interval to try to start new fetches promptly from the
      // observer's perspective (ie .1s is perceptible, so we try to make sure
      // new fetches are started after at most half that time).  The downside is
      // that we don't hand off control to serf / the OS for long periods when
      // fetches are active but no data is arriving.  We trust that doesn't
      // happen often.
      // TODO(jmaessen): Break out of Poll before timeout if work becomes
      // available, so that we initiate new fetches as promptly as possible
      // while continuing to serve the old ones.  This would let us dial the
      // poll interval up high (to multiple seconds).  The classic trick here is
      // to set up a pipe/FIFO/socket and add it to the set of things being
      // read, then use a write to force wakeup.  But will serf support this
      // kind of thing?
      const int64 kPollIntervalMs = Timer::kSecondMs / 20;
      // If active_fetches_ is empty, we will not do any work and won't block
      // here.  num_active_fetches will be 0, and we'll block in the next
      // call to TransferFetches above.
      num_active_fetches = Poll(kPollIntervalMs);
      SERF_DEBUG(LOG(INFO) << "Finished polling from serf thread ("
                 << this << ")");
    }
  }

  apr_thread_t* thread_id_;

  // protects initiate_fetches_, initiate_fetches_nonempty_, and thread_finish_.
  AprMutex initiate_mutex_;
  // pushed in the main thread; popped by TransferFetches().
  scoped_ptr<SerfFetchPool> initiate_fetches_;
  // condvar that indicates that initiate_fetches_ has become nonempty.  During
  // normal operation, only the serf worker thread consumes initiated fetches
  // (this can change during thread shutdown), but the usual condition variable
  // caveats apply: Just because the condition variable indicates
  // initiate_fetches_nonempty_ doesn't mean it's true, and a waiting thread
  // must check initiate_fetches_ explicitly while holding initiate_mutex_.
  AprCondvar initiate_fetches_nonempty_;

  // Flag to signal worker to finish working and terminate.
  bool thread_finish_;

  DISALLOW_COPY_AND_ASSIGN(SerfThreadedFetcher);
};

bool SerfFetch::Start(SerfUrlAsyncFetcher* fetcher) {
  // Note: this is called in the thread's context, so this is when we do
  // the pool ops.
  fetcher_ = fetcher;
  apr_pool_create(&pool_, fetcher_->pool());
  bucket_alloc_ = serf_bucket_allocator_create(pool_, NULL, NULL);

  fetch_start_ms_ = timer_->NowMs();
  // Parse and validate the URL.
  if (!ParseUrl()) {
    return false;
  }

  apr_status_t status = serf_connection_create2(&connection_,
                                                fetcher_->serf_context(),
                                                url_,
                                                ConnectionSetup, this,
                                                ClosedConnection, this,
                                                pool_);
  if (status != APR_SUCCESS) {
    message_handler_->Error(str_url_.c_str(), 0,
                            "Error status=%d (%s) serf_connection_create2",
                            status, GetAprErrorString(status).c_str());
    return false;
  }
  serf_connection_request_create(connection_, SetupRequest, this);

  // Start the fetch. It will connect to the remote host, send the request,
  // and accept the response, without blocking.
  status = serf_context_run(fetcher_->serf_context(), 0, fetcher_->pool());

  if (status == APR_SUCCESS || APR_STATUS_IS_TIMEUP(status)) {
    return true;
  } else {
    message_handler_->Error(str_url_.c_str(), 0,
                            "serf_context_run error status=%d (%s)",
                            status, GetAprErrorString(status).c_str());
    return false;
  }
}


// Set up the proxy for all the connections in the context. The proxy is in the
// format of hostname:port.
bool SerfUrlAsyncFetcher::SetupProxy(const char* proxy) {
  apr_status_t status = 0;
  if (proxy == NULL || *proxy == '\0') {
    return true;  // No proxy to be set.
  }

  apr_sockaddr_t* proxy_address = NULL;
  apr_port_t proxy_port;
  char* proxy_host;
  char* proxy_scope;
  status = apr_parse_addr_port(&proxy_host, &proxy_scope, &proxy_port, proxy,
                               pool_);
  if (status != APR_SUCCESS || proxy_host == NULL || proxy_port == 0 ||
      (status = apr_sockaddr_info_get(&proxy_address, proxy_host, APR_UNSPEC,
                                      proxy_port, 0, pool_)) != APR_SUCCESS) {
    return false;
  }
  serf_config_proxy(serf_context_, proxy_address);
  return true;
}

SerfUrlAsyncFetcher::SerfUrlAsyncFetcher(const char* proxy, apr_pool_t* pool,
                                         Statistics* statistics, Timer* timer,
                                         int64 timeout_ms)
    : pool_(NULL),
      timer_(timer),
      mutex_(NULL),
      serf_context_(NULL),
      threaded_fetcher_(NULL),
      active_count_(NULL),
      request_count_(NULL),
      byte_count_(NULL),
      time_duration_ms_(NULL),
      cancel_count_(NULL),
      timeout_count_(NULL),
      timeout_ms_(timeout_ms) {
  CHECK(statistics != NULL);
  request_count_  =
      statistics->GetVariable(SerfStats::kSerfFetchRequestCount);
  byte_count_ = statistics->GetVariable(SerfStats::kSerfFetchByteCount);
  time_duration_ms_ =
      statistics->GetVariable(SerfStats::kSerfFetchTimeDurationMs);
  cancel_count_ = statistics->GetVariable(SerfStats::kSerfFetchCancelCount);
  active_count_ = statistics->GetVariable(SerfStats::kSerfFetchActiveCount);
  timeout_count_ = statistics->GetVariable(SerfStats::kSerfFetchTimeoutCount);
  Init(pool, proxy);
  threaded_fetcher_ = new SerfThreadedFetcher(this, proxy);
}

SerfUrlAsyncFetcher::SerfUrlAsyncFetcher(SerfUrlAsyncFetcher* parent,
                                         const char* proxy)
    : pool_(NULL),
      timer_(parent->timer_),
      mutex_(NULL),
      serf_context_(NULL),
      threaded_fetcher_(NULL),
      active_count_(parent->active_count_),
      request_count_(parent->request_count_),
      byte_count_(parent->byte_count_),
      time_duration_ms_(parent->time_duration_ms_),
      cancel_count_(parent->cancel_count_),
      timeout_count_(parent->timeout_count_),
      timeout_ms_(parent->timeout_ms()) {
  Init(parent->pool(), proxy);
  threaded_fetcher_ = NULL;
}

SerfUrlAsyncFetcher::~SerfUrlAsyncFetcher() {
  CancelActiveFetches();
  completed_fetches_.DeleteAll();
  int orphaned_fetches = active_fetches_.size();
  if (orphaned_fetches != 0) {
    LOG(ERROR) << "SerfFecher destructed with " << orphaned_fetches
               << " orphaned fetches.";
    if (active_count_ != NULL) {
      active_count_->Add(-orphaned_fetches);
    }
    if (cancel_count_ != NULL) {
      cancel_count_->Add(orphaned_fetches);
    }
  }

  active_fetches_.DeleteAll();
  if (threaded_fetcher_ != NULL) {
    delete threaded_fetcher_;
  }
  delete mutex_;
  apr_pool_destroy(pool_);  // also calls apr_allocator_destroy on the allocator
}

void SerfUrlAsyncFetcher::Init(apr_pool_t* parent_pool, const char* proxy) {
  // Here, we give each our Serf threads' (main and work) separate pools
  // with separate allocators. This is done because:
  //
  // 1) Concurrent allocations from the same pools are not (thread)safe.
  // 2) Concurrent allocations from different pools using the same allocator
  //    are not safe unless the allocator has a mutex set.
  // 3) prefork's pchild pool (which is our ancestor) has an allocator without
  //    a mutex set.
  //
  // Note: the above is all about the release version of the pool code, the
  // checking one has some additional locking!
  apr_allocator_t* allocator = NULL;
  CHECK(apr_allocator_create(&allocator) == APR_SUCCESS);
  apr_pool_create_ex(&pool_, parent_pool, NULL /*abortfn*/, allocator);
  apr_allocator_owner_set(allocator, pool_);

  mutex_ = new AprMutex(pool_);
  serf_context_ = serf_context_create(pool_);

  if (!SetupProxy(proxy)) {
    LOG(WARNING) << "Proxy failed: " << proxy;
  }
}

void SerfUrlAsyncFetcher::CancelActiveFetches() {
  // If there are still active requests, cancel them.
  int num_canceled = 0;
  {
    ScopedMutex lock(mutex_);
    while (!active_fetches_.empty()) {
      // Cancelling a fetch requires that the fetch reside in active_fetches_,
      // but can invalidate iterators pointing to the affected fetch.  To avoid
      // trouble, we simply ask for the oldest element, knowing it will go away.
      SerfFetch* fetch = active_fetches_.oldest();
      LOG(WARNING) << "Aborting fetch of " << fetch->str_url();
      fetch->Cancel();
      ++num_canceled;
    }
  }
  if (num_canceled != 0) {
    if (cancel_count_ != NULL) {
      cancel_count_->Add(num_canceled);
    }
  }
}

bool SerfUrlAsyncFetcher::StreamingFetch(const std::string& url,
                                         const RequestHeaders& request_headers,
                                         ResponseHeaders* response_headers,
                                         Writer* fetched_content_writer,
                                         MessageHandler* message_handler,
                                         UrlAsyncFetcher::Callback* callback) {
  SerfFetch* fetch = new SerfFetch(
      url, request_headers, response_headers, fetched_content_writer,
      message_handler, callback, timer_);
  request_count_->Add(1);
  if (callback->EnableThreaded()) {
    message_handler->Message(kInfo, "Initiating async fetch for %s",
                             url.c_str());
    threaded_fetcher_->InitiateFetch(fetch);
  } else {
    message_handler->Message(kInfo, "Initiating blocking fetch for %s",
                             url.c_str());
    {
      ScopedMutex mutex(mutex_);
      if (fetch->Start(this)) {
        active_fetches_.Add(fetch);
        active_count_->Add(1);
      } else {
        callback->Done(false);
        delete fetch;
      }
    }
  }
  return false;
}

void SerfUrlAsyncFetcher::PrintActiveFetches(
    MessageHandler* handler) const {
  ScopedMutex mutex(mutex_);
  for (SerfFetchPool::const_iterator p = active_fetches_.begin(),
           e = active_fetches_.end(); p != e; ++p) {
    SerfFetch* fetch = *p;
    handler->Message(kInfo, "Active fetch: %s",
                     fetch->str_url());
  }
}

// If active_fetches_ is empty, this does no work and returns 0.
int SerfUrlAsyncFetcher::Poll(int64 max_wait_ms) {
  // Run serf polling up to microseconds.
  ScopedMutex mutex(mutex_);
  if (!active_fetches_.empty()) {
    apr_status_t status =
        serf_context_run(serf_context_, 1000*max_wait_ms, pool_);
    completed_fetches_.DeleteAll();
    if (APR_STATUS_IS_TIMEUP(status)) {
      // Remove expired fetches from the front of the queue.
      // This relies on the insertion-ordering guarantee
      // provided by the Pool iterator.
      int64 stale_cutoff = timer_->NowMs() - timeout_ms_;
      int timeouts = 0;
      // This loop calls Cancel, which deletes a fetch and thus invalidates
      // iterators; we thus rely on retrieving oldest().
      while (!active_fetches_.empty()) {
        SerfFetch* fetch = active_fetches_.oldest();
        if (fetch->fetch_start_ms() >= stale_cutoff) {
          // This and subsequent fetches are still active, so we're done.
          break;
        }
        LOG(WARNING) << "Fetch timed out: " << fetch->str_url();
        timeouts++;
        // Note that cancelling the fetch will ultimately call FetchComplete and
        // delete it from the pool.
        fetch->Cancel();
      }
      if ((timeouts > 0) && (timeout_count_ != NULL)) {
        timeout_count_->Add(timeouts);
      }
    }
    bool success = ((status == APR_SUCCESS) || APR_STATUS_IS_TIMEUP(status));
    // TODO(jmarantz): provide the success status to the caller if there is a
    // need.
    if (!success && !active_fetches_.empty()) {
      // TODO(jmarantz): I have a new theory that we are getting
      // behind when our self-directed URL fetches queue up multiple
      // requests for the same URL, which might be sending the Serf
      // library into an n^2 situation with its polling, even though
      // we are using an rb_tree to hold the active fetches.  We
      // should fix this by keeping a map from url->SerfFetch, where
      // we'd have to store lists of Callback*, ResponseHeader*, Writer* so
      // all interested parties were updated if and when the fetch finally
      // completed.
      // NOTE(jmaessen): this is actually hard because all the above data is
      // process-local, and the multiple requests are likely cross-process.
      //
      // In the meantime by putting more detail into the log here, we'll
      // know whether we are accumulating active fetches to make the
      // server fall over.
      LOG(ERROR) << "Serf status " << status << " ("
                 << GetAprErrorString(status) << " ) polling for "
                 << active_fetches_.size()
                 << ((threaded_fetcher_ == NULL) ? ": (threaded)"
                     : ": (non-blocking)")
                 << " (" << this << ") for " << max_wait_ms/1.0e3
                 << " seconds";
      CleanupFetchesWithErrors();
    }
  }
  return active_fetches_.size();
}

void SerfUrlAsyncFetcher::FetchComplete(SerfFetch* fetch) {
  // We do not have a ScopedMutex in FetchComplete, because it is only
  // called from Poll and CancelActiveFetches, which have ScopedMutexes.
  // Note that SerfFetch::Cancel is currently not exposed from outside this
  // class.
  active_fetches_.Remove(fetch);
  completed_fetches_.Add(fetch);
  fetch->message_handler()->Message(kInfo, "Fetch complete: %s",
                                    fetch->str_url());
  if (time_duration_ms_) {
    time_duration_ms_->Add(fetch->TimeDuration());
  }
  if (byte_count_) {
    byte_count_->Add(fetch->bytes_received());
  }
  if (active_count_) {
    active_count_->Add(-1);
  }
}

bool SerfUrlAsyncFetcher::AnyPendingFetches() {
  ScopedMutex lock(mutex_);
  return !active_fetches_.empty();
}

int SerfUrlAsyncFetcher:: ApproximateNumActiveFetches() {
  ScopedMutex lock(mutex_);
  return active_fetches_.size();
}

bool SerfUrlAsyncFetcher::WaitForActiveFetches(
    int64 max_ms, MessageHandler* message_handler, WaitChoice wait_choice) {
  bool ret = true;
  if ((threaded_fetcher_ != NULL) && (wait_choice != kMainlineOnly)) {
    ret &= threaded_fetcher_->WaitForActiveFetchesHelper(
        max_ms, message_handler);
  }
  if (wait_choice != kThreadedOnly) {
    ret &= WaitForActiveFetchesHelper(max_ms, message_handler);
  }
  return ret;
}

bool SerfUrlAsyncFetcher::WaitForActiveFetchesHelper(
    int64 max_ms, MessageHandler* message_handler) {
  bool any_pending_fetches = AnyPendingFetches();
  if (any_pending_fetches) {
    int64 now_ms = timer_->NowMs();
    int64 end_ms = now_ms + max_ms;
    while ((now_ms < end_ms) && any_pending_fetches) {
      int64 remaining_ms = end_ms - now_ms;
      SERF_DEBUG(LOG(INFO) << "Blocking process waiting " << remaining_ms
                 << "ms for " << ApproximateNumActiveFetches()
                 << " fetches to complete");
      SERF_DEBUG(PrintActiveFetches(message_handler));
      Poll(remaining_ms);
      now_ms = timer_->NowMs();
      any_pending_fetches = AnyPendingFetches();
    }
    if (any_pending_fetches) {
      message_handler->Message(
          kError, "Serf timeout waiting for fetches to complete:");
      PrintActiveFetches(message_handler);
      return false;
    }
    SERF_DEBUG(LOG(INFO) << "Serf successfully completed "
               << ApproximateNumActiveFetches() << " active fetches");
  }
  return true;
}

void SerfUrlAsyncFetcher::CleanupFetchesWithErrors() {
  // Create a copy of list of active fetches, as we may have to cancel
  // some failed ones, modifying the list.
  std::vector<SerfFetch*> fetches;
  for (SerfFetchPool::iterator i = active_fetches_.begin();
       i != active_fetches_.end(); ++i) {
    fetches.push_back(*i);
  }

  // Check each fetch to see if it needs cleanup because its serf connection
  // got into an error state.
  for (int i = 0, size = fetches.size(); i < size; ++i) {
    fetches[i]->CleanupIfError();
  }
}

void SerfUrlAsyncFetcher::Initialize(Statistics* statistics) {
  if (statistics != NULL) {
    statistics->AddVariable(SerfStats::kSerfFetchRequestCount);
    statistics->AddVariable(SerfStats::kSerfFetchByteCount);
    statistics->AddVariable(SerfStats::kSerfFetchTimeDurationMs);
    statistics->AddVariable(SerfStats::kSerfFetchCancelCount);
    statistics->AddVariable(SerfStats::kSerfFetchActiveCount);
    statistics->AddVariable(SerfStats::kSerfFetchTimeoutCount);
  }
}

}  // namespace net_instaweb
