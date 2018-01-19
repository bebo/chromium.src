// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef X264_VIDEO_ENCODE_ACCELERATOR_WIN_H_
#define X264_VIDEO_ENCODE_ACCELERATOR_WIN_H_

#include <mfapi.h>
#include <mfidl.h>
#include <stdint.h>
#include <strmif.h>

#include <memory>

#include "base/atomic_ref_count.h"
#include "base/bind.h"
#include "base/containers/circular_deque.h"
#include "base/memory/weak_ptr.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/win/scoped_comptr.h"
#include "media/gpu/media_gpu_export.h"
#include "media/video/video_encode_accelerator.h"

namespace media {


}  // namespace media

#endif  // MEDIA_GPU_MEDIA_FOUNDATION_VIDEO_ENCODE_ACCELERATOR_WIN_H_
