// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/qsv_video_encode_accelerator_win.h"

#include "base/strings/utf_string_conversions.h"
#include "base/win/registry.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#include "libavutil/rational.h"
}

using base::win::RegKey;

namespace media {

QsvVideoEncodeAccelerator::QsvVideoEncodeAccelerator():
  FFMpegBaseVideoEncodeAccelerator("libQsv") {
}

QsvVideoEncodeAccelerator::~QsvVideoEncodeAccelerator() {}

void QsvVideoEncodeAccelerator::ConfigureFromRegistry() {
  RegKey beboKey(HKEY_CURRENT_USER, L"SOFTWARE\\Bebo\\App", KEY_READ);

  std::string preset = "fast";
  std::string tune = "";
  DWORD max_b_frames = 0;
  DWORD rc_buffer_size = 100000;
  DWORD crf = 20;
  DWORD global_quality = 0;
  DWORD twopass = 0;

  if (beboKey.Valid()) {
    if (beboKey.HasValue(L"preset")) {
      std::wstring value;
      beboKey.ReadValue(L"preset", &value);
      preset = base::WideToUTF8(value);
    }

    if (beboKey.HasValue(L"max_b_frames")) {
       beboKey.ReadValueDW(L"max_b_frames", &max_b_frames);
    }

    if (beboKey.HasValue(L"rc_buffer_size")) {
       beboKey.ReadValueDW(L"rc_buffer_size", &rc_buffer_size);
    }

    if (beboKey.HasValue(L"crf")) {
       beboKey.ReadValueDW(L"crf", &crf);
    }

    if (beboKey.HasValue(L"global_quality")) {
       beboKey.ReadValueDW(L"global_quality", &global_quality);
    }

    if (beboKey.HasValue(L"twopass")) {
       beboKey.ReadValueDW(L"twopass", &twopass);
    }

    if (beboKey.HasValue(L"tune")) {
      std::wstring value;
      beboKey.ReadValue(L"tune", &value);
      tune= base::WideToUTF8(value);
    }
  }

  if (codec_->id == AV_CODEC_ID_H264) {
    av_opt_set(avc_context_->priv_data, "preset", preset.c_str(), 0);
    LOG(INFO) << "preset: " << preset;
  }

  if (tune.length() > 0) {
    av_opt_set(avc_context_->priv_data, "tune", tune.c_str(), 0);
    LOG(INFO) << "tune: " << tune;
  }

  avc_context_->max_b_frames = max_b_frames;
  LOG(INFO) << "max_b_frames: " << avc_context_->max_b_frames;

  if (crf > 0) {
    av_opt_set_int(avc_context_->priv_data, "crf", 23, 0);
    LOG(INFO) << "crf: " << crf;
  } else {
    av_opt_set_int(avc_context_->priv_data, "cbr", true, 0);
    LOG(INFO) << "cbr: true";
  }

  if (rc_buffer_size > 0) {
    avc_context_->rc_buffer_size = rc_buffer_size;
    LOG(INFO) << "rc_buffer_size: " << rc_buffer_size;
  }

  if (global_quality > 0)  {
    avc_context_->global_quality = global_quality;
    LOG(INFO) << "global_quality: " << global_quality;
  }

  if (twopass > 0) {
    av_opt_set_int(avc_context_->priv_data, "2pass", twopass, 0);
    LOG(INFO) << "2pass: " << twopass;
  }

  // crf_max
  // cqp
}

}  // namespace media
