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
                              const DirectShowDeviceCapability& capability_lhs,
                              const DirectShowDeviceCapability& capability_rhs) {
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

const DirectShowDeviceCapability& GetBestMatchedCapability(
    const DirectShowVideoCaptureFormat& requested,
    const DirectShowDeviceCapabilityList& capabilities) {
  DCHECK(!capabilities.empty());
  const DirectShowDeviceCapability* best_match = &(*capabilities.begin());
  for (const DirectShowDeviceCapability& capability : capabilities) {
    if (CompareCapability(requested, capability, *best_match))
      best_match = &capability;
  }
  return *best_match;
}

}  // namespace media
