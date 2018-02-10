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
  std::string nvenc_profile = "high";
  std::string nvenc_rc = "vbr_hq";
  DWORD nvenc_rc_lookahead = 10;
  DWORD nvenc_temporal_aq = 1;
  DWORD rc_buffer_size_ms = 2000;
  DWORD rc_max_rate_pct = 110; // set max to avg bitrate * x/100
  DWORD rc_min_rate_pct = 0; // set min to avg bitrate * x/100

  if (beboKey.Valid()) {

    if (beboKey.HasValue(L"nvenc_rc_lookahead")) {
      beboKey.ReadValueDW(L"nvenc_rc_lookahead", &nvenc_rc_lookahead);
    }

    if (beboKey.HasValue(L"rc_buffer_size_ms")) {
      beboKey.ReadValueDW(L"rc_buffer_size_ms", &rc_buffer_size_ms);
    }
    rc_buffer_size_ms_ = rc_buffer_size_ms;

    if (beboKey.HasValue(L"rc_min_rate_pct")) {
      beboKey.ReadValueDW(L"rc_min_rate_pct", &rc_min_rate_pct);
    }
    rc_min_rate_pct_ = rc_min_rate_pct;

    if (beboKey.HasValue(L"rc_max_rate_pct")) {
      beboKey.ReadValueDW(L"rc_max_rate_pct", &rc_max_rate_pct);
    }
    rc_max_rate_pct_ = rc_max_rate_pct;

    if (beboKey.HasValue(L"nvenc_temporal_aq")) {
      beboKey.ReadValueDW(L"nvenc_temporal_aq", &nvenc_temporal_aq);
    }

    if (beboKey.HasValue(L"nvenc_preset")) {
      std::wstring value;
      beboKey.ReadValue(L"nvenc_preset", &value);
      nvenc_preset = base::WideToUTF8(value);
    }

    if (beboKey.HasValue(L"nvenc_profile")) {
      std::wstring value;
      beboKey.ReadValue(L"nvenc_profile", &value);
      nvenc_profile = base::WideToUTF8(value);
    }

    if (beboKey.HasValue(L"nvenc_rc")) {
      std::wstring value;
      beboKey.ReadValue(L"nvenc_rc", &value);
      nvenc_rc = base::WideToUTF8(value);
    }
  }

  if (codec_->id == AV_CODEC_ID_H264) {
    av_opt_set(avc_context_->priv_data, "preset", nvenc_preset.c_str(), 0);
    LOG(INFO) << "nvenc_preset: " << nvenc_preset;
    av_opt_set(avc_context_->priv_data, "profile", nvenc_profile.c_str(), 0);
    LOG(INFO) << "nvenc_profile: " << nvenc_profile;
  }
  if (nvenc_rc.length() > 0) {
    av_opt_set(avc_context_->priv_data, "rc", nvenc_rc.c_str(), 0);
    LOG(INFO) << "nvenc_rc: " << nvenc_rc;
  }

  if (nvenc_rc_lookahead != 0) {
    av_opt_set_int(avc_context_->priv_data, "rc-lookahead", nvenc_rc_lookahead, 0);
    LOG(INFO) << implementation_name_ << " rc-lookahead: " << nvenc_rc_lookahead;
  }

  if (nvenc_temporal_aq != 0) {
    av_opt_set_int(avc_context_->priv_data, "temporal-aq", nvenc_temporal_aq, 0);
    LOG(INFO) << implementation_name_ << " temporal-aq: " << nvenc_temporal_aq;
  }

  LOG(INFO) << implementation_name_ << " rc_max_rate_pct: " << rc_max_rate_pct_
            << " rc_min_rate_pct: " << rc_min_rate_pct_
            << " rc_buffer_size_ms: " << rc_buffer_size_ms_ ;
}

}  // namespace media
