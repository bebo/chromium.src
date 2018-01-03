
#include "media/direct_show/direct_show_device_factory.h"

#include <mfapi.h>
#include <mferror.h>

#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <objbase.h>
#include <dshow.h>

#include <algorithm>
#include <list>
#include <utility>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/singleton.h"
#include "base/metrics/histogram_macros.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_comptr.h"
#include "base/win/scoped_variant.h"
#include "media/base/media_switches.h"

using base::win::ScopedCoMem;
using base::win::ScopedComPtr;
using base::win::ScopedVariant;

namespace media {
namespace {
static std::string GetDeviceInstancePath(const std::string& device_id) {
  const size_t end_token = device_id.find_last_of('#');
  if (end_token == std::string::npos) {
    return std::string();
  }
  const std::string path = device_id.substr(0, end_token);
  return path;
}
} // namespace

DirectShowDeviceFactory::DirectShowDeviceFactory() {
  LOG(INFO) << __func__ ;
}

DirectShowDeviceFactory::~DirectShowDeviceFactory() {
  LOG(INFO) << __func__ ;
}

DirectShowDeviceFactory * DirectShowDeviceFactory::GetInstance() {
  return base::Singleton<DirectShowDeviceFactory, 
         base::StaticMemorySingletonTraits<DirectShowDeviceFactory>>::get();
}

bool DirectShowDeviceFactory::IsDirectShowDevice(std::string device_id) {
  if (device_descriptors_.empty()) {
    DirectShowDeviceDescriptors device_descriptors;
    GetDeviceDescriptors(DirectShowType::Audio, CLSID_AudioInputDeviceCategory, &device_descriptors);
    GetDeviceDescriptors(DirectShowType::Video, CLSID_VideoInputDeviceCategory, &device_descriptors);
    device_descriptors_ = device_descriptors;
  }

  for (DirectShowDeviceDescriptor& ds : device_descriptors_) {
    if (ds.device_id == device_id) {
      return true;
    }
  }

  return false;
}

DirectShow* DirectShowDeviceFactory::GetController(std::string device_id) {
  auto search = devices_.find(device_id);
  DirectShow* device = NULL;
  if (search != devices_.end()) {
    LOG(INFO) << "Controller found: " << device_id;
    device = search->second;
  } else {
    LOG(INFO) << "Controller NOT FOUND, creating: " << device_id;
    device  = new DirectShow(device_id);
    devices_.emplace(std::make_pair(device_id, device));
  }
  return device;
}

void DirectShowDeviceFactory::GetDeviceDescriptors(DirectShowType type,
    DirectShowDeviceDescriptors* device_descriptors) {
  // TODO: need to be smarter on this one
  // currently we show both (Video) and (Audio)
  // AVerMedia for example is a capture device that has both audio and video
  // Elgato has two separate category of filter for both audio and video.
  // I think we can check the device path and filter it off. 
  // device path's last section is the device instance id. (Elgato's both video and audio has the same instance id)
  // One option is: we can check if there are more than two instance id then 
  // we can do matching on the category of the device path.

  if (type == DirectShowType::Audio) {
    LOG(INFO) << "bebo GetDeviceDescriptors (Audio)";
    GetDeviceDescriptors(type, CLSID_AudioInputDeviceCategory, device_descriptors);
  } else {
    LOG(INFO) << "bebo GetDeviceDescriptors (Video)";
  }
  GetDeviceDescriptors(type, CLSID_VideoInputDeviceCategory, device_descriptors);
}

void DirectShowDeviceFactory::GetDeviceDescriptors(DirectShowType type, GUID category, 
    DirectShowDeviceDescriptors* device_descriptors) {
  DCHECK(device_descriptors);

  ScopedComPtr<ICreateDevEnum> dev_enum;
  HRESULT hr = ::CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC,
                                  IID_PPV_ARGS(&dev_enum));
  if (FAILED(hr))
    return;

  ScopedComPtr<IEnumMoniker> enum_moniker;
  hr = dev_enum->CreateClassEnumerator(category,
                                       enum_moniker.GetAddressOf(), 0);
  // CreateClassEnumerator returns S_FALSE on some Windows OS
  // when no camera exist. Therefore the FAILED macro can't be used.

  if (hr == S_OK) {
    // Enumerate all video capture devices.
    for (ScopedComPtr<IMoniker> moniker;
        enum_moniker->Next(1, moniker.GetAddressOf(), NULL) == S_OK;
        moniker.Reset()) {
      ScopedComPtr<IPropertyBag> prop_bag;
      hr = moniker->BindToStorage(0, 0, IID_PPV_ARGS(&prop_bag));
      if (FAILED(hr))
        continue;

      // Find the description or friendly name.
      ScopedVariant name;
      hr = prop_bag->Read(L"Description", name.Receive(), 0);
      if (FAILED(hr))
        hr = prop_bag->Read(L"FriendlyName", name.Receive(), 0);

      if (FAILED(hr) || name.type() != VT_BSTR)
        continue;

      const std::string device_name(base::SysWideToUTF8(V_BSTR(name.ptr())));
      LOG(INFO) << "Device name: " << device_name;
      if (!DirectShow::IsDeviceWhiteListed(device_name))
        continue;

      name.Reset();
      hr = prop_bag->Read(L"DevicePath", name.Receive(), 0);
      std::string id;
      if (FAILED(hr) || name.type() != VT_BSTR) {
        id = device_name;
      } else {
        DCHECK_EQ(name.type(), VT_BSTR);
        id = base::SysWideToUTF8(V_BSTR(name.ptr()));
      }

      bool skip_device = false;
      for (DirectShowDeviceDescriptor& ds : *device_descriptors) {
        const std::string ds_model_id = GetDeviceInstancePath(ds.device_id);
        const std::string model_id = GetDeviceInstancePath(id);
        LOG(INFO) << "ds.device_id: " << ds.device_id << ", device_id: " << id;
        LOG(INFO) << "ds_model_id: " << ds_model_id << ", model_id: " << model_id;
        if (!ds_model_id.empty() && 
            !model_id.empty() && 
            ds_model_id.compare(model_id) == 0) {
          skip_device = true;
          break;
        }
      }

      if (skip_device) {
        continue;
      }

      const std::string model_id = "CaptureCard - 0000:0000";

      device_descriptors->emplace_back(device_name, id, model_id);
    }
  }

}

DirectShowDeviceDescriptor::DirectShowDeviceDescriptor(
      const std::string& display_name,
      const std::string& device_id,
      const std::string& model_id)
  : display_name(display_name),
    device_id(device_id),
    model_id(model_id) {}

DirectShowDeviceDescriptor::~DirectShowDeviceDescriptor() {};

} // namespace media
