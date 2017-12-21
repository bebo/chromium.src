
#include "media/direct_show/direct_show.h"

#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/singleton.h"
#include "base/strings/utf_string_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "media/audio/win/avrt_wrapper_win.h"

using base::win::ScopedCoMem;
using base::win::ScopedComPtr;
using base::win::ScopedCOMInitializer;
using base::win::ScopedVariant;


EXTERN_C const CLSID CLSID_NullRenderer;

GUID kElgatoGameCaptureHD = {0x39f50f4c,
                          0x99E1,
                          0x464A,
                          {0xb6, 0xf9, 0xd6, 0x05, 0xb4, 0xfb, 0x59, 0x18}};
GUID kAlaxAudioTestSrc = {0xbf53d5ec,
                          0xa137,
                          0x4a3e,
                          {0xb7, 0xcf, 0xb6, 0x73, 0x56, 0x8a, 0x9d, 0x8b}};
GUID kAvermediaVideoCapture = {0x8ec460c4,
                          0x0d2e,
                          0x46fd,
                          {0xad, 0x9b, 0x33, 0x1f, 0xf3, 0x3d, 0xe3, 0xb6}};

#undef ALAX_AUDIO_TEST
#define AVERMEDIA

enum FILTER {
  FILTER_ELGATO_GAME_CAPTURE_HD = 0,
  FILTER_MAX = FILTER_ELGATO_GAME_CAPTURE_HD,
};

const int kFilterSize = FILTER_MAX + 1;

#ifdef ALAX_AUDIO_TEST
const GUID kFilterArray[kFilterSize] = {kAlaxAudioTestSrc};
#else
#ifdef AVERMEDIA
const GUID kFilterArray[kFilterSize] = {kAvermediaVideoCapture};
#else
const GUID kFilterArray[kFilterSize] = {kElgatoGameCaptureHD};
#endif
#endif

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

const std::string kFilterArrayName[kFilterSize] = {"Elgato Game Capture HD"};


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

    hr = ks_property->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, NULL, 0,
                          &pin_category, sizeof(pin_category), &return_value);

    WCHAR cat[128];
    StringFromGUID2(pin_category, cat, arraysize(cat));
    LOG(INFO) << "pin matches category, return_value: " << cat;

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

DirectShow::DirectShow(std::string device_id) {
  LOG(INFO) << __func__ ;
  friendly_name_ = "DirectShow";  // FIXME device name??

  // Load the Avrt DLL if not already loaded. Required to support MMCSS.
  bool avrt_init = avrt::Initialize();
  DCHECK(avrt_init) << "Failed to load the Avrt.dll";

  // Set up the desired capture format specified by the client.
  WAVEFORMATEX* format = &format_.Format;
  format->wFormatTag = WAVE_FORMAT_PCM;
  format->nSamplesPerSec = 48000; // params.sample_rate();
  format->wBitsPerSample = 16; // params.bits_per_sample();
  format->nChannels = 2; // params.channels();
  format->nBlockAlign = (format->wBitsPerSample / 8) * format->nChannels;
  format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
  format->cbSize = 0;

  // Size in bytes of each audio frame.
  frame_size_ = format->nBlockAlign;


}

DirectShow::~DirectShow() {
  LOG(INFO) << __func__ ;
  if (media_control_.Get())
    media_control_->Stop();

  if (graph_builder_.Get()) {
    if (audio_sink_filter_.get()) {
      graph_builder_->RemoveFilter(audio_sink_filter_.get());
    }

#ifndef ALAX_AUDIO_TEST
    if (null_renderer_.Get()) {
      graph_builder_->RemoveFilter(null_renderer_.Get());
    }
#endif

    if (capture_filter_.Get()) {
      graph_builder_->RemoveFilter(capture_filter_.Get());
    }
  }

  if (capture_graph_builder_.Get())
    capture_graph_builder_.Reset();
}


void DirectShow::StopThread() {
  if (capture_thread_) {
    /* SetEvent(stop_capture_event_.Get()); FIXME */
    capture_thread_->Join();
    capture_thread_.reset();
  }

  HRESULT hr = media_control_->Stop();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to stop the capture graph", hr);
  if (FAILED(hr)) {
    return;
  }

  if (graph_builder_.Get()) {
    if (audio_sink_filter_.get()) {
      graph_builder_->RemoveFilter(audio_sink_filter_.get());
    }

#ifndef ALAX_AUDIO_TEST
    if (null_renderer_.Get()) {
      graph_builder_->RemoveFilter(null_renderer_.Get());
    }
#endif

    if (capture_filter_.Get()) {
      graph_builder_->RemoveFilter(capture_filter_.Get());
    }
  }


  graph_builder_->Disconnect(input_audio_sink_pin_.Get());

#ifndef ALAX_AUDIO_TEST
  graph_builder_->Disconnect(output_video_capture_pin_.Get());
  graph_builder_->Disconnect(null_renderer_pin_.Get());
#endif
}

// Finds and creates a DirectShow Video Capture filter matching the |device_id|.
// static
HRESULT DirectShow::GetDeviceFilter(const std::string& device_id,
                                               IBaseFilter** filter) {
  DCHECK(filter);

#ifdef AVERMEDIA
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
      //L"DevicePath", L"Description",
                                              L"FriendlyName"};

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
        hr = moniker->BindToObject(0, 0, IID_PPV_ARGS(&capture_filter));
        DLOG_IF(ERROR, FAILED(hr)) << "Failed to bind camera filter: "
                                   << logging::SystemErrorCodeToString(hr);
        break;
      }
    }
  }

  *filter = capture_filter.Detach();
  if (!*filter && SUCCEEDED(hr))
    hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

  return hr;
#else
  ScopedComPtr<IBaseFilter> capture_filter;
  for (int i = 0; i < kFilterSize; i++) {
    GUID guid = kFilterArray[i];
    std::string name = kFilterArrayName[i];

    if (name.compare(device_id) == 0) {
      HRESULT hr = ::CoCreateInstance(guid, NULL, CLSCTX_INPROC_SERVER,
          IID_PPV_ARGS(&capture_filter));

      if (SUCCEEDED(hr)) {
        *filter = capture_filter.Detach();
        return hr;
      }
    }
  }

  return E_NOTFOUND;
#endif

}

HRESULT DirectShow::GetCrossbarFilter(const std::string& device_id,
                                               IBaseFilter** filter) {
  DCHECK(filter);

  ScopedComPtr<ICreateDevEnum> dev_enum;
  HRESULT hr = ::CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC,
                                  IID_PPV_ARGS(&dev_enum));
  if (FAILED(hr))
    return hr;

  ScopedComPtr<IEnumMoniker> enum_moniker;
  hr = dev_enum->CreateClassEnumerator(AM_KSCATEGORY_CROSSBAR,
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
    //L"DevicePath", L"Description",
                                              L"FriendlyName"};

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
        hr = moniker->BindToObject(0, 0, IID_PPV_ARGS(&capture_filter));
        DLOG_IF(ERROR, FAILED(hr)) << "Failed to bind camera filter: "
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



  /* stop_capture_event_.Set(CreateEvent(NULL, FALSE, FALSE, NULL)); */
  /* DCHECK(stop_capture_event_.IsValid()); */

void DirectShow::Run() {
  LOG(INFO) << friendly_name_ << " DirectShow::Run()";

  HRESULT hr = SetCaptureDevice();
  LOG(INFO) << friendly_name_ << " DirectShow::Run() - SetCaptureDevice - " << hr;


#ifdef AVERMEDIA
  hr = graph_builder_->ConnectDirect(output_video_crossbar_pin_.Get(),
      input_video_capture_pin_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to connect the Capture graph", hr);
  if (FAILED(hr)) {
    return;
  }

  hr = graph_builder_->ConnectDirect(output_audio_crossbar_pin_.Get(),
      input_audio_capture_pin_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to connect the Capture graph", hr);
  if (FAILED(hr)) {
    return;
  }
#endif

  LOG(INFO) << "DirectShow::Run() about to connect direct (audio) ";
  hr = graph_builder_->ConnectDirect(output_audio_capture_pin_.Get(),
      input_audio_sink_pin_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to connect the Capture graph", hr);
  if (FAILED(hr)) {
    return;
  }

#ifndef ALAX_AUDIO_TEST
  LOG(INFO) << "DirectShow::Run() about to connect direct (video)";

  hr = graph_builder_->ConnectDirect(output_video_capture_pin_.Get(),
      null_renderer_pin_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to connect the Capture graph", hr);
  if (FAILED(hr)) {
    return;
  }
#endif

  LOG(INFO) << "output_audio_capture_pin_ (2)";
  PrintPinInfo(output_audio_capture_pin_.Get());

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

  /* FIXME */
  /* DWORD ret = WaitForSingleObject(stop_capture_event_.Get(), INFINITE); */
  /* LOG(INFO) << "DirectShow::Run() Ret: " << ret; */
}


HRESULT DirectShow::SetCaptureDevice() {
  LOG(INFO) << friendly_name_ << " DirectShow::SetCaptureDevice()";

////// open
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

#ifdef AVERMEDIA
  hr = GetCrossbarFilter("AVerMedia GC550 Crossbar",
                         crossbar_filter_.GetAddressOf());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to get crossbar filter", hr);
  if (!crossbar_filter_.Get())
    return hr;
#endif


#ifdef AVERMEDIA
  hr = GetDeviceFilter("AVerMedia GC550 Video Capture",
                      capture_filter_.GetAddressOf());
#else
  hr = GetDeviceFilter(device_id_,
                      capture_filter_.GetAddressOf());
#endif
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create capture filter", hr);
  if (!capture_filter_.Get())
    return hr;

  // Create the sink filter used for receiving Captured frames.
  audio_sink_filter_ = new AudioSinkFilter(this);
  if (audio_sink_filter_.get() == NULL) {
    LOG(ERROR) << "Failed to create sink filter";
    return E_OUTOFMEMORY;
  }

  input_audio_sink_pin_ = audio_sink_filter_->GetPin(0);

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

  hr = ::CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC,
                          IID_PPV_ARGS(&null_renderer_));
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create null renderer", hr);
  if (FAILED(hr))
    return hr;

 null_renderer_pin_ = GetPin(null_renderer_.Get(), PINDIR_INPUT,
     GUID_NULL, GUID_NULL);
 if (null_renderer_pin_.Get() == NULL) {
    LOG(INFO) << "Failed to find null renderer pin";
    return E_OUTOFMEMORY;
  }

  hr = capture_graph_builder_->SetFiltergraph(graph_builder_.Get());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to give graph to capture graph builder",
                              hr);
  if (FAILED(hr))
    return hr;

  hr = graph_builder_.CopyTo(media_control_.GetAddressOf());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to create media control builder", hr);
  if (FAILED(hr))
    return hr;

#ifdef ALAX_AUDIO_TEST
  output_audio_capture_pin_ = GetPin(capture_filter_.Get(), PINDIR_OUTPUT,
      GUID_NULL, GUID_NULL);
#else 

#ifdef AVERMEDIA
#if 0
  hr = capture_graph_builder_->FindPin(crossbar_filter_.Get(), PINDIR_OUTPUT,
      &GUID_NULL, &MEDIATYPE_Video, TRUE, 
      0, &output_video_crossbar_pin_);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to find crossbar video output pin", hr);
  if (FAILED(hr))
    return hr;
#endif
  output_video_crossbar_pin_ = GetPinByName(crossbar_filter_.Get(), PINDIR_OUTPUT, 
     "0: Video Decoder Out");
  if (output_video_crossbar_pin_.Get() == NULL) {
    LOG(ERROR) << "Failed to get crossbar video pin";
    return E_OUTOFMEMORY;
  }

  output_audio_crossbar_pin_ = GetPinByName(crossbar_filter_.Get(), PINDIR_OUTPUT, 
     "1: Audio Decoder Out");
  if (output_audio_crossbar_pin_.Get() == NULL) {
    LOG(ERROR) << "Failed to get crossbar audio pin";
    return E_OUTOFMEMORY;
  }

  // capture filter input pins
  input_video_capture_pin_ = GetPinByName(capture_filter_.Get(), PINDIR_INPUT, 
     /*"Analog Video In"*/ "0");
  if (input_video_capture_pin_.Get() == NULL) {
    LOG(ERROR) << "Failed to get video input pin";
    return E_OUTOFMEMORY;
  }

  input_audio_capture_pin_ = GetPinByName(capture_filter_.Get(), PINDIR_INPUT, 
     /*"Audio In"*/ "1");
  if (input_audio_capture_pin_.Get() == NULL) {
    LOG(ERROR) << "Failed to get audio input pin";
    return E_OUTOFMEMORY;
  }
#endif

  hr = capture_graph_builder_->FindPin(capture_filter_.Get(), PINDIR_OUTPUT,
      &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, TRUE, 
      0, &output_video_capture_pin_);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to find video output pin", hr);
  if (FAILED(hr))
    return hr;

  if (output_video_capture_pin_.Get() == NULL) {
    LOG(ERROR) << "Failed to get device video pin";
    return E_OUTOFMEMORY;
  }

  hr = capture_graph_builder_->FindPin(capture_filter_.Get(), PINDIR_OUTPUT,
      &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, TRUE, 
      0, &output_audio_capture_pin_);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to find audio output pin", hr);
  if (FAILED(hr))
    return hr;

  if (output_audio_capture_pin_.Get() == NULL) {
    LOG(ERROR) << "Failed to get device audio pin";
    return E_OUTOFMEMORY;
  }
#endif

#if 1
  LOG(INFO) << "output_audio_capture_pin_";
  PrintPinInfo(output_audio_capture_pin_.Get());

  LOG(INFO) << "output_video_capture_pin_";
  PrintPinInfo(output_video_capture_pin_.Get());
#endif

#ifdef AVERMEDIA
  hr = graph_builder_->AddFilter(crossbar_filter_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to add the crossbar to the graph",
                              hr);
  if (FAILED(hr))
    return hr;
#endif

  hr = graph_builder_->AddFilter(capture_filter_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to add the capture device to the graph",
                              hr);
  if (FAILED(hr))
    return hr;

  hr = graph_builder_->AddFilter(audio_sink_filter_.get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to add the sink filter to the graph", hr);
  if (FAILED(hr))
    return hr;

#ifndef ALAX_AUDIO_TEST
  hr = graph_builder_->AddFilter(null_renderer_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to add the sink filter to the graph", hr);
  if (FAILED(hr))
    return hr;
#endif

///////// start
#ifndef ALAX_AUDIO_TEST
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

  // Get the windows capability from the capture device.
  // GetStreamCaps can return S_FALSE which we consider an error. Therefore the
  // FAILED macro can't be used.
  int cap_index = 1;
  hr = stream_config->GetStreamCaps(cap_index,
                                    media_type.Receive(), caps.get());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to get capture device capabilities", hr);
  if (hr != S_OK) {
    return hr;
  }

  // Order the capture device to use this format.
  hr = stream_config->SetFormat(media_type.get());
  DLOG_IF_FAILED_WITH_HRESULT("Failed to set capture device output format", hr);
  if (FAILED(hr)) {
   return hr;
  }

#if 0
  ScopedComPtr<IAMBufferNegotiation> neg;
  hr = output_audio_capture_pin_.CopyTo(neg.GetAddressOf());
  DLOG_IF_FAILED_WITH_HRESULT("Can't get the iam buffer negotiation", hr);
  if (FAILED(hr)) {
    return hr;
  }
#endif

  WAVEFORMATEX* format = new WAVEFORMATEX;
  format->wFormatTag = WAVE_FORMAT_PCM;
  format->nSamplesPerSec = 48000; // params.sample_rate();
  format->wBitsPerSample = 16; // params.bits_per_sample();
  format->nChannels = 2; // params.channels();
  format->nBlockAlign = (format->wBitsPerSample / 8) * format->nChannels;
  format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
  format->cbSize = 0;
  /* ResetFormat(format); */

#if 0
  ALLOCATOR_PROPERTIES props;
  props.cBuffers = -1;
  props.cbBuffer = format->nAvgBytesPerSec * 20 / 1000;
  props.cbAlign = -1;
  props.cbPrefix = -1;
  hr = neg->SuggestAllocatorProperties(&props);
#endif


  delete format;
 
#else 
  ScopedComPtr<IAMBufferNegotiation> neg;
  hr = output_audio_capture_pin_.CopyTo(neg.GetAddressOf());
  DLOG_IF_FAILED_WITH_HRESULT("Can't get the iam buffer negotiation", hr);
  if (FAILED(hr)) {
    return hr;
  }


  WAVEFORMATEX* format = new WAVEFORMATEX;
  format->wFormatTag = WAVE_FORMAT_PCM;
  format->nSamplesPerSec = 48000; // params.sample_rate();
  format->wBitsPerSample = 16; // params.bits_per_sample();
  format->nChannels = 2; // params.channels();
  format->nBlockAlign = (format->wBitsPerSample / 8) * format->nChannels;
  format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
  format->cbSize = 0;

  /* ResetFormat(format); */

  ALLOCATOR_PROPERTIES props;
  props.cBuffers = -1;
  props.cbBuffer = format->nAvgBytesPerSec * 20 / 1000;
  props.cbAlign = -1;
  props.cbPrefix = -1;
  hr = neg->SuggestAllocatorProperties(&props);

  delete format;
#endif

  return hr;
}


HRESULT DirectShow::InitializeAudioEngine() {
  return S_OK;
}

/* void DirectShow::ReportOpenResult() const { */
/*   DCHECK(!opened_);  // This method must be called before we set this flag. */
/*   UMA_HISTOGRAM_ENUMERATION("Media.Audio.Capture.Win.Open", open_result_, */
/*                             OPEN_RESULT_MAX + 1); */
/* } */

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

      WCHAR *pin_id = new WCHAR[128];
      hr = pin->QueryId(reinterpret_cast<LPWSTR*>(&pin_id));
      if (SUCCEEDED(hr)) {
        const std::string this_pin_name(base::SysWideToUTF8(pin_id));

        if (pin_name.compare(this_pin_name) == 0) {
          delete[] pin_id;
          return pin;
        }
      }
      delete[] pin_id;
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

  if (!running_) {

    capture_thread_.reset(new base::DelegateSimpleThread(
        this, "capture_thread",
        base::SimpleThread::Options(base::ThreadPriority::REALTIME_AUDIO)));
    capture_thread_->Start();
    running_ = true;
  }
}

void DirectShow::RegisterObserver(AudioSinkFilterObserver* observer) {
    audio_observer_ = observer;
    EnsureGraphIsRunning();

    WAVEFORMATEXTENSIBLE* format = new WAVEFORMATEXTENSIBLE(format_);
    observer->FormatChanged(format);
}

void DirectShow::UnregisterObserver(AudioSinkFilterObserver* observer) {
    audio_observer_ = nullptr;
    //ShouldGraphBeRunning(); FIXME
}

void DirectShow::AudioFrameReceived(const uint8_t* buffer,
                                          int length,
                                          base::TimeDelta timestamp) {

  if (audio_observer_ != NULL) {  // TODO make atomic
    audio_observer_->AudioFrameReceived(buffer,length, timestamp);
  }
}

} // namespace media
