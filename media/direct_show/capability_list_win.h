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
#include <mmreg.h>

#include "media/direct_show/video_capture_types.h"

using media::directshow::DirectShowVideoCaptureFormat;
namespace media {

// video

struct DirectShowVideoCapability {
  DirectShowVideoCapability(int index, const DirectShowVideoCaptureFormat& format)
      : stream_index(index), supported_format(format), info_header() {}

  // Used by VideoCaptureDeviceWin.
  DirectShowVideoCapability(int index,
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

typedef std::list<DirectShowVideoCapability> DirectShowVideoCapabilityList;

const DirectShowVideoCapability& GetBestMatchedCapability(
    const DirectShowVideoCaptureFormat& requested,
    const DirectShowVideoCapabilityList& capabilities);

// audio 

struct DirectShowAudioCapability {
  DirectShowAudioCapability(int index, const WAVEFORMATEX& format)
      : stream_index(index), supported_format(format) {}

  const int stream_index;
  const WAVEFORMATEX supported_format;
};

typedef std::list<DirectShowAudioCapability> DirectShowAudioCapabilityList;

const DirectShowAudioCapability& GetBestMatchedCapability(
    const WAVEFORMATEX& requested,
    const DirectShowAudioCapabilityList& capabilities);


}  // namespace media

#endif  // MEDIA_DIRECT_SHOW_CAPABILITY_LIST_WIN_H_
