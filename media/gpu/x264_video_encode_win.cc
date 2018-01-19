// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/x264_video_encode_win.h"

#pragma warning(push)
#pragma warning(disable : 4800)  // Disable warning for added padding.

#include <codecapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <objbase.h>

#include <iterator>
#include <utility>
#include <vector>

#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "base/win/registry.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_variant.h"
#include "base/win/windows_version.h"
#include "media/base/win/mf_helpers.h"
#include "media/base/win/mf_initializer.h"
#include "media/base/win/mf_initializer.h"
#include "base/strings/utf_string_conversions.h"
#include "third_party/libyuv/include/libyuv.h"

using base::win::ScopedComPtr;
using media::mf::MediaBufferScopedPointer;
using base::win::RegKey;

#define BVLOG VLOG

namespace media {

}  // namespace content
