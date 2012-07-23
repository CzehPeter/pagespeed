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
// Author: lsong@google.com (Libo Song)
//         jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/apache/instaweb_handler.h"

#include "base/scoped_ptr.h"
#include "net/instaweb/apache/apache_slurp.h"
#include "net/instaweb/apache/apache_message_handler.h"
#include "net/instaweb/apache/apr_timer.h"
#include "net/instaweb/apache/header_util.h"
#include "net/instaweb/apache/instaweb_context.h"
#include "net/instaweb/apache/interface_mod_spdy.h"
#include "net/instaweb/apache/mod_instaweb.h"
#include "net/instaweb/apache/serf_url_async_fetcher.h"
#include "net/instaweb/automatic/public/resource_fetch.h"
#include "net/instaweb/http/public/request_headers.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/http/public/sync_fetcher_adapter_callback.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/rewriter/public/add_instrumentation_filter.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/null_statistics.h"
#include "net/instaweb/util/public/shared_mem_referer_statistics.h"
#include "net/instaweb/util/public/shared_mem_statistics.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string_writer.h"

#include "apr_strings.h"
#include "http_config.h"
#include "http_core.h"
#include "http_protocol.h"
#include "http_request.h"
#include "net/instaweb/apache/apache_logging_includes.h"

namespace net_instaweb {

namespace {

const char kStatisticsHandler[] = "mod_pagespeed_statistics";
const char kRefererStatisticsHandler[] = "mod_pagespeed_referer_statistics";
const char kMessageHandler[] = "mod_pagespeed_message";
const char kBeaconHandler[] = "mod_pagespeed_beacon";
const char kResourceUrlNote[] = "mod_pagespeed_resource";
const char kResourceUrlNo[] = "<NO>";
const char kResourceUrlYes[] = "<YES>";

bool IsCompressibleContentType(const char* content_type) {
  if (content_type == NULL) {
    return false;
  }
  GoogleString type = content_type;
  size_t separator_idx = type.find(";");
  if (separator_idx != GoogleString::npos) {
    type.erase(separator_idx);
  }

  bool res = false;
  if (type.find("text/") == 0) {
    res = true;
  } else if (type.find("application/") == 0) {
    if (type.find("javascript") != type.npos ||
        type.find("json") != type.npos ||
        type.find("ecmascript") != type.npos ||
        type == "application/livescript" ||
        type == "application/js" ||
        type == "application/jscript" ||
        type == "application/x-js" ||
        type == "application/xhtml+xml" ||
        type == "application/xml") {
      res = true;
    }
  }

  return res;
}

// Default handler when the file is not found
void instaweb_404_handler(const GoogleString& url, request_rec* request) {
  request->status = HTTP_NOT_FOUND;
  ap_set_content_type(request, "text/html; charset=utf-8");
  ap_rputs("<html><head><title>Not Found</title></head>", request);
  ap_rputs("<body><h1>Apache server with mod_pagespeed</h1>OK", request);
  ap_rputs("<hr>NOT FOUND:", request);
  ap_rputs(url.c_str(), request);
  ap_rputs("</body></html>", request);
}

void send_out_headers_and_body(request_rec* request,
                               const ResponseHeaders& response_headers,
                               const GoogleString& output) {
  ResponseHeadersToApacheRequest(response_headers, request);
  if (response_headers.status_code() == HttpStatus::kOK &&
      IsCompressibleContentType(request->content_type)) {
    // Make sure compression is enabled for this response.
    ap_add_output_filter("DEFLATE", NULL, request, request->connection);
  }

  // Recompute the content-length, because the content may have changed.
  ap_set_content_length(request, output.size());
  // Send the body
  ap_rwrite(output.c_str(), output.size(), request);
}


// Determines whether the url can be handled as a mod_pagespeed resource,
// and handles it, returning true.  A 'true' routine means that this
// method believed the URL was a mod_pagespeed resource -- it does not
// imply that it was handled successfully.  That information will be
// in the status code in the response headers.
bool handle_as_resource(ApacheResourceManager* manager,
                        request_rec* request,
                        const GoogleString& url) {
  GoogleUrl gurl(url);
  bool is_ps_url = manager->IsPagespeedResource(gurl);
  if (is_ps_url) {
    MessageHandler* message_handler = manager->message_handler();
    message_handler->Message(kInfo, "Fetching resource %s...", url.c_str());

    GoogleString output;  // TODO(jmarantz): Quit buffering resource output.
    StringWriter writer(&output);

    SyncFetcherAdapterCallback* callback = new SyncFetcherAdapterCallback(
        manager->thread_system(), &writer);

    // Filter limited request headers into backend fetch.
    // TODO(sligocki): Put this filtering in ResourceFetch and instead use:
    // ApacheRequestToRequestHeaders(*request, callback->request_headers());
    for (int i = 0, n = arraysize(RewriteDriver::kPassThroughRequestAttributes);
         i < n; ++i) {
      const char* value = apr_table_get(
          request->headers_in,
          RewriteDriver::kPassThroughRequestAttributes[i]);
      if (value != NULL) {
        callback->request_headers()->Add(
            RewriteDriver::kPassThroughRequestAttributes[i], value);
      }
    }

    // Set custom options.
    RewriteOptions* custom_options = NULL;
    ApacheConfig* directory_options = static_cast<ApacheConfig*>
        ap_get_module_config(request->per_dir_config, &pagespeed_module);
    if ((directory_options != NULL) && directory_options->modified()) {
      custom_options = manager->apache_factory()->NewRewriteOptions();
      custom_options->Merge(*manager->global_options());
      custom_options->Merge(*directory_options);
    }

    bool using_spdy = (mod_spdy_get_spdy_version(request->connection) != 0);
    RewriteDriver* driver = ResourceFetch::GetDriver(
        gurl, custom_options, using_spdy, manager);
    if (ResourceFetch::BlockingFetch(gurl, manager, driver, callback)) {
      ResponseHeaders* response_headers = callback->response_headers();
      // TODO(sligocki): Check that this is already done in ResourceFetch
      // and remove redundant setting here.
      response_headers->SetDate(manager->timer()->NowMs());
      // ResourceFetch adds X-Page-Speed header, old mod_pagespeed code
      // did not. For now, we remove that header for consistency.
      // TODO(sligocki): Consistently use X- headers in MPS and PSA.
      // I think it would be good to change X-Mod-Pagespeed -> X-Page-Speed
      // and use that for all HTML and resource requests.
      response_headers->RemoveAll(kPageSpeedHeader);
      message_handler->Message(kInfo, "Fetch succeeded for %s, status=%d",
                               url.c_str(), response_headers->status_code());
      send_out_headers_and_body(request, *response_headers, output);
    } else {
      RewriteStats* stats = manager->rewrite_stats();
      stats->resource_404_count()->Add(1);
      instaweb_404_handler(url, request);
    }

    callback->Release();
  }

  return is_ps_url;
}

// Write out boilerplate HTTP headers for our custom handlers
// (like /mod_pagespeed_statistics).
void write_handler_response(const StringPiece& output, request_rec* request) {
  ResponseHeaders response_headers;
  response_headers.SetStatusAndReason(HttpStatus::kOK);
  response_headers.set_major_version(1);
  response_headers.set_minor_version(1);
  response_headers.Add(HttpAttributes::kContentType, "text/html");
  AprTimer timer;
  int64 now_ms = timer.NowMs();
  response_headers.SetDate(now_ms);
  response_headers.SetLastModified(now_ms);
  response_headers.Add(HttpAttributes::kCacheControl,
                       HttpAttributes::kNoCache);
  send_out_headers_and_body(request, response_headers, output.as_string());
}

// Returns request URL if it was a .pagespeed. rewritten resource URL.
// Otherwise returns NULL. Since other Apache modules can change request->uri,
// we run save_url_hook early to stow the original request URL in a note.
// This method reads that note and thus should return the URL that the
// browser actually requested (rather than a mod_rewrite altered URL).
const char* get_instaweb_resource_url(request_rec* request) {
  const char* resource = apr_table_get(request->notes, kResourceUrlNote);

  // If our translate_name hook, save_url_hook, failed
  // to run because some other module's translate_hook returned OK first,
  // then run it now.  The main reason we try to do this early is to
  // save our URL before mod_rewrite mutates it.
  if (resource == NULL) {
    save_url_hook(request);
    resource = apr_table_get(request->notes, kResourceUrlNote);
  }

  if (resource != NULL && strcmp(resource, kResourceUrlNo) == 0) {
    return NULL;
  }

  const char* url = apr_table_get(request->notes, kPagespeedOriginalUrl);
  return url;
}

void log_resource_referral(request_rec* request,
                           ApacheRewriteDriverFactory* factory) {
  // If all the pieces are in place, we log this request as a resource referral
  // for future prerender decision-making purposes
  SharedMemRefererStatistics* referer_stats =
      factory->shared_mem_referer_statistics();
  if (referer_stats != NULL) {
    const char* original_url = apr_table_get(request->notes,
                                             kPagespeedOriginalUrl);
    if (original_url != NULL) {
      const char* referer = apr_table_get(request->headers_in,
                                          HttpAttributes::kReferer);
      if (referer != NULL) {
        GoogleUrl referer_url(referer);
        GoogleUrl resource_url(original_url);
        referer_stats->LogResourceRequestWithReferer(resource_url,
                                                     referer_url);
      }
    }
  }
}

}  // namespace

apr_status_t instaweb_handler(request_rec* request) {
  apr_status_t ret = DECLINED;
  const char* url = get_instaweb_resource_url(request);
  ApacheResourceManager* manager =
      InstawebContext::ManagerFromServerRec(request->server);
  ApacheConfig* config = manager->config();
  ApacheRewriteDriverFactory* factory = manager->apache_factory();

  log_resource_referral(request, factory);

  if (strcmp(request->handler, kStatisticsHandler) == 0) {
    GoogleString output;
    StringWriter writer(&output);
    Statistics* statistics = factory->statistics();
    if (statistics != NULL) {
      // Write <pre></pre> for Dump to keep good format.
      writer.Write("<pre>", factory->message_handler());
      statistics->Dump(&writer, factory->message_handler());
      writer.Write("</pre>", factory->message_handler());
      statistics->RenderHistograms(&writer, factory->message_handler());
    } else {
      writer.Write("mod_pagespeed statistics is not enabled\n",
                   factory->message_handler());
    }
    write_handler_response(output, request);
    ret = OK;

  } else if (strcmp(request->handler, kRefererStatisticsHandler) == 0) {
    GoogleString output;
    StringWriter writer(&output);
    factory->DumpRefererStatistics(&writer);
    write_handler_response(output, request);
    ret = OK;

  } else if (strcmp(request->handler, kMessageHandler) == 0) {
    // Request for page /mod_pagespeed_message.
    GoogleString output;
    StringWriter writer(&output);
    ApacheMessageHandler* handler = factory->apache_message_handler();
    // Write <pre></pre> for Dump to keep good format.
    writer.Write("<pre>", factory->message_handler());
    if (!handler->Dump(&writer)) {
      writer.Write("Writing to mod_pagespeed_message failed. \n"
                   "Please check if it's enabled in pagespeed.conf.\n",
                   factory->message_handler());
    }
    writer.Write("</pre>", factory->message_handler());
    write_handler_response(output, request);
    ret = OK;

  } else if (strcmp(request->handler, kBeaconHandler) == 0) {
    manager->HandleBeacon(request->unparsed_uri);
    ret = HTTP_NO_CONTENT;

  } else if (url != NULL) {
    // Only handle GET request
    if (request->method_number != M_GET) {
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, request,
                    "Not GET request: %d.", request->method_number);
    } else if (handle_as_resource(manager, request, url)) {
      ret = OK;
    }

  } else if (config->slurping_enabled() || config->test_proxy()) {
    SlurpUrl(manager, request);
    if (request->status == HTTP_NOT_FOUND) {
      RewriteStats* stats = manager->rewrite_stats();
      stats->slurp_404_count()->Add(1);
    }
    ret = OK;
  }
  return ret;
}

// This translator must be inserted into the translate_name chain
// prior to mod_rewrite.  By saving the original URL in a
// request->notes and using that in our handler, we prevent
// mod_rewrite from borking URL names that need to be handled by
// mod_pagespeed.
//
// This hack seems to be the most robust way to immunize mod_pagespeed
// from when mod_rewrite rewrites the URL.  We still need mod_rewrite
// to do required complex processing of the filename (e.g. prepending
// the DocumentRoot) so mod_authz_host is happy, so we return DECLINED
// even for mod_pagespeed resources.
//
// One alternative strategy is to return OK to bypass mod_rewrite
// entirely, but then we'd have to duplicate the functionality in
// mod_rewrite that prepends the DocumentRoot, which is itself
// complex.  See mod_rewrite.c:hook_fixup(), and look for calls to
// ap_document_root().
//
// Or we could return DECLINED but set a note "mod_rewrite_rewritten"
// to try to convince mod_rewrite to leave our URLs alone, which seems
// fragile as that's an internal string literal in mod_rewrite.c and
// is not documented anywhwere.
//
// Another strategy is to return OK but leave request->filename NULL.
// In that case, the server kernel generates an ominious 'info'
// message:
//
//     [info] [client ::1] Module bug?  Request filename is missing for URI
//     /mod_pagespeed_statistics
//
// This is generated by httpd/src/server/request.c line 486, and right
// above that is this comment:
//
//     "OK" as a response to a real problem is not _OK_, but to
//     allow broken modules to proceed, we will permit the
//     not-a-path filename to pass the following two tests.  This
//     behavior may be revoked in future versions of Apache.  We
//     still must catch it later if it's heading for the core
//     handler.  Leave INFO notes here for module debugging.
//
// It seems like the simplest, most robust approach is to squirrel
// away the original URL *before* mod_rewrite sees it in
// kPagespeedOriginalUrl "mod_pagespeed_url" and use *that* rather than
// request->unparsed_uri (which mod_rewrite might have mangled) when
// procesing the request.
//
// Additionally we store whether or not this request is a pagespeed
// resource or not in kResourceUrlNote.
apr_status_t save_url_hook(request_rec *request) {

  // This call to MakeRequestUrl() not only returns the url but also
  // saves it for future use so that if another module changes the
  // url in the request, we still have the original one.
  const char* url = InstawebContext::MakeRequestUrl(request);

  StringPiece parsed_url(request->uri);
  bool bypass_mod_rewrite = false;
  // Note: We cannot use request->handler because it may not be set yet :(
  // TODO(sligocki): Make this robust to custom statistics and beacon URLs.
  // Note: we must compare against the parsed URL because unparsed_url has
  // ?ets=load:xx at the end for kBeaconHandler.
  if (parsed_url.ends_with(kStatisticsHandler) ||
      parsed_url.ends_with(kBeaconHandler) ||
      parsed_url.ends_with(kMessageHandler) ||
      parsed_url.ends_with(kRefererStatisticsHandler)) {
    bypass_mod_rewrite = true;
  } else {
    ApacheResourceManager* manager =
        InstawebContext::ManagerFromServerRec(request->server);
    RewriteDriver* rewrite_driver = manager->decoding_driver();
    RewriteFilter* filter;
    GoogleUrl gurl(url);
    OutputResourcePtr output_resource(
        rewrite_driver->DecodeOutputResource(gurl, &filter));
    if (output_resource.get() != NULL) {
      bypass_mod_rewrite = true;
    }
  }

  if (bypass_mod_rewrite) {
    apr_table_set(request->notes, kResourceUrlNote, kResourceUrlYes);
  } else {
    // Leave behind a note for non-instaweb requests that says that
    // our handler got called and we decided to pass.  This gives us
    // one final chance at serving resources in the presence of a
    // module that intercepted 'translate_name' before mod_pagespeed.
    // The absence of this marker indicates that translate_name did
    // not get a chance to run, and thus we should try to look at
    // the URI directly.
    apr_table_set(request->notes, kResourceUrlNote, kResourceUrlNo);
  }
  return DECLINED;
}

// Override core_map_to_storage for pagespeed resources.
apr_status_t instaweb_map_to_storage(request_rec* request) {
  apr_status_t ret = DECLINED;
  if (get_instaweb_resource_url(request) != NULL) {
    // core_map_to_storage does at least two things:
    //  1) checks filename length limits
    //  2) determines directory specific options
    // We want (2) but not (1).  If we simply return OK we will keep
    // core_map_to_storage from running and let through our long filenames but
    // resource requests that require regeneration will not respect directory
    // specific options.
    //
    // To fix this we need to be more dependent on apache internals than we
    // would like.  core_map_to_storage always calls ap_directory_walk(request),
    // which does both (1) and (2) and appears to work entirely off of
    // request->filename.  But ap_directory_walk doesn't care whether the last
    // request->segment of the path actually exists.  So if we change the
    // request->filename from something like:
    //    /var/www/path/to/LEAF_WHICH_MAY_BE_HUGE.pagespeed.FILTER.HASH.EXT
    // to:
    //    /var/www/path/to/A
    // then we will bypass the filename length limit without harming the load of
    // directory specific options.
    //
    // So: modify request->filename in place to cut it off after the last '/'
    // character and replace the whole leaf with 'A', and then call
    // ap_directory_walk to figure out custom options.
    int fname_length = strlen(request->filename);
    int last_slash = 0;
    for (int i = 0 ; i < fname_length ; i++) {
      if (request->filename[i] == '/') {
        last_slash = i;
      }
    }
    if (last_slash + 2 <= fname_length) {
      request->filename[last_slash + 1] = 'A';
      request->filename[last_slash + 2] = '\0';
    }
    ap_directory_walk(request);

    // mod_speling, if enabled, looks for the filename on the file system,
    // and tries to "correct" the spelling.  This is not desired for
    // mod_pagesped resources, but mod_speling will not do this damage
    // when request->filename == NULL.  See line 219 of
    // http://svn.apache.org/viewvc/httpd/httpd/trunk/modules/mappers/
    // mod_speling.c?revision=983065&view=markup
    //
    // Note that mod_speling runs 'hook_fixups' at APR_HOOK_LAST, and
    // we are currently running instaweb_map_to_storage in map_to_storage
    // HOOK_FIRST-2, which is a couple of phases before hook_fixups.
    //
    // If at some point we stop NULLing the filename here we need to modify the
    // code above that mangles it to use a temporary buffer instead.
    request->filename = NULL;

    // Keep core_map_to_storage from running and rejecting our long filenames.
    ret = OK;
  }
  return ret;
}

}  // namespace net_instaweb
