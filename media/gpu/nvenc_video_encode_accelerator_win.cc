// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/nvenc_video_encode_accelerator_win.h"

/* #pragma warning(push) */
/* #pragma warning(disable : 4800)  // Disable warning for added padding. */

#include "base/strings/utf_string_conversions.h"
#include "base/win/registry.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#include "libavutil/rational.h"
}

using base::win::RegKey;

namespace media {

NvEncVideoEncodeAccelerator::NvEncVideoEncodeAccelerator():
  //TODO - the nvenc encoder name is deprecated - should use h264_nvenc - but
  //need to rebuild ffmpeg to expose... and skipping the 4.5 h for now
  /* FFMpegBaseVideoEncodeAccelerator("h264_nvenc") { */
  FFMpegBaseVideoEncodeAccelerator("nvenc", AV_PIX_FMT_YUV420P) {
}

NvEncVideoEncodeAccelerator::~NvEncVideoEncodeAccelerator() {}

void NvEncVideoEncodeAccelerator::ConfigureFromRegistry() {
  RegKey beboKey(HKEY_CURRENT_USER, L"SOFTWARE\\Bebo\\App", KEY_READ);

  std::string nvenc_preset = "slow";
  std::string nvenc_vprofile = "main";
  std::string nvenc_rc = "";

  if (beboKey.Valid()) {
    if (beboKey.HasValue(L"nvenc_preset")) {
      std::wstring value;
      beboKey.ReadValue(L"nvenc_preset", &value);
      nvenc_preset = base::WideToUTF8(value);
    }

    if (beboKey.HasValue(L"nvenc_vprofile")) {
      std::wstring value;
      beboKey.ReadValue(L"nvenc_vprofile", &value);
      nvenc_vprofile = base::WideToUTF8(value);
    }

    if (beboKey.HasValue(L"nvenc_rc")) {
      std::wstring value;
      beboKey.ReadValue(L"nvenc_rc", &value);
      nvenc_rc = base::WideToUTF8(value);
    }
  }

  if (codec_->id == AV_CODEC_ID_H264) {
    av_opt_set(avc_context_->priv_data, "preset", nvenc_preset.c_str(), 0);
    LOG(INFO) << "preset: " << nvenc_preset;
    av_opt_set(avc_context_->priv_data, "vprofile", nvenc_vprofile.c_str(), 0);
    LOG(INFO) << "vprofile: " << nvenc_vprofile;
  }
  if (nvenc_rc.length() > 0) {
    av_opt_set(avc_context_->priv_data, "rc", nvenc_rc.c_str(), 0);
    LOG(INFO) << "rc: " << nvenc_rc;
  }

  // crf_max
  // cqp
}

}  // namespace media
