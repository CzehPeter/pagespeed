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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_SINGLE_RESOURCE_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_SINGLE_RESOURCE_FILTER_H_

#include "base/basictypes.h"
#include "net/instaweb/rewriter/public/rewrite_filter.h"
#include "net/instaweb/rewriter/public/output_resource.h"

namespace net_instaweb {

// A helper base class for RewriteFilters which only convert one input resource
// to one output resource. This class helps implement both HTML rewriting
// and Fetch in terms of a single RewriteLoadedResource method, and takes
// care of resource management and caching.
//
// Subclasses should implement RewriteLoadedResource and call
// Rewrite*WithCaching when rewriting HTML using the returned
// CachedResult (which may be NULL) to get rewrite results.
class RewriteSingleResourceFilter : public RewriteFilter {
 public:
  explicit RewriteSingleResourceFilter(
      RewriteDriver* driver, StringPiece filter_prefix)
      : RewriteFilter(driver, filter_prefix) {
  }
  virtual ~RewriteSingleResourceFilter();

  virtual bool Fetch(OutputResource* output_resource,
                     Writer* response_writer,
                     const RequestHeaders& request_header,
                     ResponseHeaders* response_headers,
                     MessageHandler* message_handler,
                     UrlAsyncFetcher::Callback* callback);

  enum RewriteResult {
    kRewriteFailed,  // rewrite is impossible or undesirable
    kRewriteOk,  // rewrite went fine
    kTooBusy   // the system is temporarily too busy to handle this
               // rewrite request; no conclusion can be drawn on whether
               // it's worth trying again or not.
  };

  // Rewrite the given resource using this filter's RewriteLoadedResource,
  // taking  advantage of various caching techniques to avoid recomputation
  // whenever possible.
  //
  // If your filter code and the original URL are enough to produce your output
  // pass in resource_manager_->url_escaper() into encoder. If not, pass in
  // an encoder that incorporates any other settings into the output URL.
  //
  // If nothing can be done (as the input data hasn't been fetched in time
  // and we do not have cached output) returns NULL. Otherwise returns
  // a CachedResult stating whether the resource is optimizable, and if so at
  // what URL the output is, along with any metadata that was stored when
  // examining it.
  //
  // Note: The metadata may be useful even when optimizable() is false.
  // For example a filter could store dimensions of an image in them, even
  // if it chose to not change it, so any <img> tags can be given appropriate
  // width and height.
  //
  // Precondition: in != NULL, in is security-checked
  OutputResource::CachedResult* RewriteResourceWithCaching(
      Resource* in, UrlSegmentEncoder* encoder);

  // Variant of above, using default encoder.
  OutputResource::CachedResult* RewriteResourceWithCaching(Resource* in);

 protected:
  // Variant of the above that makes and cleans up input resource for in_url.
  // Note that the URL will be expanded and security checked with respect to the
  // current base URL for the HTML parser.
  OutputResource::CachedResult* RewriteWithCaching(const StringPiece& in_url,
                                                   UrlSegmentEncoder* encoder);

  // RewriteSingleResourceFilter will make sure to disregard any written
  // cache data with a version number different from what this method returns.
  //
  // Filters should increase this version when they add some new
  // metadata they rely on to do proper optimization or when
  // the quality of their optimization has increased significantly from
  // previous version.
  //
  // The default implementation returns 0.
  virtual int FilterCacheFormatVersion() const;

  // Derived classes must implement this function instead of Fetch.
  //
  // The last parameter gets the UrlSegmentEncoder used to encode or decode
  // the output URL.
  //
  // If rewrite succeeds, make sure to set the content-type on the output
  // resource, call ResourceManager::Write, and return kRewriteOk.
  //
  // If rewrite fails, simply return kRewriteFailed.
  //
  // In case it would be inadvisable to run the rewrite due to external
  // factors such as system load (rather than contents of the input)
  // return kTooBusy.
  virtual RewriteResult RewriteLoadedResource(const Resource* input_resource,
                                              OutputResource* output_resource,
                                              UrlSegmentEncoder* encoder) = 0;

  // If the filter does any custom encoding of result URLs it should
  // override this method to return a fresh, non-NULL UrlSegmentEncoder object
  // to use to help decode the URL for a Fetch. The RewriteSingleResourceFilter
  // class will take and hold ownership of this object.
  //
  // The default implementation returns NULL which makes
  // resource_manager_->url_escaper() be used.
  virtual UrlSegmentEncoder* CreateUrlEncoderForFetch() const;

 private:
  class FetchCallback;
  friend class RewriteSingleResourceFilterTest;

  // Check and record whether metadata version matches
  // FilterCacheFormatVersion() respectively.
  bool IsValidCacheFormat(OutputResource::CachedResult* cached);
  void UpdateCacheFormat(OutputResource* output_resource);

  // Metadata key we use to store the input timestamp.
  static const char kInputTimestampKey[];

  // Tries to rewrite input_resource to output_resource, and if successful
  // updates the cache as appropriate. Does not call WriteUnoptimizable on
  // failure.
  RewriteResult RewriteLoadedResourceAndCacheIfOk(
      const Resource* input_resource, OutputResource* output_resource,
      UrlSegmentEncoder* encoder);

  // Records that rewrite of input -> output failed (either due to
  // unavailability of input or failed conversion).
  void CacheRewriteFailure(const Resource* input_resource,
                           OutputResource* output_resource,
                           MessageHandler* message_handler);

  DISALLOW_COPY_AND_ASSIGN(RewriteSingleResourceFilter);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_REWRITE_SINGLE_RESOURCE_FILTER_H_
