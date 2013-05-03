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
//
// Author: jmarantz@google.com (Joshua Marantz)
//         lsong@google.com (Libo Song)

#include "net/instaweb/apache/instaweb_context.h"

#include "base/logging.h"
#include "net/instaweb/apache/apache_server_context.h"
#include "net/instaweb/apache/apr_timer.h"
#include "net/instaweb/apache/header_util.h"
#include "net/instaweb/apache/mod_instaweb.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/user_agent_matcher.h"
#include "net/instaweb/rewriter/public/furious_matcher.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/condvar.h"
#include "net/instaweb/util/public/gzip_inflater.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/timer.h"
#include "net/instaweb/util/stack_buffer.h"
#include "apr_strings.h"
#include "http_config.h"
#include "http_core.h"

namespace net_instaweb {

// Number of times to go down the request->prev chain looking for an
// absolute url.
const int kRequestChainLimit = 5;

PropertyCallback::PropertyCallback(RewriteDriver* driver,
                                   ThreadSystem* thread_system,
                                   const StringPiece& key)
    : PropertyPage(PropertyPage::kPropertyCachePage,
                   key, driver->request_context(),
                   thread_system->NewMutex(),
                   driver->server_context()->page_property_cache()),
  driver_(driver),
  done_(false),
  mutex_(thread_system->NewMutex()),
  condvar_(mutex_->NewCondvar()) {
}

void PropertyCallback::Done(bool success) {
  ScopedMutex lock(mutex_.get());
  driver_->set_property_page(this);
  done_ = true;
  condvar_->Signal();
}

void PropertyCallback::BlockUntilDone() {
  int iters = 0;
  ScopedMutex lock(mutex_.get());
  while (!done_) {
    condvar_->TimedWait(Timer::kSecondMs);
    if (!done_) {
      driver_->message_handler()->Message(
          kError, "Waiting for property cache fetch to complete. "
          "Elapsed time: %ds", ++iters);
    }
  }
}

InstawebContext::InstawebContext(request_rec* request,
                                 RequestHeaders* request_headers,
                                 const ContentType& content_type,
                                 ApacheServerContext* server_context,
                                 const GoogleString& absolute_url,
                                 const RequestContextPtr& request_context,
                                 bool use_custom_options,
                                 const RewriteOptions& options)
    : content_encoding_(kNone),
      content_type_(content_type),
      server_context_(server_context),
      string_writer_(&output_),
      absolute_url_(absolute_url),
      request_headers_(request_headers),
      started_parse_(false),
      sent_headers_(false),
      populated_headers_(false),
      modify_caching_headers_(true) {
  if (options.running_furious()) {
    // Furious requires custom options because it has to make changes based on
    // what ExperimentSpec the user should be seeing.
    use_custom_options = true;
  }
  if (use_custom_options) {
    // TODO(jmarantz): this is a temporary hack until we sort out better
    // memory management of RewriteOptions.  This will drag on performance.
    // We need to do this because we are changing RewriteDriver to keep
    // a reference to its options throughout its lifetime to refer to the
    // domain lawyer and other options.
    RewriteOptions* custom_options = options.Clone();

    // If we're running a Furious experiment, determine the state of this
    // request and reset the options accordingly.
    if (custom_options->running_furious()) {
      SetFuriousStateAndCookie(request, custom_options);
    }
    server_context_->ComputeSignature(custom_options);
    rewrite_driver_ = server_context_->NewCustomRewriteDriver(
        custom_options, request_context);
  } else {
    rewrite_driver_ = server_context_->NewRewriteDriver(request_context);
  }
  modify_caching_headers_ =
      rewrite_driver_->options()->modify_caching_headers();

  const char* user_agent = apr_table_get(request->headers_in,
                                         HttpAttributes::kUserAgent);
  rewrite_driver_->SetUserAgent(user_agent);

  // Begin the property cache lookup. This should be as early as possible since
  // it may be asynchronous (in the case of memcached).
  // TODO(jud): It would be ideal to move this even earlier. As early as, say,
  // save_url_hook. However, there is no request specific context to save the
  // result in at that point.
  PropertyCallback* property_callback(InitiatePropertyCacheLookup());

  rewrite_driver_->EnableBlockingRewrite(request_headers);

  ComputeContentEncoding(request);
  apr_pool_cleanup_register(request->pool,
                            this, apache_cleanup<InstawebContext>,
                            apr_pool_cleanup_null);

  bucket_brigade_ = apr_brigade_create(request->pool,
                                       request->connection->bucket_alloc);

  if (content_encoding_ == kGzip || content_encoding_ == kDeflate) {
    // TODO(jmarantz): consider keeping a pool of these if they are expensive
    // to initialize.
    if (content_encoding_ == kGzip) {
      inflater_.reset(new GzipInflater(GzipInflater::kGzip));
    } else {
      inflater_.reset(new GzipInflater(GzipInflater::kDeflate));
    }
    inflater_->Init();
  }

  // Make the entire request headers available to filters.
  rewrite_driver_->set_request_headers(request_headers_.get());

  response_headers_.Clear();
  rewrite_driver_->set_response_headers_ptr(&response_headers_);
  // TODO(lsong): Bypass the string buffer, write data directly to the next
  // apache bucket.
  rewrite_driver_->SetWriter(&string_writer_);

  // Wait until property cache lookup is complete
  if (property_callback != NULL) {
    property_callback->BlockUntilDone();
  }
}

InstawebContext::~InstawebContext() {
}

void InstawebContext::Rewrite(const char* input, int size) {
  if (inflater_.get() != NULL) {
    char buf[kStackBufferSize];
    inflater_->SetInput(input, size);
    while (inflater_->HasUnconsumedInput()) {
      int num_inflated_bytes = inflater_->InflateBytes(buf, kStackBufferSize);
      DCHECK_LE(0, num_inflated_bytes) << "Corrupted zip inflation";
      if (num_inflated_bytes > 0) {
        ProcessBytes(buf, num_inflated_bytes);
      }
    }
  } else {
    DCHECK_LE(0, size) << "negatively sized buffer passed from apache";
    if (size > 0) {
      ProcessBytes(input, size);
    }
  }
}

void InstawebContext::Flush() {
  if (html_detector_.already_decided() && started_parse_) {
    rewrite_driver_->Flush();
  }
}

void InstawebContext::Finish() {
  if (!html_detector_.already_decided()) {
    // We couldn't determine whether this is HTML or not till the very end,
    // so serve it unmodified.
    html_detector_.ReleaseBuffered(&output_);
  }

  if (started_parse_) {
    rewrite_driver_->FinishParse();
  } else {
    rewrite_driver_->Cleanup();
  }
}

void InstawebContext::PopulateHeaders(request_rec* request) {
  if (!populated_headers_) {
    ApacheRequestToResponseHeaders(*request, &response_headers_, NULL);
    populated_headers_ = true;
  }
}

void InstawebContext::ProcessBytes(const char* input, int size) {
  CHECK_LT(0, size);

  if (!html_detector_.already_decided()) {
    if (html_detector_.ConsiderInput(StringPiece(input, size))) {
      if (html_detector_.probable_html()) {
        // Note that we use started_parse_ and not probable_html()
        // in all other spots as an error fallback.
        started_parse_ = rewrite_driver_->StartParseWithType(absolute_url_,
                                                             content_type_);
      }

      // If we buffered up any bytes in previous calls, make sure to
      // release them.
      GoogleString buffer;
      html_detector_.ReleaseBuffered(&buffer);
      if (!buffer.empty()) {
        // Recurse on initial buffer of whitespace before processing
        // this call's input below.
        ProcessBytes(buffer.data(), buffer.size());
      }
    }
  }

  // Either as effect of above or initially at entry.
  if (html_detector_.already_decided()) {
    if (started_parse_) {
      rewrite_driver_->ParseText(input, size);
    } else {
      // Looks like something that's not HTML.  Send it directly to the
      // output buffer.
      output_.append(input, size);
    }
  }
}

void InstawebContext::ComputeContentEncoding(request_rec* request) {
  // Check if the content is gzipped. Steal from mod_deflate.
  const char* encoding = apr_table_get(request->headers_out,
                                       HttpAttributes::kContentEncoding);
  if (encoding != NULL) {
    const char* err_enc = apr_table_get(request->err_headers_out,
                                        HttpAttributes::kContentEncoding);
    if (err_enc != NULL) {
      // We don't properly handle stacked encodings now.
      content_encoding_ = kOther;
    }
  } else {
    encoding = apr_table_get(request->err_headers_out,
                             HttpAttributes::kContentEncoding);
  }

  if (encoding != NULL) {
    if (StringCaseEqual(encoding, HttpAttributes::kGzip)) {
      content_encoding_ = kGzip;
    } else if (StringCaseEqual(encoding, HttpAttributes::kDeflate)) {
      content_encoding_ = kDeflate;
    } else {
      content_encoding_ = kOther;
    }
  }
}

PropertyCallback* InstawebContext::InitiatePropertyCacheLookup() {
  PropertyCallback* property_callback = NULL;
  if (server_context_->page_property_cache()->enabled()) {
    const UserAgentMatcher* user_agent_matcher =
        server_context_->user_agent_matcher();
    UserAgentMatcher::DeviceType device_type =
        user_agent_matcher->GetDeviceTypeForUA(rewrite_driver_->user_agent());
    StringPiece device_type_suffix =
        UserAgentMatcher::DeviceTypeSuffix(device_type);

    GoogleString key = server_context_->GetPagePropertyCacheKey(
        absolute_url_, rewrite_driver_->options(), device_type_suffix);
    property_callback = new PropertyCallback(rewrite_driver_,
                                             server_context_->thread_system(),
                                             key);
    server_context_->page_property_cache()->Read(property_callback);
  }
  return property_callback;
}

ApacheServerContext* InstawebContext::ServerContextFromServerRec(
    server_rec* server) {
  return static_cast<ApacheServerContext*>
      ap_get_module_config(server->module_config, &pagespeed_module);
}

// This function stores the request uri on the first call, and then
// uses that value for all future calls.  This should prevent the url
// from changing due to changes to the reqeust from other modules.
// In some code paths, a new request is made that throws away the old
// url.  Therefore, if we have not yet stored the url, check to see if
// there was a previous request in this chain, and use its url as the
// original.
const char* InstawebContext::MakeRequestUrl(const RewriteOptions& options,
                                            request_rec* request) {
  const char* url = apr_table_get(request->notes, kPagespeedOriginalUrl);

  if (url == NULL) {
    // Go down the prev chain to see if there this request was a rewrite
    // from another one.  We want to store the uri the user passed in,
    // not what we re-wrote it to.  We should not iterate down this
    // chain more than once (MakeRequestUrl will already have been
    // called for request->prev, before this request is created).
    // However, max out at 5 iterations, just in case.
    request_rec *prev = request->prev;
    for (int i = 0; (url == NULL) && (prev != NULL) && (i < kRequestChainLimit);
         ++i, prev = prev->prev) {
      url = apr_table_get(prev->notes, kPagespeedOriginalUrl);
    }

    // Chase 'main' chain as well, clamping at kRequestChainLimit loops.
    // This will eliminate spurious 'index.html' noise we've seen from
    // slurps.  See 'make apache_debug_slurp_test' -- the attempt to
    // slurp 'www.example.com'.  The reason this is necessary is that
    // mod_dir.c's fixup_dir() calls ap_internal_fast_redirect in
    // http_request.c, which mutates the original requests's uri fields,
    // leaving little trace of the url we actually need to resolve.  Also
    // note that http_request.c:ap_internal_fast_redirect 'overlays'
    // the source r.notes onto the dest r.notes, which in this case would
    // work against us if we don't first propagate the OriginalUrl.
    request_rec *main = request->main;
    for (int i = 0; (url == NULL) && (main != NULL) && (i < kRequestChainLimit);
         ++i, main = main->main) {
      url = apr_table_get(main->notes, kPagespeedOriginalUrl);
    }

    /*
     * In some contexts we are seeing relative URLs passed
     * into request->unparsed_uri.  But when using mod_slurp, the rewritten
     * HTML contains complete URLs, so this construction yields the host:port
     * prefix twice.
     *
     * TODO(jmarantz): Figure out how to do this correctly at all times.
     */
    if (url == NULL) {
      if ((strncmp(request->unparsed_uri, "http://", 7) == 0) ||
          (strncmp(request->unparsed_uri, "https://", 8) == 0)) {
        url = apr_pstrdup(request->pool, request->unparsed_uri);
      } else {
        url = ap_construct_url(request->pool, request->unparsed_uri, request);
      }
    }

    // Fix URL based on X-Forwarded-Proto.
    // http://code.google.com/p/modpagespeed/issues/detail?id=546
    // For example, if Apache gives us the URL "http://www.example.com/"
    // and there is a header: "X-Forwarded-Proto: https", then we update
    // this base URL to "https://www.example.com/".
    if (options.respect_x_forwarded_proto()) {
      const char* x_forwarded_proto =
          apr_table_get(request->headers_in, HttpAttributes::kXForwardedProto);
      if (x_forwarded_proto != NULL) {
        if (StringCaseEqual(x_forwarded_proto, "http") ||
            StringCaseEqual(x_forwarded_proto, "https")) {
          StringPiece url_sp(url);
          StringPiece::size_type colon_pos = url_sp.find(":");
          if (colon_pos != StringPiece::npos) {
            // Replace URL protocol with that specified in X-Forwarded-Proto.
            GoogleString new_url =
                StrCat(x_forwarded_proto, url_sp.substr(colon_pos));
            url = apr_pstrdup(request->pool, new_url.c_str());
          }
        } else {
          LOG(WARNING) << "Unsupported X-Forwarded-Proto: " << x_forwarded_proto
                       << " for URL " << url << " protocol not changed.";
        }
      }
    }

    // Note: apr_table_setn does not take ownership of url, it is owned by
    // the Apache pool.
    apr_table_setn(request->notes, kPagespeedOriginalUrl, url);
  }
  return url;
}

void InstawebContext::SetFuriousStateAndCookie(request_rec* request,
                                               RewriteOptions* options) {
  // If we didn't get a valid (i.e. currently-running experiment) value from
  // the cookie, determine which experiment this request should end up in
  // and set the cookie accordingly.
  bool need_cookie = server_context_->furious_matcher()->
      ClassifyIntoExperiment(*request_headers_, options);
  if (need_cookie) {
    ResponseHeaders resp_headers;
    AprTimer timer;
    const char* url = apr_table_get(request->notes, kPagespeedOriginalUrl);
    int furious_value = options->furious_id();
    server_context_->furious_matcher()->StoreExperimentData(
        furious_value, url,
        timer.NowMs() + options->furious_cookie_duration_ms(),
        &resp_headers);
    AddResponseHeadersToRequest(&resp_headers, NULL,
                                options->modify_caching_headers(), request);
  }
}

}  // namespace net_instaweb
