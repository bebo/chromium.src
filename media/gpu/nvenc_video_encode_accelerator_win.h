// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NVENC_VIDEO_ENCODE_ACCELERATOR_WIN_H_
#define NVENC_VIDEO_ENCODE_ACCELERATOR_WIN_H_

#include "media/gpu/ffmpeg_base_video_encode_accelerator_win.h"

namespace media {

class NvEncVideoEncodeAccelerator : public FFMpegBaseVideoEncodeAccelerator {

 public:
  NvEncVideoEncodeAccelerator();


 protected:
  ~NvEncVideoEncodeAccelerator() override;

 private:
  void ConfigureFromRegistry();
  DISALLOW_COPY_AND_ASSIGN(NvEncVideoEncodeAccelerator);
};

}  // namespace media

#endif  // NVENC_VIDEO_ENCODE_ACCELERATOR_WIN_H_
