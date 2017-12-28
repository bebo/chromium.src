// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/direct_show/capability_list_win.h"

#include <algorithm>
#include <functional>

#include "base/logging.h"
#include "media/direct_show/video_capture_types.h"

using namespace media::directshow;

namespace media {

static bool CompareCapability(const DirectShowVideoCaptureFormat& requested,
                              const DirectShowVideoCapability& capability_lhs,
                              const DirectShowVideoCapability& capability_rhs) {
  const DirectShowVideoCaptureFormat& lhs = capability_lhs.supported_format;
  const DirectShowVideoCaptureFormat& rhs = capability_rhs.supported_format;

  const int diff_height_lhs =
      std::abs(lhs.frame_size.height() - requested.frame_size.height());
  const int diff_height_rhs =
      std::abs(rhs.frame_size.height() - requested.frame_size.height());
  if (diff_height_lhs != diff_height_rhs)
    return diff_height_lhs < diff_height_rhs;

  const int diff_width_lhs =
      std::abs(lhs.frame_size.width() - requested.frame_size.width());
  const int diff_width_rhs =
      std::abs(rhs.frame_size.width() - requested.frame_size.width());
  if (diff_width_lhs != diff_width_rhs)
    return diff_width_lhs < diff_width_rhs;

  const float diff_fps_lhs = std::fabs(lhs.frame_rate - requested.frame_rate);
  const float diff_fps_rhs = std::fabs(rhs.frame_rate - requested.frame_rate);
  if (diff_fps_lhs != diff_fps_rhs)
    return diff_fps_lhs < diff_fps_rhs;

  return DirectShowVideoCaptureFormat::ComparePixelFormatPreference(lhs.pixel_format,
                                                          rhs.pixel_format);
}

static unsigned long absdiff(unsigned long a, unsigned long b) {
  return (a > b) ? (a - b) : (b - a);
}

static bool CompareCapability(const WAVEFORMATEX& requested,
                              const DirectShowAudioCapability& capability_lhs,
                              const DirectShowAudioCapability& capability_rhs) {
  const WAVEFORMATEX& lhs = capability_lhs.supported_format;
  const WAVEFORMATEX& rhs = capability_rhs.supported_format;

  const unsigned long diff_samplerate_lhs = absdiff(
      lhs.nSamplesPerSec, requested.nSamplesPerSec);
  const unsigned long diff_samplerate_rhs = absdiff(
      rhs.nSamplesPerSec, requested.nSamplesPerSec);
  if (diff_samplerate_lhs != diff_samplerate_rhs)
    return diff_samplerate_lhs < diff_samplerate_rhs;

  const int diff_channel_lhs =
      std::abs(lhs.nChannels - requested.nChannels);
  const int diff_channel_rhs =
      std::abs(rhs.nChannels - requested.nChannels);
  if (diff_channel_lhs != diff_channel_rhs)
    return diff_channel_lhs < diff_channel_rhs;

  // check pcm?
  return true;
}


const DirectShowVideoCapability& GetBestMatchedCapability(
    const DirectShowVideoCaptureFormat& requested,
    const DirectShowVideoCapabilityList& capabilities) {
  DCHECK(!capabilities.empty());
  const DirectShowVideoCapability* best_match = &(*capabilities.begin());
  for (const DirectShowVideoCapability& capability : capabilities) {
    if (CompareCapability(requested, capability, *best_match))
      best_match = &capability;
  }
  return *best_match;
}

const DirectShowAudioCapability& GetBestMatchedCapability(
    const WAVEFORMATEX& requested,
    const DirectShowAudioCapabilityList& capabilities) {
  const DirectShowAudioCapability* best_match = &(*capabilities.begin());
  for (const DirectShowAudioCapability& capability : capabilities) {
    if (CompareCapability(requested, capability, *best_match))
      best_match = &capability;
  }
  return *best_match;
}


}  // namespace media
