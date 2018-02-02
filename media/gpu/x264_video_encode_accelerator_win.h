// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef X264_VIDEO_ENCODE_ACCELERATOR_WIN_H_
#define X264_VIDEO_ENCODE_ACCELERATOR_WIN_H_

#include "media/gpu/ffmpeg_base_video_encode_accelerator_win.h"

namespace media {

class X264VideoEncodeAccelerator : public FFMpegBaseVideoEncodeAccelerator {

 public:
  X264VideoEncodeAccelerator();


 protected:
  ~X264VideoEncodeAccelerator() override;

 private:
  void ConfigureFromRegistry() override;
  DISALLOW_COPY_AND_ASSIGN(X264VideoEncodeAccelerator);
};

}  // namespace media

#endif  // X264_VIDEO_ENCODE_ACCELERATOR_WIN_H_
