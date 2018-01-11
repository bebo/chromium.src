// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/bebo_device/bebo_device_api.h"

#include <stddef.h>
#include <stdio.h>
#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/macros.h"
#include "base/task_runner_util.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/thread.h"
#include "content/browser/browser_main_loop.h"
#include "content/browser/renderer_host/media/media_stream_manager.h"
#include "content/browser/renderer_host/media/media_stream_ui_proxy.h"
#include "content/browser/renderer_host/media/video_capture_manager.h"
#include "content/common/media/media_stream_controls.h"
#include "content/public/browser/browser_thread.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_window.h"
#include "content/public/browser/media_capture_devices.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "chrome/browser/profiles/profile.h"
#include "extensions/common/extension.h"
#include "ui/views/win/hwnd_util.h"

namespace {
}  // namespace

namespace extensions {

bool BeboDeviceApiFunction::RunAsync() {
  content::BrowserThread::PostTask(content::BrowserThread::IO, FROM_HERE,
                          base::BindOnce(&BeboDeviceApiFunction::DoWork,
                                         base::Unretained(this)));
  return true;
}

BeboDeviceOpenPropertyPageFunction::BeboDeviceOpenPropertyPageFunction() {}

BeboDeviceOpenPropertyPageFunction::~BeboDeviceOpenPropertyPageFunction() {}

void BeboDeviceOpenPropertyPageFunction::MediaRequestPermissionCallback(
    std::string source_id,
    std::string type,
    const content::MediaStreamDevices& devices,
    std::unique_ptr<content::MediaStreamUIProxy> stream_ui) {
  LOG(INFO) << __func__ << " source id: " << source_id << ", type: " << type;

  content::MediaStreamManager* media_stream_manager =
    content::BrowserMainLoop::GetInstance()->media_stream_manager();

  std::string device_id;
  media_stream_manager->TranslateSourceIdToDeviceId(
      label_, source_id, &device_id);

  if (device_id.empty()) {
    SetError("Failed to find the device id");
    return;
  }

  LOG(INFO)
    << "source_id: " << source_id <<
    ", device_id: " << device_id;
  content::VideoCaptureManager* video_stream_manager =
    media_stream_manager->video_capture_manager();
  video_stream_manager->OpenPropertyPage(device_id, type);

  const char* error = "";
  SetError(error);

  bool success = true;
  SendResponse(success);
}

void BeboDeviceOpenPropertyPageFunction::DoWork() {
  LOG(INFO) << __func__;

  std::unique_ptr<api::bebo_device::OpenPropertyPage::Params> params(
      api::bebo_device::OpenPropertyPage::Params::Create(*args_));
  // EXTENSION_FUNCTION_VALIDATE(params.get());

  if (!content::BrowserMainLoop::GetInstance()) {
    SetError("Failed to get browser main loop");
    return;
  }

  content::WebContents* web_contents = GetSenderWebContents();
  GURL origin = extension()->is_nwjs_app() ? web_contents->GetURL().GetOrigin() : extension()->url();
  content::MediaStreamManager* media_stream_manager =
    content::BrowserMainLoop::GetInstance()->media_stream_manager();

  label_ = media_stream_manager->MakeMediaAccessRequest(
      render_frame_host()->GetProcess()->GetID(),
      render_frame_host()->GetRoutingID(),
      123,
      content::StreamControls(true, true),
      url::Origin(GURL(origin)),
      base::BindOnce(
        &BeboDeviceOpenPropertyPageFunction::MediaRequestPermissionCallback,
        this,
        params->device_id,
        params->type));

  LOG(INFO) << "label_: " << label_;
  return;
}

void BeboDeviceOpenPropertyPageFunction::CompleteFunctionWithResult(
    ) {

}

BeboDeviceGetResolutionFunction::BeboDeviceGetResolutionFunction() {}

BeboDeviceGetResolutionFunction::~BeboDeviceGetResolutionFunction() {}

void BeboDeviceGetResolutionFunction::DoWork() {
}

void BeboDeviceGetResolutionFunction::CompleteFunctionWithResult(
    ) {

}

BeboDeviceGetCapabilitiesFunction::BeboDeviceGetCapabilitiesFunction() {}

BeboDeviceGetCapabilitiesFunction::~BeboDeviceGetCapabilitiesFunction() {}

void BeboDeviceGetCapabilitiesFunction::DoWork() {
}

void BeboDeviceGetCapabilitiesFunction::CompleteFunctionWithResult(
    ) {
}


}  // namespace extensions
