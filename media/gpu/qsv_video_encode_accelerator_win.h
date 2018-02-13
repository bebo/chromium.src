// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef QSV_VIDEO_ENCODE_ACCELERATOR_WIN_H_
#define QSV_VIDEO_ENCODE_ACCELERATOR_WIN_H_

#include "media/gpu/ffmpeg_base_video_encode_accelerator_win.h"

namespace media {

class QsvVideoEncodeAccelerator : public FFMpegBaseVideoEncodeAccelerator {

 public:
  QsvVideoEncodeAccelerator();


 protected:
  ~QsvVideoEncodeAccelerator() override;

 private:
  void ConfigureFromRegistry() override;
  DISALLOW_COPY_AND_ASSIGN(QsvVideoEncodeAccelerator);
};

}  // namespace media

#endif  // Qsv_VIDEO_ENCODE_ACCELERATOR_WIN_H_
