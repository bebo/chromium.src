// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Observer class of Sinkfilter. The implementor of this class receive video
// frames from the SinkFilter DirectShow filter.

#ifndef MEDIA_DIRECT_SHOW_VIDEO_SINK_FILTER_OBSERVER_H_
#define MEDIA_DIRECT_SHOW_VIDEO_SINK_FILTER_OBSERVER_H_

#include <stdint.h>

namespace media {

namespace directshow {
struct DirectShowVideoCaptureFormat;
} // namespace directshow

class VideoSinkFilterObserver {
 public:
  // SinkFilter will call this function with all frames delivered to it.
  // buffer in only valid during this function call.
  virtual void VideoFrameReceived(const uint8_t* buffer,
                                  int length,
                                  const directshow::DirectShowVideoCaptureFormat& format,
                                  base::TimeDelta timestamp) = 0;

  virtual void FormatChanged() {}; // FIXME parameter

 protected:
  virtual ~VideoSinkFilterObserver();
};

}  // namespace media

#endif  // MEDIA_DIRECT_SHOW_VIDEO_SINK_FILTER_OBSERVER_H_
