// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Windows specific implementation of VideoCaptureDevice.
// DirectShow is used for capturing. DirectShow provide its own threads
// for capturing.

#ifndef MEDIA_DIRECT_SHOW_CAPABILITY_LIST_WIN_H_
#define MEDIA_DIRECT_SHOW_CAPABILITY_LIST_WIN_H_

#include <list>
#include <windows.h>

#include "media/direct_show/video_capture_types.h"

using media::directshow::DirectShowVideoCaptureFormat;
namespace media {

struct DirectShowDeviceCapability {
  DirectShowDeviceCapability(int index, const DirectShowVideoCaptureFormat& format)
      : stream_index(index), supported_format(format), info_header() {}

  // Used by VideoCaptureDeviceWin.
  DirectShowDeviceCapability(int index,
                const DirectShowVideoCaptureFormat& format,
                const BITMAPINFOHEADER& info_header)
      : stream_index(index),
        supported_format(format),
        info_header(info_header) {}

  const int stream_index;
  const DirectShowVideoCaptureFormat supported_format;

  // |info_header| is only valid if DirectShow is used.
  const BITMAPINFOHEADER info_header;
};

typedef std::list<DirectShowDeviceCapability> DirectShowDeviceCapabilityList;

const DirectShowDeviceCapability& GetBestMatchedCapability(
    const DirectShowVideoCaptureFormat& requested,
    const DirectShowDeviceCapabilityList& capabilities);

}  // namespace media

#endif  // MEDIA_DIRECT_SHOW_CAPABILITY_LIST_WIN_H_
