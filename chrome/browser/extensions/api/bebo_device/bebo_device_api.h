// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_EXTENSIONS_API_BEBO_DEVICE_BEBO_DEVICE_API_H_
#define CHROME_BROWSER_EXTENSIONS_API_BEBO_DEVICE_BEBO_DEVICE_API_H_

#include <map>
#include <memory>

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/memory/weak_ptr.h"
#include "chrome/common/extensions/api/bebo_device.h"
#include "content/browser/renderer_host/media/media_stream_requester.h"
#include "extensions/browser/extension_function.h"

namespace content {
class MediaStreamUIProxy;
}

namespace extensions {

class BeboDeviceApiFunction : public AsyncExtensionFunction {
 public:
  BeboDeviceApiFunction() {}

 protected:
  ~BeboDeviceApiFunction() override {}

  // ExtensionFunction:
  bool RunAsync() final;

  // Actual implementation of specific functions.
  virtual void DoWork() = 0;
};

class BeboDeviceOpenPropertyPageFunction : public BeboDeviceApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("beboDevice.openPropertyPage", 
      BEBODEVICE_OPENPROPERTYPAGE);

  BeboDeviceOpenPropertyPageFunction();

 protected:
  ~BeboDeviceOpenPropertyPageFunction() override;

  // Register function implementation.
  void DoWork() final;

 private:
  void CompleteFunctionWithResult();

  void MediaRequestPermissionCallback(std::string device_id,
      std::string type,
      const content::MediaStreamDevices& devices,
      std::unique_ptr<content::MediaStreamUIProxy> stream_ui);

  std::string label_;
};

class BeboDeviceGetResolutionFunction : public BeboDeviceApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("beboDevice.getResolution", 
      BEBODEVICE_GETRESOLUTION);

  BeboDeviceGetResolutionFunction();

 protected:
  ~BeboDeviceGetResolutionFunction() override;

  // Register function implementation.
  void DoWork() final;

 private:
  void CompleteFunctionWithResult();
};

class BeboDeviceGetCapabilitiesFunction : public BeboDeviceApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("beboDevice.getCapabilities", 
      BEBODEVICE_GETCAPABILITIES);

  BeboDeviceGetCapabilitiesFunction();

 protected:
  ~BeboDeviceGetCapabilitiesFunction() override;

  // Register function implementation.
  void DoWork() final;

 private:
  void CompleteFunctionWithResult();
};


}  // namespace extensions

#endif  // CHROME_BROWSER_EXTENSIONS_API_GCM_GCM_API_H_
