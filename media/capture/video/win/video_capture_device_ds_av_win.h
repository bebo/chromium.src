// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Windows specific implementation of VideoCaptureDevice. DirectShow is used for
// capturing. DirectShow provide its own threads for capturing.

#ifndef MEDIA_CAPTURE_VIDEO_WIN_VIDEO_CAPTURE_DEVICE_DS_AV_WIN_H_
#define MEDIA_CAPTURE_VIDEO_WIN_VIDEO_CAPTURE_DEVICE_DS_AV_WIN_H_

// Avoid including strsafe.h via dshow as it will cause build warnings.
#define NO_DSHOW_STRSAFE
#include <dshow.h>
#include <stdint.h>
#include <vidcap.h>

#include <map>
#include <string>

#include "base/containers/queue.h"
#include "base/macros.h"
#include "base/threading/thread_checker.h"
#include "base/win/scoped_comptr.h"
#include "media/capture/video/video_capture_device.h"
#include "media/capture/video/win/capability_list_win.h"

#include "media/direct_show/video_sink_filter_observer.h"
#include "media/direct_show/direct_show.h"
#include "media/capture/video_capture_types.h"

using media::directshow::DirectShowVideoCaptureFormat;

namespace base {
class Location;
}  // namespace base

namespace media {

// All the methods in the class can only be run on a COM initialized thread.
class VideoCaptureDeviceDirectShowAV : public VideoCaptureDevice,
                                       public VideoSinkFilterObserver {
 public:
  static void GetDeviceCapabilityList(const std::string& device_id,
                                      bool query_detailed_frame_rates,
                                      CapabilityList* out_capability_list);

  explicit VideoCaptureDeviceDirectShowAV(
      const VideoCaptureDeviceDescriptor& device_descriptor);
  ~VideoCaptureDeviceDirectShowAV() override;

  // Opens the device driver for this device.
  bool Init();

  // VideoCaptureDevice implementation.
  void AllocateAndStart(
      const VideoCaptureParams& params,
      std::unique_ptr<VideoCaptureDevice::Client> client) override;
  void StopAndDeAllocate() override;
  void TakePhoto(TakePhotoCallback callback) override;
  void GetPhotoState(GetPhotoStateCallback callback) override;
  void SetPhotoOptions(mojom::PhotoSettingsPtr settings,
                       SetPhotoOptionsCallback callback) override;
 private:
  enum InternalState {
    kIdle,       // The device driver is opened but camera is not in use.
    kCapturing,  // Video is being captured.
    kError       // Error accessing HW functions.
                 // User needs to recover by destroying the object.
  };

  bool InitializeVideoAndCameraControls();

  // Implements SinkFilterObserver.
  void VideoFrameReceived(const uint8_t* buffer,
                     int length,
                     const DirectShowVideoCaptureFormat& format,
                     base::TimeDelta timestamp) override;
  void FormatChanged() override;

  void SetErrorState(const base::Location& from_here,
                     const std::string& reason,
                     HRESULT hr);


  DirectShow* direct_show_;
  const VideoCaptureDeviceDescriptor device_descriptor_;
  InternalState state_;
  std::unique_ptr<VideoCaptureDevice::Client> client_;

  base::TimeTicks first_ref_time_;

  base::ThreadChecker thread_checker_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(VideoCaptureDeviceDirectShowAV);
};

}  // namespace media

#endif  // MEDIA_CAPTURE_VIDEO_WIN_VIDEO_CAPTURE_DEVICE_DS_AV_WIN_H_
