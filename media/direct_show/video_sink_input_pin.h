// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implement a DirectShow input pin used for receiving captured frames from
// a DirectShow Capture filter.

#ifndef MEDIA_DIRECT_SHOW_VIDEO_SINK_INPUT_PIN_H_
#define MEDIA_DIRECT_SHOW_VIDEO_SINK_INPUT_PIN_H_

#include "base/macros.h"
#include "media/capture/video/video_capture_device.h"
#include "media/direct_show/direct_show_pin_base.h"
#include "media/direct_show/video_sink_filter.h"
#include "media/direct_show/video_capture_types.h"

using media::directshow::DirectShowVideoCaptureFormat;

namespace media {

// Const used for converting Seconds to REFERENCE_TIME.
namespace directshow {
extern const REFERENCE_TIME kSecondsToReferenceTime;
} // namespace directshow

// Input pin of the SinkFilter.
class VideoSinkInputPin : public DirectShowPinBase {
 public:
  VideoSinkInputPin(IBaseFilter* filter, VideoSinkFilterObserver* observer);

  void SetRequestedMediaFormat(VideoPixelFormat pixel_format,
                               float frame_rate,
                               const BITMAPINFOHEADER& info_header);

  // Implement DirectShowPinBase.
  bool IsMediaTypeValid(const AM_MEDIA_TYPE* media_type) override;
  bool GetValidMediaType(int index, AM_MEDIA_TYPE* media_type) override;

  STDMETHOD(Receive)(IMediaSample* media_sample) override;

 private:
  ~VideoSinkInputPin() override;

  VideoPixelFormat requested_pixel_format_;
  float requested_frame_rate_;
  BITMAPINFOHEADER requested_info_header_;
  DirectShowVideoCaptureFormat resulting_format_;
  VideoSinkFilterObserver* observer_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(VideoSinkInputPin);
};

}  // namespace media

#endif  // MEDIA_DIRECT_SHOW_VIDEO_SINK_INPUT_PIN_H_
