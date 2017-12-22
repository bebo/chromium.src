
#include "media/direct_show/direct_show_device_factory.h"

#include <mfapi.h>
#include <mferror.h>

#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <objbase.h>
#include <dshow.h>

//#include <stddef.h>

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

DirectShowDeviceFactory::DirectShowDeviceFactory():
  device_descriptors_(),
  fake_list_(nullptr)
{
  LOG(INFO) << __func__ ;
}

DirectShowDeviceFactory::~DirectShowDeviceFactory() {
  LOG(INFO) << __func__ ;
}

DirectShowDeviceFactory * DirectShowDeviceFactory::GetInstance() {
  return base::Singleton<DirectShowDeviceFactory, base::StaticMemorySingletonTraits<DirectShowDeviceFactory>>::get();
}

bool DirectShowDeviceFactory::IsDirectShowDevice(std::string device_id) {
  // FIXME not to fake this
  if (device_id.compare("loopback") == 0) {
    return true;
  }

  for (DirectShowDeviceDescriptor& ds : device_descriptors_) {
    if (ds.device_id == device_id) {
      return true;
    }
  }
  return false;
}

DirectShow* DirectShowDeviceFactory::GetController(std::string device_id) {
  /* FIXME:
   * * get correct enumeration
   * * get from list and if not return;
   */
  if (fake_list_ == NULL) {
     fake_list_ = new DirectShow(device_id);
  }
  return fake_list_;
}

void DirectShowDeviceFactory::GetDeviceCapabilityList(DirectShowType type,
            std::string device_id,
            DirectShowDeviceCapabilityList* device_capablity_list) {
  // FIXME
}

/*
 * [ ] should this run on it's own thread?
 */
void DirectShowDeviceFactory::GetDeviceDescriptors(DirectShowType type, DirectShowDeviceDescriptors* device_descriptors) {

  DCHECK(device_descriptors);
  DVLOG(1) << __func__;

  DirectShowDeviceDescriptors update;

  ScopedComPtr<ICreateDevEnum> dev_enum;
  HRESULT hr = ::CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC,
                                  IID_PPV_ARGS(&dev_enum));
  if (FAILED(hr))
    return;

  ScopedComPtr<IEnumMoniker> enum_moniker;
  hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
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
      // FIXME
      /* if (! IsWhiteListed(device_name)) */
      /*   continue; */

      name.Reset();
      hr = prop_bag->Read(L"DevicePath", name.Receive(), 0);
      std::string id;
      if (FAILED(hr) || name.type() != VT_BSTR) {
        id = device_name;
      } else {
        DCHECK_EQ(name.type(), VT_BSTR);
        id = base::SysWideToUTF8(V_BSTR(name.ptr()));
      }
      // FIXME
      //const std::string model_id = GetDeviceModelId(id);
      const std::string model_id = "TEST";

      device_descriptors->emplace_back(device_name, id, model_id);
      update.emplace_back(device_name, id, model_id);
    }
  }
  device_descriptors_ = update;
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
