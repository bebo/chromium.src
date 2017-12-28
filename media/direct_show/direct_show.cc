#include "media/direct_show/direct_show.h"

#include <iostream>
#include <iomanip>

#include <ks.h>
#include <ksmedia.h>
#define __STREAMS__ // workaround for ikspin typedef
#include <ksproxy.h>
#undef __STREAMS__
#include <objbase.h>
#include <wmsdkidl.h>

#include "media/direct_show/video_sink_filter.h"

#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/singleton.h"
#include "base/strings/utf_string_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "media/base/timestamp_constants.h"

using base::win::ScopedCoMem;
using base::win::ScopedComPtr;
using base::win::ScopedCOMInitializer;
using base::win::ScopedVariant;
using media::directshow::DirectShowVideoCaptureFormat;
using media::directshow::kMediaSubTypeI420;
using media::directshow::kMediaSubTypeHDYC;
using media::directshow::kMediaSubTypeZ16;
using media::directshow::kMediaSubTypeINVZ;
using media::directshow::kMediaSubTypeY16;

static const REFERENCE_TIME kSecondsToReferenceTime = 10000000;

static const enum WhitelistedFilterNames {
  GAME_CAPTURE_HD_60_PRO = 0,
                         GAME_CAPTURE_HD_60_S = 1,
                         GAME_CAPTURE_HD_60 = 2,
                         AVERMEDIA_GC550_VIDEO_CAPTURE = 3,
                         WHITELISTED_FILTER_MAX = AVERMEDIA_GC550_VIDEO_CAPTURE
};

static const char* const kWhitelistedFilterNames[] = {
  "Game Capture HD60 Pro",
  "Game Capture HD60S",
  "Game Capture HD60",
  "AVerMedia GC550 Video Capture",
};

static_assert(arraysize(kWhitelistedFilterNames) == WHITELISTED_FILTER_MAX + 1,
    "kWhitelistedFilterNames should be same size as "
    "WhitelistedFilterNames enum");

#define AVERMEDIA

#if DCHECK_IS_ON()
#define DLOG_IF_FAILED_WITH_HRESULT(message, hr)                      \
{                                                                   \
  DLOG_IF(ERROR, FAILED(hr))                                        \
  << (message) << ": " << logging::SystemErrorCodeToString(hr); \
}
#else
#define DLOG_IF_FAILED_WITH_HRESULT(message, hr) \
{                                                                   \
  LOG_IF(ERROR, FAILED(hr))                                        \
  << (message) << ": " << logging::SystemErrorCodeToString(hr); \
}
#endif

namespace {
// Check if a Pin matches a category.
bool PinMatchesCategory(IPin* pin, REFGUID category) {
  DCHECK(pin);
  bool found = false;
  ScopedComPtr<IKsPropertySet> ks_property;
  HRESULT hr = pin->QueryInterface(IID_PPV_ARGS(&ks_property));
  if (SUCCEEDED(hr)) {
    GUID pin_category;
    DWORD return_value;
    hr = ks_property->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, NULL, 0,
        &pin_category, sizeof(pin_category), &return_value);

    if (SUCCEEDED(hr) && (return_value == sizeof(pin_category))) {
      found = (pin_category == category);
    }
  }
  return found;
}

// Check if a Pin's MediaType matches a given |major_type|.
bool PinMatchesMajorType(IPin* pin, REFGUID major_type) {
  DCHECK(pin);
  media::DirectShow::ScopedMediaType media_type;
  ScopedComPtr<IEnumMediaTypes> media_type_enum;

  HRESULT hr = pin->EnumMediaTypes(&media_type_enum);
  if (media_type_enum.Get() == NULL) {
    return false;
  }

  hr = media_type_enum->Reset();  // set to first mediatype
  while ((hr = media_type_enum->Next(1, media_type.Receive(), NULL)) == S_OK) {
    if (media_type->majortype == major_type) {
      return true;
    }
    media_type.Free();
  }

  return false;
}

bool GetPinMedium(IPin* pin, REGPINMEDIUM& medium) {
  ScopedComPtr<IKsPin> iks_pin;
  HRESULT hr = pin->QueryInterface(IID_IKsPin, &iks_pin);
  if (FAILED(hr)) {
    return false;
  }

  ScopedCoMem<KSMULTIPLE_ITEM> items;
  hr = iks_pin->KsQueryMediums(&items);
  if (FAILED(hr)) { // pins do not support medium
    return false;
  }

  ULONG item_count = items->Count;
  if (!item_count) {
    return false;
  }

  REGPINMEDIUM* pin_medium = (REGPINMEDIUM*)(items + 1);
  for (ULONG i = 0; i < item_count; i++, pin_medium++) {
    // Do not connect a pin if the medium has a CLSID of GUID_NULL or
    // KSMEDIUMSETID_Standard. These are default values indicating 
    // that the pin does not support mediums.
    if (pin_medium->clsMedium != GUID_NULL &&
        pin_medium->clsMedium != KSMEDIUMSETID_Standard) {
      medium = *pin_medium;
      return true;
    }
  }
  return false;
}

bool FilterMatchesMedium(IBaseFilter* filter, REGPINMEDIUM medium) {
  ScopedComPtr<IPin> pin;
  ScopedComPtr<IEnumPins> pin_enum;
  HRESULT hr = filter->EnumPins(pin_enum.GetAddressOf());
  if (pin_enum.Get() == NULL) {
    return false;
  }

  hr = pin_enum->Reset();  // set to first pin
  while ((hr = pin_enum->Next(1, pin.GetAddressOf(), NULL)) == S_OK) {
    REGPINMEDIUM cur_pin_medium;
    if (GetPinMedium(pin.Get(), cur_pin_medium)) {
      if (cur_pin_medium.clsMedium == medium.clsMedium) {
        return true;
      }
    }
    pin.Reset();
  }

  pin.Reset();
  return false;
}

void PrintPinInfo(IPin* pin) {
  HRESULT hr;

  if (!pin) {
    LOG(INFO) << "pin == null";
    return;
  }

  AM_MEDIA_TYPE media_type = {0};
  hr = pin->ConnectionMediaType(&media_type);
  if (SUCCEEDED(hr)) {
    WCHAR major_type[128] = {0};
    StringFromGUID2(media_type.majortype, major_type, arraysize(major_type));

    WCHAR sub_type[128] = {0};
    StringFromGUID2(media_type.subtype, sub_type, arraysize(sub_type));

    WCHAR format_type[128] = {0};
    StringFromGUID2(media_type.formattype, format_type, arraysize(format_type));

    LOG(INFO) << "media_type, majortype: " << major_type
      << ", subtype: " << sub_type
      << ", formattype: " << format_type;
  } else {
    LOG(INFO) << "Failed to get connection media type";
  }


  WCHAR *pin_id = new WCHAR[128];
  hr = pin->QueryId(&pin_id);
  if (SUCCEEDED(hr)) {
    LOG(INFO) << "pin id: " << pin_id;
  } else {
    LOG(INFO) << "Failed to QueryId";
  }
  delete[] pin_id;

  PIN_INFO pin_info = {0};
  hr = pin->QueryPinInfo(&pin_info);
  if (SUCCEEDED(hr)) {
    LOG(INFO) << "pFilter: " << pin_info.pFilter
      << ", dir: " << pin_info.dir
      << ", achName: " << pin_info.achName;
  } else {
    LOG(INFO) << "Failed to query pin info";
  }
}
}  // namespace

namespace media {

DirectShow::DirectShow(std::string device_id): 
  device_id_(device_id),
  has_audio_(true),
  has_video_(true),
  audio_observer_(NULL),
  video_observer_(NULL) {
    LOG(INFO) << __func__;
    friendly_name_ = "DirectShow";  // FIXME device name??
}

DirectShow::~DirectShow() {
  LOG(INFO) << __func__;
  StopThread();
}

// static
void DirectShow::PrintPinCapabilities(const std::string& name,
    IAMStreamConfig* stream_config) {
  int count = 0, size = 0;
  HRESULT hr = stream_config->GetNumberOfCapabilities(&count, &size);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to GetNumberOfCapabilities", hr);
  if (FAILED(hr)) {
    return;
  }

  LOG(INFO) << name << " number of capabilities, count: " << count << ", size: " << size;

  for (int i = 0; i < count; i++) {
    std::unique_ptr<BYTE[]> caps(new BYTE[size]);
    ScopedMediaType media_type;
    hr = stream_config->GetStreamCaps(i,
        media_type.Receive(), caps.get());

    WCHAR major_uuid[64];
    WCHAR sub_uuid[64];
    StringFromGUID2(media_type->majortype, major_uuid, 64);
    StringFromGUID2(media_type->subtype, sub_uuid, 64);

    if (media_type->majortype == WMMEDIATYPE_Audio ||
        media_type->majortype == MEDIATYPE_Audio) {
      WAVEFORMATEX* const wfex =
        reinterpret_cast<WAVEFORMATEX*>(media_type->pbFormat);

      LOG(INFO)
        << name << " "
        << "major_uuid: " << major_uuid 
        << ", sub_uuid: " << sub_uuid 
        << ", channels: " << wfex->nChannels
        << ", sample_rate: " << wfex->nSamplesPerSec
        << ", bit_depth: " << wfex->wBitsPerSample;
    } else if (media_type->majortype == WMMEDIATYPE_Video ||
               media_type->majortype == MEDIATYPE_Video) {
      VIDEOINFOHEADER* const pvi =
        reinterpret_cast<VIDEOINFOHEADER*>(media_type->pbFormat);
      LOG(INFO) 
        << name << " "
        << "major_uuid: " << major_uuid 
        << ", sub_uuid: " << sub_uuid 
        << ", width: " << pvi->bmiHeader.biWidth 
        << ", height: " << pvi->bmiHeader.biHeight
        << std::fixed << std::setprecision(2)
        << ", fps: " << ((double) kSecondsToReferenceTime / pvi->AvgTimePerFrame);
    }
  }
}

// static
bool DirectShow::IsDeviceWhiteListed(const std::string& name) {
  DCHECK_EQ(WHITELISTED_FILTER_MAX + 1,
      static_cast<int>(arraysize(kWhitelistedFilterNames)));
  for (size_t i = 0; i < arraysize(kWhitelistedFilterNames); ++i) {
    if (base::StartsWith(name, kWhitelistedFilterNames[i],
          base::CompareCase::INSENSITIVE_ASCII) != 0) {
      LOG(INFO) << "Enumerated whitelist device: " << name;
      return true;
    }
    LOG(INFO) << "Enumerated whitelist failed: device: " << name;
  }
  return false;
}

// static
void DirectShow::GetDeviceVideoCapabilityList(
    const std::string& device_id,
    bool query_detailed_frame_rates,
    DirectShowVideoCapabilityList* out_capability_list) {

  base::win::ScopedComPtr<IBaseFilter> capture_filter;
  HRESULT hr = DirectShow::GetDeviceFilter(
      AM_KSCATEGORY_CAPTURE,
      device_id, capture_filter.GetAddressOf());
  if (!capture_filter.Get()) {
    LOG(ERROR) << "Failed to create capture filter: "
      << logging::SystemErrorCodeToString(hr);
    return;
  }

  base::win::ScopedComPtr<IPin> output_capture_pin(
      DirectShow::GetPin(capture_filter.Get(), PINDIR_OUTPUT,
        PIN_CATEGORY_CAPTURE, GUID_NULL));
  if (!output_capture_pin.Get()) {
    LOG(ERROR) << "Failed to get capture output pin";
    return;
  }

  GetVideoPinCapabilityList(capture_filter, output_capture_pin,
      query_detailed_frame_rates, out_capability_list);
}

// static
void DirectShow::GetDeviceAudioCapabilityList(
    const std::string& device_id,
    DirectShowAudioCapabilityList* out_capability_list) {

  base::win::ScopedComPtr<IBaseFilter> capture_filter;
  HRESULT hr = DirectShow::GetDeviceFilter(
      AM_KSCATEGORY_CAPTURE,
      device_id, capture_filter.GetAddressOf());
  if (!capture_filter.Get()) {
    hr = DirectShow::GetDeviceFilter(
        CLSID_AudioInputDeviceCategory,
        device_id, capture_filter.GetAddressOf());

    if (!capture_filter.Get()) {
      LOG(ERROR) << "Failed to create capture filter: "
        << logging::SystemErrorCodeToString(hr);
      return;
    }
  }

  base::win::ScopedComPtr<IPin> output_capture_pin(
      DirectShow::GetPin(capture_filter.Get(), PINDIR_OUTPUT,
        PIN_CATEGORY_CAPTURE, MEDIATYPE_Audio));
  if (!output_capture_pin.Get()) {
    LOG(ERROR) << "Failed to get capture output pin";
    return;
  }

  GetAudioPinCapabilityList(capture_filter, output_capture_pin,
      out_capability_list);
}

// static
void DirectShow::GetVideoPinCapabilityList(
    base::win::ScopedComPtr<IBaseFilter> capture_filter,
    base::win::ScopedComPtr<IPin> output_capture_pin,
    bool query_detailed_frame_rates,
    DirectShowVideoCapabilityList* out_capability_list) {
  ScopedComPtr<IAMStreamConfig> stream_config;
  HRESULT hr = output_capture_pin.CopyTo(stream_config.GetAddressOf());
  if (FAILED(hr)) {
    DLOG(ERROR) << "Failed to get IAMStreamConfig interface from "
      "capture device: "
      << logging::SystemErrorCodeToString(hr);
    return;
  }

  // Get interface used for getting the frame rate.
  ScopedComPtr<IAMVideoControl> video_control;
  hr = capture_filter.CopyTo(video_control.GetAddressOf());

  int count = 0, size = 0;
  hr = stream_config->GetNumberOfCapabilities(&count, &size);
  if (FAILED(hr)) {
    DLOG(ERROR) << "GetNumberOfCapabilities failed: "
      << logging::SystemErrorCodeToString(hr);
    return;
  }

  std::unique_ptr<BYTE[]> caps(new BYTE[size]);
  for (int i = 0; i < count; ++i) {
    DirectShow::ScopedMediaType media_type;
    hr = stream_config->GetStreamCaps(i, media_type.Receive(), caps.get());
    // GetStreamCaps() may return S_FALSE, so don't use FAILED() or SUCCEED()
    // macros here since they'll trigger incorrectly.
    if (hr != S_OK || !media_type.get()) {
      LOG(ERROR) << "GetStreamCaps failed: "
        << logging::SystemErrorCodeToString(hr);
      return;
    }

    if (media_type->majortype == MEDIATYPE_Video &&
        media_type->formattype == FORMAT_VideoInfo) {
      DirectShowVideoCaptureFormat format;
      format.pixel_format =
        DirectShow::TranslateMediaSubtypeToPixelFormat(
            media_type->subtype);
      if (format.pixel_format == PIXEL_FORMAT_UNKNOWN)
        continue;
      VIDEOINFOHEADER* h =
        reinterpret_cast<VIDEOINFOHEADER*>(media_type->pbFormat);
      format.frame_size.SetSize(h->bmiHeader.biWidth, h->bmiHeader.biHeight);

      std::vector<float> frame_rates;
      if (query_detailed_frame_rates && video_control.Get()) {
        // Try to get a better |time_per_frame| from IAMVideoControl. If not,
        // use the value from VIDEOINFOHEADER.
        ScopedCoMem<LONGLONG> time_per_frame_list;
        LONG list_size = 0;
        const SIZE size = {format.frame_size.width(),
          format.frame_size.height()};
        hr = video_control->GetFrameRateList(output_capture_pin.Get(), i, size,
            &list_size, &time_per_frame_list);
        // Sometimes |list_size| will be > 0, but time_per_frame_list will be
        // NULL. Some drivers may return an HRESULT of S_FALSE which
        // SUCCEEDED() translates into success, so explicitly check S_OK. See
        // http://crbug.com/306237.
        if (hr == S_OK && list_size > 0 && time_per_frame_list) {
          for (int k = 0; k < list_size; k++) {
            LONGLONG time_per_frame = *(time_per_frame_list.get() + k);
            if (time_per_frame <= 0)
              continue;
            frame_rates.push_back(kSecondsToReferenceTime /
                static_cast<float>(time_per_frame));
          }
        }
      }

      if (frame_rates.empty() && h->AvgTimePerFrame > 0) {
        frame_rates.push_back(kSecondsToReferenceTime /
            static_cast<float>(h->AvgTimePerFrame));
      }
      if (frame_rates.empty())
        frame_rates.push_back(0.0f);

      for (const auto& frame_rate : frame_rates) {
        format.frame_rate = frame_rate;
        out_capability_list->emplace_back(i, format, h->bmiHeader);
      }
    }
  }
}

// static
void DirectShow::GetAudioPinCapabilityList(
    base::win::ScopedComPtr<IBaseFilter> capture_filter,
    base::win::ScopedComPtr<IPin> output_capture_pin,
    DirectShowAudioCapabilityList* out_capability_list) {
  ScopedComPtr<IAMStreamConfig> stream_config;
  HRESULT hr = output_capture_pin.CopyTo(stream_config.GetAddressOf());
  if (FAILED(hr)) {
    DLOG(ERROR) << "Failed to get IAMStreamConfig interface from "
      "capture device: "
      << logging::SystemErrorCodeToString(hr);
    return;
  }

  int count = 0, size = 0;
  hr = stream_config->GetNumberOfCapabilities(&count, &size);
  if (FAILED(hr)) {
    DLOG(ERROR) << "GetNumberOfCapabilities failed: "
      << logging::SystemErrorCodeToString(hr);
    return;
  }

  std::unique_ptr<BYTE[]> caps(new BYTE[size]);
  for (int i = 0; i < count; ++i) {
    DirectShow::ScopedMediaType media_type;
    hr = stream_config->GetStreamCaps(i, media_type.Receive(), caps.get());

    // GetStreamCaps() may return S_FALSE, so don't use FAILED() or SUCCEED()
    // macros here since they'll trigger incorrectly.
    if (hr != S_OK || !media_type.get()) {
      LOG(ERROR) << "GetStreamCaps failed: "
        << logging::SystemErrorCodeToString(hr);
      return;
    }

    if (media_type->majortype == MEDIATYPE_Audio &&
        media_type->formattype == FORMAT_WaveFormatEx) {

      WAVEFORMATEX wfex =
        *reinterpret_cast<WAVEFORMATEX*>(media_type->pbFormat);
      out_capability_list->emplace_back(i, wfex);
    }
  }
}


// Finds an IPin on an IBaseFilter given the direction, Category and/or Major
// Type. If either |category| or |major_type| are GUID_NULL, they are ignored.
// static
ScopedComPtr<IPin> DirectShow::GetPin(IBaseFilter* filter,
    PIN_DIRECTION pin_dir,
    REFGUID category,
    REFGUID major_type) {
  ScopedComPtr<IPin> pin;
  ScopedComPtr<IEnumPins> pin_enum;
  HRESULT hr = filter->EnumPins(pin_enum.GetAddressOf());
  if (pin_enum.Get() == NULL)
    return pin;

  // Get first unconnected pin.
  hr = pin_enum->Reset();  // set to first pin
  while ((hr = pin_enum->Next(1, pin.GetAddressOf(), NULL)) == S_OK) {
    PIN_DIRECTION this_pin_dir = static_cast<PIN_DIRECTION>(-1);
    hr = pin->QueryDirection(&this_pin_dir);
    if (pin_dir == this_pin_dir) {
      if ((category == GUID_NULL || PinMatchesCategory(pin.Get(), category)) &&
          (major_type == GUID_NULL ||
           PinMatchesMajorType(pin.Get(), major_type))) {
        return pin;
      }
    }
    pin.Reset();
  }

  DCHECK(!pin.Get());
  return pin;
}

// static
VideoPixelFormat DirectShow::TranslateMediaSubtypeToPixelFormat(
    const GUID& sub_type) {
  static struct {
    const GUID& sub_type;
    VideoPixelFormat format;
  } const kMediaSubtypeToPixelFormatCorrespondence[] = {
    {kMediaSubTypeI420, PIXEL_FORMAT_I420},
    {MEDIASUBTYPE_IYUV, PIXEL_FORMAT_I420},
    {MEDIASUBTYPE_RGB24, PIXEL_FORMAT_RGB24},
    {MEDIASUBTYPE_YUY2, PIXEL_FORMAT_YUY2},
    {MEDIASUBTYPE_MJPG, PIXEL_FORMAT_MJPEG},
    {MEDIASUBTYPE_UYVY, PIXEL_FORMAT_UYVY},
    {MEDIASUBTYPE_ARGB32, PIXEL_FORMAT_ARGB},
    {kMediaSubTypeHDYC, PIXEL_FORMAT_UYVY},
    {kMediaSubTypeY16, PIXEL_FORMAT_Y16},
    {kMediaSubTypeZ16, PIXEL_FORMAT_Y16},
    {kMediaSubTypeINVZ, PIXEL_FORMAT_Y16},
  };
  for (const auto& pixel_format : kMediaSubtypeToPixelFormatCorrespondence) {
    if (sub_type == pixel_format.sub_type)
      return pixel_format.format;
  }
#ifndef NDEBUG
  WCHAR guid_str[128];
  StringFromGUID2(sub_type, guid_str, arraysize(guid_str));
  DVLOG(2) << "Device (also) supports an unknown media type " << guid_str;
#endif
  return PIXEL_FORMAT_UNKNOWN;
}

//static
ScopedComPtr<IPin> DirectShow::GetPinByName(IBaseFilter* filter,
    PIN_DIRECTION pin_dir,
    const std::string& pin_name) {
  ScopedComPtr<IPin> pin;
  ScopedComPtr<IEnumPins> pin_enum;
  HRESULT hr = filter->EnumPins(pin_enum.GetAddressOf());
  if (pin_enum.Get() == NULL)
    return pin;

  // Get first unconnected pin.
  hr = pin_enum->Reset();  // set to first pin
  while ((hr = pin_enum->Next(1, pin.GetAddressOf(), NULL)) == S_OK) {
    PIN_DIRECTION this_pin_dir = static_cast<PIN_DIRECTION>(-1);
    hr = pin->QueryDirection(&this_pin_dir);
    if (pin_dir == this_pin_dir) {
      PrintPinInfo(pin.Get());

      PIN_INFO pin_info = {0};
      hr = pin->QueryPinInfo(&pin_info);

      if (SUCCEEDED(hr)) {
        const std::string this_pin_name(base::SysWideToUTF8(pin_info.achName));

        if (pin_name.compare(this_pin_name) == 0) {
          return pin;
        }
      }
    }
    pin.Reset();
  }

  DCHECK(!pin.Get());
  return pin;
}

void DirectShow::StartThread() {
#if 0
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to CoInitializeEx", hr);
#endif

  GetDeviceVideoCapabilityList(device_id_, true, &video_capabilities_);
  GetDeviceAudioCapabilityList(device_id_, &audio_capabilities_);

  capture_thread_.reset(new base::DelegateSimpleThread(
        this, "capture_thread",
        base::SimpleThread::Options(base::ThreadPriority::REALTIME_AUDIO)));
  capture_thread_->Start();
}

void DirectShow::StopThread() {
#if 0
  CoUninitialize();
#endif

  if (capture_thread_) {
    capture_thread_->Join();
    capture_thread_.reset();
  }

  if (media_control_.Get()) {
    media_control_->Stop();
  }

  if (graph_builder_.Get()) {
    if (has_audio_) {
      graph_builder_->Disconnect(output_audio_capture_pin_.Get());
      graph_builder_->Disconnect(input_audio_sink_pin_.Get());
    }

    if (has_video_) {
      graph_builder_->Disconnect(output_video_capture_pin_.Get());
      graph_builder_->Disconnect(input_video_sink_pin_.Get());
    }

    if (audio_sink_filter_.get()) {
      graph_builder_->RemoveFilter(audio_sink_filter_.get());
      audio_sink_filter_ = NULL;
    }

    if (video_sink_filter_.get()) {
      graph_builder_->RemoveFilter(video_sink_filter_.get());
      video_sink_filter_ = NULL;
    }

    if (capture_filter_.Get()) {
      graph_builder_->RemoveFilter(capture_filter_.Get());
    }

    if (crossbar_filter_.Get()) {
      graph_builder_->RemoveFilter(crossbar_filter_.Get());
    }
  }

  if (capture_graph_builder_.Get()) {
    capture_graph_builder_.Reset();
  }
}

// Finds and creates a DirectShow Video Capture filter matching the |device_id|.
// static
HRESULT DirectShow::GetDeviceFilter(GUID category,
    const std::string& device_id,
    IBaseFilter** filter) {
  DCHECK(filter);

  ScopedComPtr<ICreateDevEnum> dev_enum;
  HRESULT hr = ::CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC,
      IID_PPV_ARGS(&dev_enum));
  if (FAILED(hr)) {
    return hr;
  }

  ScopedComPtr<IBaseFilter> capture_filter;
  ScopedComPtr<IEnumMoniker> enum_moniker;
  hr = dev_enum->CreateClassEnumerator(category,
      enum_moniker.GetAddressOf(), 0);
  if (hr != S_OK) {
    return hr;
  }

  LOG(INFO) << "LOOKING FOR DEVICE: " << device_id;

  for (ScopedComPtr<IMoniker> moniker;
      enum_moniker->Next(1, moniker.GetAddressOf(), NULL) == S_OK;
      moniker.Reset()) {
    ScopedComPtr<IPropertyBag> prop_bag;
    hr = moniker->BindToStorage(0, 0, IID_PPV_ARGS(&prop_bag));
    if (FAILED(hr))
      continue;

    // Find |device_id| via DevicePath, Description or FriendlyName, whichever
    // is available first and is a VT_BSTR (i.e. String) type.
    static const wchar_t* kPropertyNames[] = {
      L"DevicePath", L"Description", L"FriendlyName"};

    ScopedVariant name;
    for (const auto* property_name : kPropertyNames) {
      prop_bag->Read(property_name, name.Receive(), 0);
      if (name.type() == VT_BSTR)
        break;
    }

    if (name.type() == VT_BSTR) {
      const std::string device_path(base::SysWideToUTF8(V_BSTR(name.ptr())));
      if (device_path.compare(device_id) == 0) {
        // We have found the requested device
        // LOG(INFO) << "FOUND DEVICE: " << device_id;
        hr = moniker->BindToObject(0, 0, IID_PPV_ARGS(&capture_filter));
        LOG_IF(ERROR, FAILED(hr)) << "Failed to bind device filter: "
          << logging::SystemErrorCodeToString(hr);
        break;
      }
    }
  }

  *filter = capture_filter.Detach();
  if (!*filter && SUCCEEDED(hr))
    hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

  return hr;

}

//static
HRESULT DirectShow::GetCrossbarFilter(ICaptureGraphBuilder2* graph_builder,
    IBaseFilter* capture_filter,
    IBaseFilter** filter) {
  ScopedComPtr<IAMCrossbar> crossbar;

  HRESULT hr = graph_builder->FindInterface(&LOOK_UPSTREAM_ONLY, NULL,
      capture_filter, IID_PPV_ARGS(&crossbar));
  if (FAILED(hr)) { // no crossbar
    return hr;
  }

  ScopedComPtr<IBaseFilter> crossbar_filter;
  crossbar.CopyTo(crossbar_filter.GetAddressOf());

  ScopedComPtr<IPin> pin;
  pin = GetPin(crossbar_filter.Get(), PINDIR_OUTPUT, GUID_NULL, GUID_NULL);
  if (pin.Get() == NULL) { // no pin?
    LOG(ERROR) << "Failed to get any crossbar output pin", hr;
    return E_NOTIMPL;
  }

  // get pin medium to find kernel filter
  REGPINMEDIUM pin_medium;
  bool pin_medium_result = GetPinMedium(pin.Get(), pin_medium);
  if (!pin_medium_result) { // no pin medium
    LOG(ERROR) << "Failed to get crossbar output pin medium", hr;
    return E_NOTIMPL;
  }

  ScopedComPtr<ICreateDevEnum> dev_enum;
  hr = ::CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC,
      IID_PPV_ARGS(&dev_enum));
  if (FAILED(hr)) {
    return hr;
  }

  ScopedComPtr<IEnumMoniker> enum_moniker;
  hr = dev_enum->CreateClassEnumerator(AM_KSCATEGORY_CROSSBAR,
      enum_moniker.GetAddressOf(), 0);
  if (hr != S_OK) {
    return hr;
  }

  ScopedComPtr<IBaseFilter> cur_capture_filter;
  for (ScopedComPtr<IMoniker> moniker;
      enum_moniker->Next(1, moniker.GetAddressOf(), NULL) == S_OK;
      moniker.Reset()) {
    ScopedComPtr<IPropertyBag> prop_bag;
    hr = moniker->BindToStorage(0, 0, IID_PPV_ARGS(&prop_bag));
    if (FAILED(hr))
      continue;

    hr = moniker->BindToObject(0, 0, IID_PPV_ARGS(&cur_capture_filter));
    DLOG_IF(ERROR, FAILED(hr)) << "Failed to bind camera filter: "
      << logging::SystemErrorCodeToString(hr);

    bool match = FilterMatchesMedium(cur_capture_filter.Get(), pin_medium);
    if (match) {
      LOG(INFO) << "Found matching filter by medium";
      break;
    }
    cur_capture_filter.Reset();
  }

  *filter = cur_capture_filter.Detach();
  if (!*filter && SUCCEEDED(hr))
    hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

  return hr;
}

void DirectShow::ShouldGraphBeRunning() {
  running_count_.Increment();

  if (running_count_.IsOne()) {
    StartThread();
  }
}

void DirectShow::ShouldGraphBeStopping() {
  running_count_.Decrement();

  if (running_count_.IsZero()) {
    StopThread();
  }
}

void DirectShow::SetRequestedVideoFormat(VideoCaptureFormat params) {
  LOG(INFO) << __func__
    << " "
    << params.frame_size.width() << "x" << params.frame_size.height() 
    << " " << params.frame_rate << "fps";

  DirectShowVideoCaptureFormat format(
      params.frame_size,
      params.frame_rate, 
      params.pixel_format);
  requested_video_format_ = format;

  // If there's an observer attached most likely it's running already?
  {
    base::AutoLock al(video_lock_);
    if (video_observer_ != NULL) {
      StopThread();
      StartThread();
    }
  }
}

void DirectShow::SetRequestedAudioFormat(AudioParameters params) {
  LOG(INFO) << __func__;

  WAVEFORMATEX* format = &requested_audio_format_.Format;
  format->wFormatTag = WAVE_FORMAT_PCM;
  format->nSamplesPerSec = params.sample_rate();
  format->wBitsPerSample = params.bits_per_sample();
  format->nChannels = params.channels();
  format->nBlockAlign = (format->wBitsPerSample / 8) * format->nChannels;
  format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
  format->cbSize = 0;

  // If there's an observer attached most likely it's running already?
  {
    base::AutoLock al(audio_lock_);
    if (audio_observer_ != NULL) {
      StopThread();
      StartThread();
    }
  }
}

void DirectShow::RegisterObserver(AudioSinkFilterObserver* observer) {
  {
    base::AutoLock al(audio_lock_);
    audio_observer_ = observer;
  }
  ShouldGraphBeRunning();
  observer->FormatChanged(new WAVEFORMATEXTENSIBLE(requested_audio_format_));
}

void DirectShow::UnregisterObserver(AudioSinkFilterObserver* observer) {
  {
    base::AutoLock al(audio_lock_);
    audio_observer_ = NULL;
  }
  ShouldGraphBeStopping();
}

void DirectShow::RegisterObserver(VideoSinkFilterObserver* observer) {
  {
    base::AutoLock al(video_lock_);
    video_observer_ = observer;
  }
  ShouldGraphBeRunning();
  // observer->FormatChanged(new VideoPixelFormat(format_));
}

void DirectShow::UnregisterObserver(VideoSinkFilterObserver* observer) {
  {
    base::AutoLock al(video_lock_);
    video_observer_ = NULL;
  }
  ShouldGraphBeStopping();
}

HRESULT DirectShow::SetupGraph() {
  HRESULT hr = ::CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&graph_builder_));
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create capture filter", hr);
  if (FAILED(hr)) {
    return hr;
  }

  hr = ::CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC,
      IID_PPV_ARGS(&capture_graph_builder_));
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create the Capture Graph Builder", hr);
  if (FAILED(hr)) {
    return hr;
  }

  hr = capture_graph_builder_->SetFiltergraph(graph_builder_.Get());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to give graph to capture graph builder",
      hr);
  if (FAILED(hr)) {
    return hr;
  }

  hr = graph_builder_.CopyTo(media_control_.GetAddressOf());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create media control builder", hr);
  if (FAILED(hr)) {
    return hr;
  }

  return hr;
}

HRESULT DirectShow::SetupCrossbar() {
  HRESULT hr = GetCrossbarFilter(capture_graph_builder_.Get(),
      capture_filter_.Get(),
      crossbar_filter_.GetAddressOf());
  if (crossbar_filter_.Get() != NULL) {
    LOG(INFO) << "Device has a crossbar";
    hr = graph_builder_->AddFilter(crossbar_filter_.Get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to add the crossbar to the graph", hr);
    if (FAILED(hr)) {
      return hr;
    }
  } else {
    LOG(INFO) << "Device does not have a crossbar";
  }

  return S_OK; // maybe S_FALSE when device doesnt have a crossbar
}


HRESULT DirectShow::SetupVideoCapture() {
  HRESULT hr = GetDeviceFilter(AM_KSCATEGORY_CAPTURE, device_id_,
      capture_filter_.GetAddressOf());
  if (!capture_filter_.Get()) {
    has_video_ = false;
    return hr;
  }

  hr = graph_builder_->AddFilter(capture_filter_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to add the capture device to the graph", hr);

  hr = capture_graph_builder_->FindPin(capture_filter_.Get(), PINDIR_OUTPUT,
      &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, TRUE, 
      0, &output_video_capture_pin_);
  if (output_video_capture_pin_.Get() == NULL) {
    has_video_ = false;
  }

  // setup video sink filter if there's video pin
  if (has_video_) {
    video_sink_filter_ = new VideoSinkFilter(this);
    if (video_sink_filter_.get() == NULL) {
      LOG(ERROR) << "Failed to create video sink filter";
      return E_OUTOFMEMORY;
    }
    input_video_sink_pin_ = video_sink_filter_->GetPin(0);

    hr = graph_builder_->AddFilter(video_sink_filter_.get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to add the video sink filter to the graph", hr);
    if (FAILED(hr)) {
      return hr;
    }
  }

  return S_OK;
}

HRESULT DirectShow::SetupVideoCaptureFormat() {
  if (!has_video_) {
    return S_FALSE;
  }

  const DirectShowVideoCapability found_capability =
    GetBestMatchedCapability(requested_video_format_, video_capabilities_);

  // Reduce the frame rate if the requested frame rate is lower
  // than the capability.
  const float frame_rate =
      std::min(requested_video_format_.frame_rate,
               found_capability.supported_format.frame_rate);

  ScopedComPtr<IAMStreamConfig> stream_config;
  HRESULT hr = output_video_capture_pin_.CopyTo(stream_config.GetAddressOf());
  DLOG_IF_FAILED_WITH_HRESULT("Can't get the Capture format settings", hr);
  if (FAILED(hr)) {
    return hr;
  }

  PrintPinCapabilities("video", stream_config.Get());

  int count = 0, size = 0;
  hr = stream_config->GetNumberOfCapabilities(&count, &size);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to GetNumberOfCapabilities", hr);
  if (FAILED(hr)) {
    return hr;
  }

  std::unique_ptr<BYTE[]> caps(new BYTE[size]);
  ScopedMediaType media_type;

  LOG(INFO) << "video best capability index: " << found_capability.stream_index;
  hr = stream_config->GetStreamCaps(found_capability.stream_index,
      media_type.Receive(), caps.get());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to get capture device capabilities", hr);
  if (hr != S_OK) {
    return hr;
  }

  if (media_type->formattype == FORMAT_VideoInfo) {
    VIDEOINFOHEADER* h =
      reinterpret_cast<VIDEOINFOHEADER*>(media_type->pbFormat);
    if (frame_rate > 0)
      h->AvgTimePerFrame = kSecondsToReferenceTime / frame_rate;
  }

  video_sink_filter_->SetRequestedMediaFormat(
      found_capability.supported_format.pixel_format, frame_rate,
      found_capability.info_header);

  ScopedMediaType get_media_type;
  hr = stream_config->GetFormat(get_media_type.Receive());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to get capture device output format", hr);
  if (SUCCEEDED(hr)) {
    VIDEOINFOHEADER* h =
      reinterpret_cast<VIDEOINFOHEADER*>(get_media_type->pbFormat);
    LOG(INFO) << "GetFormat: preferred video output format, "
      << h->bmiHeader.biWidth << "x" << h->bmiHeader.biHeight;
  }

  // Order the capture device to use this format.
  hr = stream_config->SetFormat(media_type.get());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to set capture device output format", hr);
  if (FAILED(hr)) {
    return hr;
  }
  LOG(INFO) << "Successfully set capture device output format: " << std::hex << hr;

  return S_OK;
}

HRESULT DirectShow::SetupAudioCapture() {
  // if theres no capture filter yet, for when filter is not in ks capture
  HRESULT hr;
  if (!capture_filter_.Get()) {
    hr = GetDeviceFilter(CLSID_AudioInputDeviceCategory,
        device_id_, capture_filter_.GetAddressOf());

    if (!capture_filter_.Get()) {
      has_audio_ = false;
      return hr;
    }

    hr = graph_builder_->AddFilter(capture_filter_.Get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to add the capture device to the graph", hr);
  }

  hr = capture_graph_builder_->FindPin(capture_filter_.Get(), PINDIR_OUTPUT,
      &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, TRUE,
      0, &output_audio_capture_pin_);
  if (output_audio_capture_pin_.Get() == NULL) {
    has_audio_ = false;
  }

  // setup video sink filter if there's audio pin
  if (has_audio_) {
    audio_sink_filter_ = new AudioSinkFilter(this);
    if (audio_sink_filter_.get() == NULL) {
      LOG(ERROR) << "Failed to create audio sink filter";
      return E_OUTOFMEMORY;
    }
    input_audio_sink_pin_ = audio_sink_filter_->GetPin(0);

    hr = graph_builder_->AddFilter(audio_sink_filter_.get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to add the audio sink filter to the graph", hr);
    if (FAILED(hr)) {
      return hr;
    }
  }

  return S_OK;
}

HRESULT DirectShow::SetupAudioCaptureFormat() {
  if (!has_audio_) {
    return S_FALSE;
  }

  const DirectShowAudioCapability found_capability =
    GetBestMatchedCapability(requested_audio_format_.Format, audio_capabilities_);

  ScopedComPtr<IAMStreamConfig> stream_config;
  HRESULT hr = output_audio_capture_pin_.CopyTo(stream_config.GetAddressOf());
  DLOG_IF_FAILED_WITH_HRESULT("Can't get the capture format settings for audio", hr);
  if (FAILED(hr)) {
    return hr;
  }

  PrintPinCapabilities("audio", stream_config.Get());

  int count = 0, size = 0;
  hr = stream_config->GetNumberOfCapabilities(&count, &size);
  DLOG_IF_FAILED_WITH_HRESULT("Failed audio GetNumberOfCapabilities", hr);
  if (FAILED(hr)) {
    return hr;
  }

  std::unique_ptr<BYTE[]> caps(new BYTE[size]);
  ScopedMediaType media_type;

  LOG(INFO) << "audio best capability index: " << found_capability.stream_index;
  hr = stream_config->GetStreamCaps(found_capability.stream_index,
      media_type.Receive(), caps.get());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to get audio capture device capabilities", hr);
  if (hr != S_OK) {
    return hr;
  }

  audio_sink_filter_->SetRequestedMediaFormat(requested_audio_format_.Format);

  // Order the capture device to use this format.
  hr = stream_config->SetFormat(media_type.get());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to set audio capture device output format", hr);
  if (FAILED(hr)) {
    return hr;
  }

  return S_OK;
}

void DirectShow::Run() {
  LOG(INFO) << friendly_name_ << " DirectShow::Run()";

  HRESULT hr = SetupGraph();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to setup graph", hr);

  hr = SetupVideoCapture();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to setup video capture", hr);

  hr = SetupAudioCapture();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to setup audio capture", hr);

  hr = SetupCrossbar();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to setup crosbar", hr);

  hr = SetupVideoCaptureFormat();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to setup video capture format", hr);

  hr = SetupAudioCaptureFormat();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to setup audio capture format", hr);

  // connect video pins
  if (has_video_) {
    hr = graph_builder_->ConnectDirect(output_video_capture_pin_.Get(),
        input_video_sink_pin_.Get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to connect video capture pin to video sink pin", hr);
    PrintPinInfo(output_video_capture_pin_.Get());
  }

  // connect audio pins
  if (has_audio_) {
    hr = graph_builder_->ConnectDirect(output_audio_capture_pin_.Get(),
        input_audio_sink_pin_.Get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to connect audio capture pin to audio sink pin", hr);
    PrintPinInfo(output_audio_capture_pin_.Get());
  }

  hr = media_control_->Pause();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to pause the capture device", hr);
  if (FAILED(hr)) {
    return;
  }

  // Start capturing.
  hr = media_control_->Run();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to start the capture device", hr);
  if (FAILED(hr)) {
    return;
  }
}

void DirectShow::AudioFrameReceived(const uint8_t* buffer,
    int length,
    base::TimeDelta timestamp) {
  base::AutoLock al(audio_lock_);

  if (audio_observer_ != NULL) {
    audio_observer_->AudioFrameReceived(buffer, length, timestamp);
  }
}

void DirectShow::VideoFrameReceived(const uint8_t* buffer,
    int length,
    const DirectShowVideoCaptureFormat& format,
    base::TimeDelta timestamp) {
  base::AutoLock al(video_lock_);

  if (video_observer_ != NULL) {
    video_observer_->VideoFrameReceived(buffer, length, format, timestamp);
  }
}

void DirectShow::ScopedMediaType::Free() {
  if (!media_type_)
    return;

  DeleteMediaType(media_type_);
  media_type_ = NULL;
}

AM_MEDIA_TYPE** DirectShow::ScopedMediaType::Receive() {
  DCHECK(!media_type_);
  return &media_type_;
}

// Release the format block for a media type.
// http://msdn.microsoft.com/en-us/library/dd375432(VS.85).aspx
void DirectShow::ScopedMediaType::FreeMediaType(AM_MEDIA_TYPE* mt) {
  if (mt->cbFormat != 0) {
    CoTaskMemFree(mt->pbFormat);
    mt->cbFormat = 0;
    mt->pbFormat = NULL;
  }
  if (mt->pUnk != NULL) {
    NOTREACHED();
    // pUnk should not be used.
    mt->pUnk->Release();
    mt->pUnk = NULL;
  }
}

// Delete a media type structure that was allocated on the heap.
// http://msdn.microsoft.com/en-us/library/dd375432(VS.85).aspx
void DirectShow::ScopedMediaType::DeleteMediaType(
    AM_MEDIA_TYPE* mt) {
  if (mt != NULL) {
    FreeMediaType(mt);
    CoTaskMemFree(mt);
  }
}

} // namespace media
