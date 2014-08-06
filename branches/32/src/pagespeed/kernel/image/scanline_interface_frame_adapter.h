/*
 * Copyright 2013 Google Inc.
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

// Author: Victor Chudnovsky

// This file provides two sets of adapters for use by
// {Scanline, MultipleFrame} clients wishing to use code provided by the
// {MultipleFrame, Scanline} classes.
//
// * Adapters from the MultipleFrame API to the Scanline API are
//   implemented by the classes FrameToScanlineReaderAdapter and
//   FrameToScanlineWriterAdapter.
//
// * Adapters from the Scanline API to the MultipleFrame API are
//   implemented by the classes ScanlineToFrameReaderAdapter and
//   ScanlineToFrameWriterAdapter.

#ifndef PAGESPEED_KERNEL_IMAGE_SCANLINE_INTERFACE_FRAME_ADAPTER_H_
#define PAGESPEED_KERNEL_IMAGE_SCANLINE_INTERFACE_FRAME_ADAPTER_H_

#include <cstddef>
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/image/image_frame_interface.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/scanline_interface.h"
#include "pagespeed/kernel/image/scanline_status.h"

namespace net_instaweb {

class MessageHandler;

}

namespace pagespeed {

namespace image_compression {

////////// MultipleFrame API to Scanline API adapters.


// The class FrameToScanlineReaderAdapter takes ownership of a
// MultipleFrameReader and exposes ScanlineReaderInterface methods.
class FrameToScanlineReaderAdapter : public ScanlineReaderInterface {
 public:
  // Acquires ownership of 'frame_reader'.
  explicit FrameToScanlineReaderAdapter(MultipleFrameReader* frame_reader);
  virtual ~FrameToScanlineReaderAdapter() {}

  virtual bool Reset();
  virtual size_t GetBytesPerScanline();
  virtual bool HasMoreScanLines();

  // Will return an error status if the underlying MultipleFrameReader
  // is processing an animated image.
  virtual ScanlineStatus InitializeWithStatus(const void* image_buffer,
                                              size_t buffer_length);
  virtual ScanlineStatus ReadNextScanlineWithStatus(
      void** out_scanline_bytes);
  virtual size_t GetImageHeight();
  virtual size_t GetImageWidth();
  virtual PixelFormat GetPixelFormat();

 private:
  scoped_ptr<MultipleFrameReader> impl_;

  ImageSpec image_spec_;
  FrameSpec frame_spec_;

  // True iff the frame spans every column in the image.
  bool image_and_frame_cols_coincide_;

  // True iff the frame spans every row in the image.
  bool image_and_frame_rows_coincide_;

  size_px current_image_row_;

  // For padding images where the frame does not occupy every pixel in
  // the image. These are set iff either of
  // image_and_frame_{cols,rows}_coincide_ are false.

  // Index of the first byte in the image row where the displayed
  // frame is located.
  size_t displayed_frame_first_col_byte_;

  // Width in bytes of the displayed frame.
  size_t displayed_frame_col_byte_width_;

  // Index of the first row of the displayed frame in the image.
  size_px displayed_frame_row_num_begin_;

  // Index of one past the last row of the displayed frame in the
  // image (same semantics as the STL end iterator).
  size_px displayed_frame_row_num_end_;

  // Width in bytes of the image.
  size_t image_width_bytes_;

  // Buffer of row data. If we need to pad rows with background
  // pixels, we do so here and return a pointer to this.
  net_instaweb::scoped_array<PixelRgbaChannels> row_buffer_;

  // Buffer of background pixel rows. If we need to pad rows with
  // background pixels, we first copy the following into row_buffer_
  // before we modify the read scanline there.
  net_instaweb::scoped_array<PixelRgbaChannels> row_template_;

  DISALLOW_COPY_AND_ASSIGN(FrameToScanlineReaderAdapter);
};

// The class FrameToScanlineWriterAdapter takes ownership of a
// MultipleFrameWriter and exposes ScanlineWriterInterface methods.
class FrameToScanlineWriterAdapter : public ScanlineWriterInterface {
 public:
  // Acquires ownership of 'frame_writer'.
  explicit FrameToScanlineWriterAdapter(MultipleFrameWriter* frame_writer);
  virtual ~FrameToScanlineWriterAdapter() {}

  virtual ScanlineStatus InitWithStatus(size_t width, size_t height,
                                        PixelFormat pixel_format);
  virtual ScanlineStatus InitializeWriteWithStatus(const void* config,
                                                   GoogleString* out);
  virtual ScanlineStatus WriteNextScanlineWithStatus(
      const void *scanline_bytes);
  virtual ScanlineStatus FinalizeWriteWithStatus();

 private:
  scoped_ptr<MultipleFrameWriter> impl_;

  bool init_done_;
  ImageSpec image_spec_;
  FrameSpec frame_spec_;

  DISALLOW_COPY_AND_ASSIGN(FrameToScanlineWriterAdapter);
};


////////// Scanline API to MultipleFrame API adapters.

// The class ScanlineToFrameReaderAdapter takes ownership of a
// ScanlineReaderInterface and exposes MultipleFrameReader methods.
class ScanlineToFrameReaderAdapter : public MultipleFrameReader {
 public:
  // Acquires ownership of 'scanline_reader'.
  ScanlineToFrameReaderAdapter(ScanlineReaderInterface* scanline_reader,
                               MessageHandler* message_handler);
  virtual ScanlineStatus Reset();
  virtual ScanlineStatus Initialize(const void* image_buffer,
                                    size_t buffer_length);
  virtual bool HasMoreFrames() const;
  virtual bool HasMoreScanlines() const;
  virtual ScanlineStatus PrepareNextFrame();
  virtual ScanlineStatus ReadNextScanline(const void** out_scanline_bytes);
  virtual ScanlineStatus GetFrameSpec(FrameSpec* frame_spec) const;
  virtual ScanlineStatus GetImageSpec(ImageSpec* image_spec) const;

 private:
  enum {
    UNINITIALIZED = 0,
    INITIALIZED,
    FRAME_PREPARED,

    ERROR
  } state_;

  ImageSpec image_spec_;
  FrameSpec frame_spec_;

  scoped_ptr<ScanlineReaderInterface> impl_;

  DISALLOW_COPY_AND_ASSIGN(ScanlineToFrameReaderAdapter);
};

// The class ScanlineToFrameWriterAdapter takes ownership of a
// ScanlineWriterInterface and exposes MultipleFrameWriter methods.
class ScanlineToFrameWriterAdapter : public MultipleFrameWriter {
 public:
  // Acquires ownership of 'scanline_writer'.
  ScanlineToFrameWriterAdapter(ScanlineWriterInterface* scanline_writer,
                               MessageHandler* handler);

  virtual ScanlineStatus Initialize(const void* config,
                                    GoogleString* out);
  virtual ScanlineStatus PrepareImage(const ImageSpec* image_spec);
  virtual ScanlineStatus PrepareNextFrame(const FrameSpec* frame_spec);
  virtual ScanlineStatus WriteNextScanline(const void *scanline_bytes);
  virtual ScanlineStatus FinalizeWrite();

 private:
  enum {
    UNINITIALIZED = 0,
    INITIALIZED,
    IMAGE_PREPARED,
    FRAME_PREPARED,

    ERROR
  } state_;
  const ImageSpec* image_spec_;
  const FrameSpec* frame_spec_;

  scoped_ptr<ScanlineWriterInterface> impl_;

  const void* config_;
  GoogleString* out_;

  DISALLOW_COPY_AND_ASSIGN(ScanlineToFrameWriterAdapter);
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_KERNEL_IMAGE_SCANLINE_INTERFACE_FRAME_ADAPTER_H_
