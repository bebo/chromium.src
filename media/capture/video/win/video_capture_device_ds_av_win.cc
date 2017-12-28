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
      white_balance_mode_manual_(false),
      exposure_mode_manual_(false),
      direct_show_(NULL) {
  // TODO(mcasas): Check that CoInitializeEx() has been called with the
  // appropriate Apartment model, i.e., Single Threaded.
}

VideoCaptureDeviceDirectShowAV::~VideoCaptureDeviceDirectShowAV() {
  DCHECK(thread_checker_.CalledOnValidThread());
}

bool VideoCaptureDeviceDirectShowAV::Init() {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (direct_show_ == NULL) {
    direct_show_ = DirectShowDeviceFactory::GetInstance()->GetController(device_descriptor_.device_id);
  }

  return true;
}

void VideoCaptureDeviceDirectShowAV::AllocateAndStart(
    const VideoCaptureParams& params,
    std::unique_ptr<VideoCaptureDevice::Client> client) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (state_ != kIdle)
    return;

  client_ = std::move(client);

#if 0
  // Get the camera capability that best match the requested format.
  const CapabilityWin found_capability =
      GetBestMatchedCapability(params.requested_format, capabilities_);

  // Reduce the frame rate if the requested frame rate is lower
  // than the capability.
  const float frame_rate =
      std::min(params.requested_format.frame_rate,
               found_capability.supported_format.frame_rate);

  ScopedComPtr<IAMStreamConfig> stream_config;
  HRESULT hr = output_capture_pin_.CopyTo(stream_config.GetAddressOf());
  if (FAILED(hr)) {
    SetErrorState(FROM_HERE, "Can't get the Capture format settings", hr);
    return;
  }

  int count = 0, size = 0;
  hr = stream_config->GetNumberOfCapabilities(&count, &size);
  if (FAILED(hr)) {
    SetErrorState(FROM_HERE, "Failed to GetNumberOfCapabilities", hr);
    return;
  }

  std::unique_ptr<BYTE[]> caps(new BYTE[size]);
  ScopedMediaType media_type;

  // Get the windows capability from the capture device.
  // GetStreamCaps can return S_FALSE which we consider an error. Therefore the
  // FAILED macro can't be used.
  hr = stream_config->GetStreamCaps(found_capability.stream_index,
                                    media_type.Receive(), caps.get());
  if (hr != S_OK) {
    SetErrorState(FROM_HERE, "Failed to get capture device capabilities", hr);
    return;
  }
  if (media_type->formattype == FORMAT_VideoInfo) {
    VIDEOINFOHEADER* h =
        reinterpret_cast<VIDEOINFOHEADER*>(media_type->pbFormat);
    if (frame_rate > 0)
      h->AvgTimePerFrame = kSecondsToReferenceTime / frame_rate;
  }
  // Set the sink filter to request this format.
  sink_filter_->SetRequestedMediaFormat(
      found_capability.supported_format.pixel_format, frame_rate,
      found_capability.info_header);
  // Order the capture device to use this format.
  hr = stream_config->SetFormat(media_type.get());
  if (FAILED(hr)) {
    SetErrorState(FROM_HERE, "Failed to set capture device output format", hr);
    return;
  }
  capture_format_ = found_capability.supported_format;

  SetAntiFlickerInCaptureFilter(params);

  if (media_type->subtype == kMediaSubTypeHDYC) {
    // HDYC pixel format, used by the DeckLink capture card, needs an AVI
    // decompressor filter after source, let |graph_builder_| add it.
    hr = graph_builder_->Connect(output_capture_pin_.Get(),
                                 input_sink_pin_.Get());
  } else {
    hr = graph_builder_->ConnectDirect(output_capture_pin_.Get(),
                                       input_sink_pin_.Get(), NULL);
  }

  if (FAILED(hr)) {
    SetErrorState(FROM_HERE, "Failed to connect the Capture graph.", hr);
    return;
  }

  hr = media_control_->Pause();
  if (FAILED(hr)) {
    SetErrorState(FROM_HERE, "Failed to pause the Capture device", hr);
    return;
  }

  // Start capturing.
  hr = media_control_->Run();
  if (FAILED(hr)) {
    SetErrorState(FROM_HERE, "Failed to start the Capture device.", hr);
    return;
  }
#endif

  direct_show_->SetRequestedVideoFormat(params.requested_format);
  direct_show_->RegisterObserver(this);

  client_->OnStarted();
  state_ = kCapturing;
}

void VideoCaptureDeviceDirectShowAV::StopAndDeAllocate() {
  DCHECK(thread_checker_.CalledOnValidThread());
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


// Set the power line frequency removal in |capture_filter_| if available.
void VideoCaptureDeviceDirectShowAV::SetAntiFlickerInCaptureFilter(
    const VideoCaptureParams& params) {
  const PowerLineFrequency power_line_frequency = GetPowerLineFrequency(params);
  if (power_line_frequency != media::PowerLineFrequency::FREQUENCY_50HZ &&
      power_line_frequency != media::PowerLineFrequency::FREQUENCY_60HZ) {
    return;
  }
  ScopedComPtr<IKsPropertySet> ks_propset;
  DWORD type_support = 0;
  HRESULT hr;
  if (SUCCEEDED(hr = capture_filter_.CopyTo(ks_propset.GetAddressOf())) &&
      SUCCEEDED(hr = ks_propset->QuerySupported(
                    PROPSETID_VIDCAP_VIDEOPROCAMP,
                    KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY,
                    &type_support)) &&
      (type_support & KSPROPERTY_SUPPORT_SET)) {
    KSPROPERTY_VIDEOPROCAMP_S data = {};
    data.Property.Set = PROPSETID_VIDCAP_VIDEOPROCAMP;
    data.Property.Id = KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY;
    data.Property.Flags = KSPROPERTY_TYPE_SET;
    data.Value =
        (power_line_frequency == media::PowerLineFrequency::FREQUENCY_50HZ) ? 1
                                                                            : 2;
    data.Flags = KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
    hr = ks_propset->Set(PROPSETID_VIDCAP_VIDEOPROCAMP,
                         KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY, &data,
                         sizeof(data), &data, sizeof(data));
    DLOG_IF_FAILED_WITH_HRESULT("Anti-flicker setting failed", hr);
  }
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
