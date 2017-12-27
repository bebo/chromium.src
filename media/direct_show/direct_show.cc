#include "media/direct_show/direct_show.h"

#include <ks.h>
#include <ksmedia.h>
#define __STREAMS__ // workaround for ikspin typedef
#include <ksproxy.h>
#undef __STREAMS__
#include <objbase.h>

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
  AVERMEDIA_GC550_VIDEO_CAPTURE = 1,
  WHITELISTED_FILTER_MAX = 1
};

static const char* const kWhitelistedFilterNames[] = {
  "Game Capture HD60 Pro",
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
  AM_MEDIA_TYPE connection_media_type;
  const HRESULT hr = pin->ConnectionMediaType(&connection_media_type);
  return SUCCEEDED(hr) && connection_media_type.majortype == major_type;
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

    LOG(INFO) << "media_type, majortype: " << major_type << ", subtype: " << sub_type << ", formattype: " << format_type;
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
    LOG(INFO) << "pFilter: " << pin_info.pFilter << ", dir: " << pin_info.dir << ", achName: " << pin_info.achName;
  } else {
    LOG(INFO) << "Failed to query pin info";
  }
}

}  // namespace

namespace media {

DirectShow::DirectShow(std::string device_id)
  : device_id_(device_id),
  has_audio_(true),
  has_video_(true),
  audio_observer_(NULL),
  video_observer_(NULL) {
  LOG(INFO) << __func__;
  friendly_name_ = "DirectShow";  // FIXME device name??

  // Set up the desired capture format specified by the client.
  WAVEFORMATEX* format = &format_.Format;
  format->wFormatTag = WAVE_FORMAT_PCM;
  format->nSamplesPerSec = 48000; // params.sample_rate();
  format->wBitsPerSample = 16; // params.bits_per_sample();
  format->nChannels = 2; // params.channels();
  format->nBlockAlign = (format->wBitsPerSample / 8) * format->nChannels;
  format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
  format->cbSize = 0;
}

DirectShow::~DirectShow() {
  LOG(INFO) << __func__ ;
  StopThread();
}

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

void DirectShow::StopThread() {
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
HRESULT DirectShow::GetDeviceFilter(const std::string& device_id,
                                    IBaseFilter** filter) {
  DCHECK(filter);

  ScopedComPtr<ICreateDevEnum> dev_enum;
  HRESULT hr = ::CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC,
                                  IID_PPV_ARGS(&dev_enum));
  if (FAILED(hr))
    return hr;

  ScopedComPtr<IEnumMoniker> enum_moniker;
  hr = dev_enum->CreateClassEnumerator(AM_KSCATEGORY_CAPTURE,
                                       enum_moniker.GetAddressOf(), 0);
  if (hr != S_OK) {
    return hr;
  }

  ScopedComPtr<IBaseFilter> capture_filter;
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
        LOG(INFO) << "FOUND DEVICE: " << device_id;
        hr = moniker->BindToObject(0, 0, IID_PPV_ARGS(&capture_filter));
        LOG_IF(ERROR, FAILED(hr)) << "Failed to bind camera filter: "
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

HRESULT DirectShow::GetCrossbarFilter(ICaptureGraphBuilder2* graph_builder,
                                      IBaseFilter* capture_filter,
                                      IBaseFilter** filter) {
  // "AVerMedia GC550 Crossbar"
  ScopedComPtr<IAMCrossbar> crossbar;

  HRESULT hr = graph_builder->FindInterface(&LOOK_UPSTREAM_ONLY, NULL,
      capture_filter, IID_PPV_ARGS(&crossbar));
  DLOG_IF_FAILED_WITH_HRESULT("Failed to find upstream crossbar", hr);
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

void DirectShow::Run() {
  LOG(INFO) << friendly_name_ << " DirectShow::Run()";

  HRESULT hr = SetCaptureDevice();
  LOG(INFO) << friendly_name_ << " DirectShow::Run() - SetCaptureDevice - " << std::hex << hr;


#ifdef AVERMEDIA
  SetEncoderSetting();
#endif

  if (has_audio_) {
    LOG(INFO) << "DirectShow::Run() about to connect direct (audio) ";
    hr = graph_builder_->ConnectDirect(output_audio_capture_pin_.Get(),
        input_audio_sink_pin_.Get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to connect the capture audio", hr);
    if (FAILED(hr)) {
      return;
    }

    LOG(INFO) << "output_audio_capture_pin_ (2)";
    PrintPinInfo(output_audio_capture_pin_.Get());
  }

  if (has_video_) {

    LOG(INFO) << "DirectShow::Run() about to connect direct (video)";

    hr = graph_builder_->ConnectDirect(output_video_capture_pin_.Get(),
        input_video_sink_pin_.Get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to connect the Capture graph", hr);
    if (FAILED(hr)) {
      return;
    }
  }

  LOG(INFO) << "output_video_capture_pin_ (2)";
  PrintPinInfo(output_video_capture_pin_.Get());

  LOG(INFO) << "DirectShow::Run() about to pause";
  hr = media_control_->Pause();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to pause the Capture device", hr);
  if (FAILED(hr)) {
    return;
  }

  LOG(INFO) << "DirectShow::Run() about to run";

  // Start capturing.
  hr = media_control_->Run();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to start the Capture device", hr);
  if (FAILED(hr)) {
    return;
  }

  LOG(INFO) << "DirectShow::Run() running";
}


HRESULT DirectShow::SetCaptureDevice() {
  LOG(INFO) << friendly_name_ << " DirectShow::SetCaptureDevice()";

////// open
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to CoInitializeEx", hr);

  hr = ::CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&graph_builder_));
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create capture filter", hr);
  if (FAILED(hr))
    return hr;

  hr = ::CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC,
                          IID_PPV_ARGS(&capture_graph_builder_));
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create the Capture Graph Builder", hr);
  if (FAILED(hr))
    return hr;

  hr = capture_graph_builder_->SetFiltergraph(graph_builder_.Get());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to give graph to capture graph builder",
                              hr);
  if (FAILED(hr))
    return hr;

  hr = graph_builder_.CopyTo(media_control_.GetAddressOf());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create media control builder", hr);
  if (FAILED(hr))
    return hr;

/////// do device stuff

  hr = GetDeviceFilter(device_id_,
                       capture_filter_.GetAddressOf());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create capture filter", hr);
  if (!capture_filter_.Get())
    return hr;

  hr = graph_builder_->AddFilter(capture_filter_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to add the capture device to the graph", hr);


/// crossbar 
  hr = GetCrossbarFilter(capture_graph_builder_.Get(),
                         capture_filter_.Get(),
                         crossbar_filter_.GetAddressOf());
  if (crossbar_filter_.Get() != NULL) {
    LOG(INFO) << "Found Crossbar";
    hr = graph_builder_->AddFilter(crossbar_filter_.Get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to add the crossbar to the graph",
        hr);
    if (FAILED(hr))
      return hr;
  } else {
    LOG(INFO) << "Crossbar not found";
  }

  // Create the sink filter used for receiving Captured frames.
  audio_sink_filter_ = new AudioSinkFilter(this);
  if (audio_sink_filter_.get() == NULL) {
    LOG(ERROR) << "Failed to create sink filter";
    return E_OUTOFMEMORY;
  }

  input_audio_sink_pin_ = audio_sink_filter_->GetPin(0);

  video_sink_filter_ = new VideoSinkFilter(this);
  if (video_sink_filter_.get() == NULL) {
    LOG(ERROR) << "Failed to create sink filter";
    return E_OUTOFMEMORY;
  }

  input_video_sink_pin_ = video_sink_filter_->GetPin(0);

  hr = capture_graph_builder_->FindPin(capture_filter_.Get(), PINDIR_OUTPUT,
      &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, TRUE, 
      0, &output_video_capture_pin_);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to find video output pin", hr);

  if (output_video_capture_pin_.Get() == NULL) {
    LOG(ERROR) << "Failed to get device video pin";
    has_video_ = false;
  } else {
    LOG(INFO) << "output_video_capture_pin_";
    PrintPinInfo(output_video_capture_pin_.Get());
  }

  hr = capture_graph_builder_->FindPin(capture_filter_.Get(), PINDIR_OUTPUT,
      &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, TRUE,
      0, &output_audio_capture_pin_);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to find audio output pin", hr);
  if (FAILED(hr)) {
    has_audio_ = false;
  }

  if (output_audio_capture_pin_.Get() == NULL) {
    LOG(ERROR) << "Failed to get device audio pin";
    has_audio_ = false;
  } else {
    LOG(INFO) << "output_audio_capture_pin_";
    PrintPinInfo(output_audio_capture_pin_.Get());
  }

  if (has_audio_) {
    hr = graph_builder_->AddFilter(audio_sink_filter_.get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to add the audio sink filter to the graph", hr);
    if (FAILED(hr))
      return hr;
  }

  if (has_video_) {
    hr = graph_builder_->AddFilter(video_sink_filter_.get(), NULL);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to add the video sink filter to the graph", hr);
    if (FAILED(hr))
      return hr;
  }

  ///////// start
  if (has_video_) {
    ScopedComPtr<IAMStreamConfig> stream_config;
    hr = output_video_capture_pin_.CopyTo(stream_config.GetAddressOf());
    DLOG_IF_FAILED_WITH_HRESULT("Can't get the Capture format settings", hr);
    if (FAILED(hr)) {
      return hr;
    }

    int count = 0, size = 0;
    hr = stream_config->GetNumberOfCapabilities(&count, &size);
    DLOG_IF_FAILED_WITH_HRESULT("Failed to GetNumberOfCapabilities", hr);
    if (FAILED(hr)) {
      return hr;
    }

    LOG(INFO) << "number of capabilities, count: " << count << ", size: " << size;

    std::unique_ptr<BYTE[]> caps(new BYTE[size]);
    ScopedMediaType media_type;

    for (int i = 0; i < count; i++) {
      std::unique_ptr<BYTE[]> pcaps(new BYTE[size]);
      ScopedMediaType pmedia_type;
      hr = stream_config->GetStreamCaps(i,
          pmedia_type.Receive(), pcaps.get());

      WCHAR major_uuid[64];
      WCHAR sub_uuid[64];
      StringFromGUID2(pmedia_type->majortype, major_uuid, 64);
      StringFromGUID2(pmedia_type->subtype, sub_uuid, 64);

      VIDEOINFOHEADER* const pvi =
        reinterpret_cast<VIDEOINFOHEADER*>(pmedia_type->pbFormat);

      LOG(INFO) << "major uuid: " << major_uuid 
        << ", sub_uuid: " << sub_uuid 
        << ", width: " << pvi->bmiHeader.biWidth 
        <<  ", height: " << pvi->bmiHeader.biHeight;
    }

    // TODO: get the cap_index based on get best capability match
    int cap_index = 4;
    hr = stream_config->GetStreamCaps(cap_index,
        media_type.Receive(), caps.get());
    DLOG_IF_FAILED_WITH_HRESULT("Failed to get capture device capabilities", hr);
    if (hr != S_OK) {
      return hr;
    }

    VIDEOINFOHEADER* const pvi =
      reinterpret_cast<VIDEOINFOHEADER*>(media_type->pbFormat);
    video_sink_filter_->SetRequestedMediaFormat(
        TranslateMediaSubtypeToPixelFormat(media_type->subtype),
        30, pvi->bmiHeader);

    // Order the capture device to use this format.
    hr = stream_config->SetFormat(media_type.get());
    DLOG_IF_FAILED_WITH_HRESULT("Failed to set capture device output format", hr);
    if (FAILED(hr)) {
      return hr;
    }
  }

  return hr;
}

// static
void DirectShow::GetVideoDeviceCapabilityList(
    const std::string& device_id,
    bool query_detailed_frame_rates,
    DirectShowDeviceCapabilityList* out_capability_list) {

  base::win::ScopedComPtr<IBaseFilter> capture_filter;
  HRESULT hr = DirectShow::GetDeviceFilter(
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

  GetPinCapabilityList(capture_filter, output_capture_pin,
                       query_detailed_frame_rates, out_capability_list);
}
// static
void DirectShow::GetPinCapabilityList(
    base::win::ScopedComPtr<IBaseFilter> capture_filter,
    base::win::ScopedComPtr<IPin> output_capture_pin,
    bool query_detailed_frame_rates,
    DirectShowDeviceCapabilityList* out_capability_list) {
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

void DirectShow::EnsureGraphIsRunning() {
  if (running_.IsZero()) {
    running_.Increment();
    capture_thread_.reset(new base::DelegateSimpleThread(
        this, "capture_thread",
        base::SimpleThread::Options(base::ThreadPriority::REALTIME_AUDIO)));
    capture_thread_->Start();
  }
}

void DirectShow::RegisterObserver(AudioSinkFilterObserver* observer) {
    audio_observer_ = observer;
    EnsureGraphIsRunning();
    observer->FormatChanged(new WAVEFORMATEXTENSIBLE(format_));
}

void DirectShow::UnregisterObserver(AudioSinkFilterObserver* observer) {
    audio_observer_ = NULL;
    //ShouldGraphBeRunning(); FIXME
}

void DirectShow::RegisterObserver(VideoSinkFilterObserver* observer) {
    video_observer_ = observer;
    EnsureGraphIsRunning();
    // observer->FormatChanged(new VideoPixelFormat(format_));
}

void DirectShow::UnregisterObserver(VideoSinkFilterObserver* observer) {
    video_observer_ = NULL;
    //ShouldGraphBeRunning(); FIXME
}


void DirectShow::SetEncoderSetting() {

#if 0
  static const GUID AVER_HW_ENCODE_PROPERTY =
  {0x1bd55918, 0xbaf5, 0x4781, {0x8d, 0x76, 0xe0, 0xa0, 0xa5, 0xe1, 0xd2, 0xb8}};

  ScopedComPtr<IKsPropertySet> property;

  capture_filter_.CopyTo(property.GetAddressOf());

  if (property.Get() == NULL) {
    LOG(ERROR) << "Failed to get IKSPropertySet for encoder settings";
    return;
  }

  AVER_PARAMETERS fps_setting = {0};
  fps_setting.index = 0;
  fps_setting.param1 = 30;
  fps_setting.param2 = 0;

  AVER_PARAMETERS bitrate_setting = {0};
  bitrate_setting.index = 1;
  bitrate_setting.param1 = 60000;
  bitrate_setting.param2 = 0;

  AVER_PARAMETERS resolution_setting = {0};
  resolution_setting.index = 2;
  resolution_setting.param1 = 1280;
  resolution_setting.param2 = 720;

  AVER_PARAMETERS encoder_setting = {0};
  encoder_setting.index = 3;
  encoder_setting.param1 = 1280;
  encoder_setting.param2 = 720;

  AVER_PARAMETERS gop_setting = {0};
  gop_setting.index = 4;
  gop_setting.param1 = 30;
  gop_setting.param2 = 0;

  HRESULT hr;
  hr = property->Set(AVER_HW_ENCODE_PROPERTY, 0, 
      &fps_setting, sizeof(fps_setting),
      &fps_setting, sizeof(fps_setting)
      );
  DLOG_IF_FAILED_WITH_HRESULT("Failed to set fps", hr);

  hr = property->Set(AVER_HW_ENCODE_PROPERTY, 0, 
      &bitrate_setting, sizeof(bitrate_setting),
      &bitrate_setting, sizeof(bitrate_setting)
      );
  DLOG_IF_FAILED_WITH_HRESULT("Failed to set bitrate", hr);

  hr = property->Set(AVER_HW_ENCODE_PROPERTY, 0, 
      &resolution_setting, sizeof(resolution_setting),
      &resolution_setting, sizeof(resolution_setting)
      );
  DLOG_IF_FAILED_WITH_HRESULT("Failed to set resolution", hr);

  hr = property->Set(AVER_HW_ENCODE_PROPERTY, 0, 
      &encoder_setting, sizeof(encoder_setting),
      &encoder_setting, sizeof(encoder_setting)
      );
  DLOG_IF_FAILED_WITH_HRESULT("Failed to set encoder resolution", hr);

  hr = property->Set(AVER_HW_ENCODE_PROPERTY, 0, 
      &gop_setting, sizeof(gop_setting),
      &gop_setting, sizeof(gop_setting)
      );
  DLOG_IF_FAILED_WITH_HRESULT("Failed to set encoder resolution", hr);
#endif
}

void DirectShow::AudioFrameReceived(const uint8_t* buffer,
                                    int length,
                                    base::TimeDelta timestamp) {
  if (audio_observer_ != NULL) {  // TODO make atomic
    audio_observer_->AudioFrameReceived(buffer, length, timestamp);
  }
}

void DirectShow::VideoFrameReceived(const uint8_t* buffer,
                                    int length,
                                    const DirectShowVideoCaptureFormat& format,
                                    base::TimeDelta timestamp) {
  if (video_observer_ != NULL) {  // TODO make atomic
    video_observer_->VideoFrameReceived(buffer, length, format, timestamp);
  }
}

} // namespace media
