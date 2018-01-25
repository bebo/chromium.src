// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/capture/video/win/video_capture_device_ds_av_win.h"


#include <ks.h>
#include <ksmedia.h>
#include <objbase.h>

#include <algorithm>
#include <list>
#include <utility>

#include "base/macros.h"
#include "base/strings/sys_string_conversions.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_variant.h"
#include "media/base/timestamp_constants.h"
#include "media/capture/video/win/sink_filter_win.h" // for pixel GUIDs
#include "media/capture/video/blob_utils.h"
#include "media/capture/video/win/video_capture_device_factory_win.h"
#include "media/direct_show/direct_show_device_factory.h"
#include "media/direct_show/direct_show.h"
#include "media/direct_show/video_capture_types.h"
#include "media/direct_show/capability_list_win.h"

using base::win::ScopedCoMem;
using base::win::ScopedComPtr;
using base::win::ScopedVariant;
using media::directshow::DirectShowVideoCaptureFormat;

extern GUID kMediaSubTypeI420;
extern GUID kMediaSubTypeHDYC;
extern GUID kMediaSubTypeZ16;
extern GUID kMediaSubTypeINVZ;
extern GUID kMediaSubTypeY16;

namespace media {

#if DCHECK_IS_ON()
#define DLOG_IF_FAILED_WITH_HRESULT(message, hr)                      \
  {                                                                   \
    DLOG_IF(ERROR, FAILED(hr))                                        \
        << (message) << ": " << logging::SystemErrorCodeToString(hr); \
  }
#else
#define DLOG_IF_FAILED_WITH_HRESULT(message, hr) \
  {}
#endif

// static
void VideoCaptureDeviceDirectShowAV::GetDeviceCapabilityList(
    const std::string& device_id,
    bool query_detailed_frame_rates,
    CapabilityList* out_capability_list) {

  DirectShowVideoCapabilityList ds_caps; 
  DirectShow::GetDeviceVideoCapabilityList(
      device_id,
      query_detailed_frame_rates,
      &ds_caps);

  for (const auto cap: ds_caps) {
    VideoCaptureFormat format(cap.supported_format.frame_size,
      cap.supported_format.frame_rate,
      cap.supported_format.pixel_format);
    out_capability_list->emplace_back(cap.stream_index, format, cap.info_header);
  }
}


VideoCaptureDeviceDirectShowAV::VideoCaptureDeviceDirectShowAV(
    const VideoCaptureDeviceDescriptor& device_descriptor)
    : device_descriptor_(device_descriptor),
      state_(kIdle),
      direct_show_(NULL) {
  // LOG(INFO) << device_descriptor_.display_name << " " << __func__;
  // TODO(mcasas): Check that CoInitializeEx() has been called with the
  // appropriate Apartment model, i.e., Single Threaded.
}

VideoCaptureDeviceDirectShowAV::~VideoCaptureDeviceDirectShowAV() {
  DCHECK(thread_checker_.CalledOnValidThread());
  // LOG(INFO) << device_descriptor_.display_name << " " << __func__;
}

bool VideoCaptureDeviceDirectShowAV::Init() {
  DCHECK(thread_checker_.CalledOnValidThread());
  // LOG(INFO) << device_descriptor_.display_name << " " << __func__;

  if (direct_show_ == NULL) {
    direct_show_ = DirectShowDeviceFactory::
      GetInstance()->GetController(device_descriptor_.device_id);
  }

  return true;
}

void VideoCaptureDeviceDirectShowAV::AllocateAndStart(
    const VideoCaptureParams& params,
    std::unique_ptr<VideoCaptureDevice::Client> client) {
  DCHECK(thread_checker_.CalledOnValidThread());
  // LOG(INFO) << device_descriptor_.display_name << " " << __func__;
  if (state_ != kIdle)
    return;

  client_ = std::move(client);

  direct_show_->SetRequestedVideoFormat(params.requested_format);
  direct_show_->RegisterObserver(this);

  client_->OnStarted();
  state_ = kCapturing;
}

void VideoCaptureDeviceDirectShowAV::StopAndDeAllocate() {
  DCHECK(thread_checker_.CalledOnValidThread());
  // LOG(INFO) << device_descriptor_.display_name << " " << __func__;
  if (state_ != kCapturing)
    return;

  direct_show_->UnregisterObserver(this);

  client_.reset();
  state_ = kIdle;
}

void VideoCaptureDeviceDirectShowAV::TakePhoto(TakePhotoCallback callback) {
}

void VideoCaptureDeviceDirectShowAV::GetPhotoState(GetPhotoStateCallback callback) {
}

void VideoCaptureDeviceDirectShowAV::SetPhotoOptions(
    mojom::PhotoSettingsPtr settings,
    VideoCaptureDevice::SetPhotoOptionsCallback callback) {
}

bool VideoCaptureDeviceDirectShowAV::InitializeVideoAndCameraControls() {
  return false;
}

// Implements SinkFilterObserver::SinkFilterObserver.
void VideoCaptureDeviceDirectShowAV::VideoFrameReceived(const uint8_t* buffer,
                                          int length,
                                          const DirectShowVideoCaptureFormat& format,
                                          base::TimeDelta timestamp) {
  if (first_ref_time_.is_null())
    first_ref_time_ = base::TimeTicks::Now();

  // There is a chance that the platform does not provide us with the timestamp,
  // in which case, we use reference time to calculate a timestamp.
  if (timestamp == media::kNoTimestamp)
    timestamp = base::TimeTicks::Now() - first_ref_time_;


  // FIXME: VideoPixelStorage mapping format.pixel_storage;
  VideoPixelStorage actual_pixel_storage = VideoPixelStorage::PIXEL_STORAGE_CPU; 
  VideoCaptureFormat actual_format(format.frame_size,
    format.frame_rate, format.pixel_format, actual_pixel_storage);

  client_->OnIncomingCapturedData(buffer, length, actual_format, 0,
                                  base::TimeTicks::Now(), timestamp);
}

void VideoCaptureDeviceDirectShowAV::FormatChanged() {
}

void VideoCaptureDeviceDirectShowAV::SetErrorState(const base::Location& from_here,
    const std::string& reason,
    HRESULT hr) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DLOG_IF_FAILED_WITH_HRESULT(reason, hr);
  state_ = kError;
  client_->OnError(from_here, reason);
}

}  // namespace media
