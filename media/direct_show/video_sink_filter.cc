// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/direct_show/video_sink_filter.h"

#include "base/logging.h"
#include "media/direct_show/video_sink_input_pin.h"

namespace media {

namespace directshow {
// Define GUID for I420. This is the color format we would like to support but
// it is not defined in the DirectShow SDK.
// http://msdn.microsoft.com/en-us/library/dd757532.aspx
// 30323449-0000-0010-8000-00AA00389B71.
GUID kMediaSubTypeI420 = {0x30323449,
                          0x0000,
                          0x0010,
                          {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// UYVY synonym with BT709 color components, used in HD video. This variation
// might appear in non-USB capture cards and it's implemented as a normal YUV
// pixel format with the characters HDYC encoded in the first array word.
GUID kMediaSubTypeHDYC = {0x43594448,
                          0x0000,
                          0x0010,
                          {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

GUID kMediaSubTypeZ16 = {0x2036315a,
                         0x0000,
                         0x0010,
                         {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
GUID kMediaSubTypeINVZ = {0x5a564e49,
                          0x2d90,
                          0x4a58,
                          {0x92, 0x0b, 0x77, 0x3f, 0x1f, 0x2c, 0x55, 0x6b}};
GUID kMediaSubTypeY16 = {0x20363159,
                         0x0000,
                         0x0010,
                         {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
} // namespace directshow


#if 0
VideoSinkFilterObserver::~VideoSinkFilterObserver() {
}
#endif

VideoSinkFilter::VideoSinkFilter(VideoSinkFilterObserver* observer) : input_pin_(NULL) {
  input_pin_ = new VideoSinkInputPin(this, observer);
}

void VideoSinkFilter::SetRequestedMediaFormat(VideoPixelFormat pixel_format,
                                         float frame_rate,
                                         const BITMAPINFOHEADER& info_header) {
  input_pin_->SetRequestedMediaFormat(pixel_format, frame_rate, info_header);
}

size_t VideoSinkFilter::NoOfPins() {
  return 1;
}

IPin* VideoSinkFilter::GetPin(int index) {
  return index == 0 ? input_pin_.get() : NULL;
}

STDMETHODIMP VideoSinkFilter::GetClassID(CLSID* clsid) {
  *clsid = __uuidof(VideoSinkFilter);
  return S_OK;
}

VideoSinkFilter::~VideoSinkFilter() {
  input_pin_->SetOwner(NULL);
}

}  // namespace media
