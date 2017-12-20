// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/win/directsound_input_win.h"

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/utf_string_conversions.h"
#include "base/trace_event/trace_event.h"
#include "media/audio/audio_device_description.h"
#include "media/audio/win/audio_manager_win.h"
#include "media/audio/win/avrt_wrapper_win.h"
#include "media/audio/win/core_audio_util_win.h"
#include "media/base/audio_block_fifo.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_timestamp_helper.h"
#include "media/base/channel_layout.h"
#include "media/base/limits.h"
#include "media/base/timestamp_constants.h"

EXTERN_C const CLSID CLSID_NullRenderer;

using base::win::ScopedCoMem;
using base::win::ScopedComPtr;
using base::win::ScopedCOMInitializer;
using base::win::ScopedVariant;

#define ALAX_AUDIO_TEST

GUID kElgatoGameCaptureHD = {0x39f50f4c,
                          0x99E1,
                          0x464A,
                          {0xb6, 0xf9, 0xd6, 0x05, 0xb4, 0xfb, 0x59, 0x18}};
GUID kAlaxAudioTestSrc = {0xbf53d5ec,
                          0xa137,
                          0x4a3e,
                          {0xb7, 0xcf, 0xb6, 0x73, 0x56, 0x8a, 0x9d, 0x8b}};

enum FILTER {
  FILTER_ELGATO_GAME_CAPTURE_HD = 0,
  FILTER_MAX = FILTER_ELGATO_GAME_CAPTURE_HD,
};
const int kFilterSize = FILTER_MAX + 1;

#ifdef ALAX_AUDIO_TEST
const GUID kFilterArray[kFilterSize] = {kAlaxAudioTestSrc}; // kElgatoGameCaptureHD
#else
const GUID kFilterArray[kFilterSize] = {kElgatoGameCaptureHD};
#endif

const std::string kFilterArrayName[kFilterSize] = {"Elgato Game Capture HD"};

namespace media {

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
bool IsSupportedFormatForConversion(const WAVEFORMATEX& format) {
  if (format.nSamplesPerSec < limits::kMinSampleRate ||
      format.nSamplesPerSec > limits::kMaxSampleRate) {
    return false;
  }

  switch (format.wBitsPerSample) {
    case 8:
    case 16:
    case 32:
      break;
    default:
      return false;
  }

  if (GuessChannelLayout(format.nChannels) == CHANNEL_LAYOUT_UNSUPPORTED) {
    LOG(ERROR) << "Hardware configuration not supported for audio conversion";
    return false;
  }

  return true;
}
}  // namespace

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


void PrintPinInfo(IPin* pin) {
  HRESULT hr;

  if (!pin) {
    LOG(INFO) << "pin == null";
    return;
  }

  AM_MEDIA_TYPE media_type = {0};
  hr = pin->ConnectionMediaType(&media_type);
  if (SUCCEEDED(hr)) {
    WCHAR major_type[128];
    StringFromGUID2(media_type.majortype, major_type, arraysize(major_type));

    WCHAR sub_type[128];
    StringFromGUID2(media_type.subtype, sub_type, arraysize(sub_type));

    WCHAR format_type[128];
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

DirectSoundAudioInputStream::DirectSoundAudioInputStream(AudioManagerWin* manager,
                                               const AudioParameters& params,
                                               const std::string& device_id)
    : manager_(manager), device_id_(device_id), params_(params) {
  DCHECK(manager_);
  DCHECK(!device_id_.empty());

  is_loopback_device_ = device_id.compare(AudioDeviceDescription::kLoopbackInputDeviceId) == 0;
  friendly_name_ = "DirectSound";

  // Load the Avrt DLL if not already loaded. Required to support MMCSS.
  bool avrt_init = avrt::Initialize();
  DCHECK(avrt_init) << "Failed to load the Avrt.dll";

  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::DirectSoundAudioInputStream "
    << params.AsHumanReadableString();

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
  // Store size of audio packets which we expect to get from the audio
  // endpoint device in each capture event.
  packet_size_frames_ = params.GetBytesPerBuffer() / format->nBlockAlign;
  packet_size_bytes_ = params.GetBytesPerBuffer();

  // 3840
#ifdef ALAX_AUDIO_TEST
  // mono
  endpoint_buffer_size_frames_ = 3840 / (format->nBlockAlign / 2); // 20 * 480 / format->nBlockAlign;
#else
  endpoint_buffer_size_frames_ = 4608 / format->nBlockAlign; // 20 * 480 / format->nBlockAlign;
#endif

  DVLOG(1) << "Number of bytes per audio frame  : " << frame_size_;
  DVLOG(1) << "Number of audio frames per packet: " << packet_size_frames_;
  LOG(INFO) << friendly_name_ << " Number of bytes per audio frame  : " << frame_size_;
  LOG(INFO) << friendly_name_ << " Number of audio frames per packet: " << packet_size_frames_;

  // All events are auto-reset events and non-signaled initially.
  stop_capture_event_.Set(CreateEvent(NULL, FALSE, FALSE, NULL));
  DCHECK(stop_capture_event_.IsValid());
}

DirectSoundAudioInputStream::~DirectSoundAudioInputStream() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

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

// Finds and creates a DirectShow Video Capture filter matching the |device_id|.
// static
HRESULT DirectSoundAudioInputStream::GetDeviceFilter(const std::string& device_id,
                                               IBaseFilter** filter) {
  DCHECK(filter);

  // go through our whitelisted filters first (bebo-game-capture, capture cards)
  // so that we won't fail on the shortcircuit for when no camera exist in the OS
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
}



bool DirectSoundAudioInputStream::Open() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK_EQ(OPEN_RESULT_OK, open_result_);

  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::Open()";

  opened_ = true; // SUCCEEDED(hr);

  return opened_;
}

void DirectSoundAudioInputStream::Start(AudioInputCallback* callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(callback);
  DLOG_IF(ERROR, !opened_) << "Open() has not been called successfully";

  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::Start()";

  if (!opened_) {
    LOG(ERROR) << friendly_name_ << " DirectSoundAudioInputStream::Start() - Open() has not been called successfully";
    return;
  }

  if (started_) {
    return;
  }

  sink_ = callback;

  capture_thread_.reset(new base::DelegateSimpleThread(
      this, "capture_thread",
      base::SimpleThread::Options(base::ThreadPriority::REALTIME_AUDIO)));
  capture_thread_->Start();

  started_ = true; // SUCCEEDED(hr);

  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::Start() " << started_;
}

void DirectSoundAudioInputStream::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(1) << "DirectSoundAudioInputStream::Stop()";
  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::Stop()";

  if (!started_)
    return;

  if (capture_thread_) {
    SetEvent(stop_capture_event_.Get());
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

  started_ = false;
  sink_ = NULL;
}

void DirectSoundAudioInputStream::Close() {
  DVLOG(1) << "DirectSoundAudioInputStream::Close()";
  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::Close()";
  // It is valid to call Close() before calling open or Start().
  // It is also valid to call Close() after Start() has been called.
  Stop();

  if (converter_)
    converter_->RemoveInput(this);

  // Inform the audio manager that we have been closed. This will cause our
  // destruction.
  manager_->ReleaseInputStream(this);
}

double DirectSoundAudioInputStream::GetMaxVolume() {
  // Verify that Open() has been called succesfully, to ensure that an audio
  // session exists and that an ISimpleAudioVolume interface has been created.
  DLOG_IF(ERROR, !opened_) << "Open() has not been called successfully";
  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::GetMaxVolume()";
  if (!opened_)
    return 0.0;

  // The effective volume value is always in the range 0.0 to 1.0, hence
  // we can return a fixed value (=1.0) here.
  return 1.0;
}

void DirectSoundAudioInputStream::SetVolume(double volume) {
  DVLOG(1) << "SetVolume(volume=" << volume << ")";
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK_GE(volume, 0.0);
  DCHECK_LE(volume, 1.0);
  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::SetVolume(" << volume << ")";

  DLOG_IF(ERROR, !opened_) << "Open() has not been called successfully";
  if (!opened_)
    return;
}

double DirectSoundAudioInputStream::GetVolume() {
  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::GetVolume()";
  DCHECK(opened_) << "Open() has not been called successfully";
  if (!opened_)
    return 0.0;

  // Retrieve the current volume level. The value is in the range 0.0 to 1.0.
  return static_cast<double>(1.0f);
}

bool DirectSoundAudioInputStream::IsMuted() {
  DCHECK(opened_) << "Open() has not been called successfully";
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(3) << friendly_name_ << " DirectSoundAudioInputStream::IsMuted()";
  // LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::IsMuted()";
  if (!opened_)
    return false;

  return false;
}

void DirectSoundAudioInputStream::Run() {
  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::Run()";

  HRESULT hr = SetCaptureDevice();
  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::Run() - SetCaptureDevice - " << hr;

  size_t capture_buffer_size =
    std::max(2 * endpoint_buffer_size_frames_ * frame_size_,
        2 * packet_size_frames_ * frame_size_);
  int buffers_required = capture_buffer_size / packet_size_bytes_;
  if (converter_ && imperfect_buffer_size_conversion_)
    ++buffers_required;

  DCHECK(!fifo_);
  LOG(INFO) << "DirectSoundAudioInputStream::Run() about to reset fifo";

  fifo_.reset(new AudioBlockFifo(format_.Format.nChannels, packet_size_frames_,
                                 buffers_required));

  LOG(INFO) << "DirectSoundAudioInputStream::Run() about to connect direct (audio) ";
  hr = graph_builder_->ConnectDirect(output_audio_capture_pin_.Get(),
      input_audio_sink_pin_.Get(), NULL);
  DLOG_IF_FAILED_WITH_HRESULT("Failed to connect the Capture graph", hr);
  if (FAILED(hr)) {
    return;
  }

#ifndef ALAX_AUDIO_TEST
  LOG(INFO) << "DirectSoundAudioInputStream::Run() about to connect direct (video)";

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

  LOG(INFO) << "DirectSoundAudioInputStream::Run() about to pause";
  hr = media_control_->Pause();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to pause the Capture device", hr);
  if (FAILED(hr)) {
    return;
  }

  LOG(INFO) << "DirectSoundAudioInputStream::Run() about to run";

  // Start capturing.
  hr = media_control_->Run();
  DLOG_IF_FAILED_WITH_HRESULT("Failed to start the Capture device", hr);
  if (FAILED(hr)) {
    return;
  }

  LOG(INFO) << "DirectSoundAudioInputStream::Run() running";

  DWORD ret = WaitForSingleObject(stop_capture_event_.Get(), INFINITE);
  LOG(INFO) << "DirectSoundAudioInputStream::Run() Ret: " << ret;
}


void DirectSoundAudioInputStream::HandleError(HRESULT err) {
  NOTREACHED() << "Error code: " << err;
  if (sink_)
    sink_->OnError();
}

HRESULT DirectSoundAudioInputStream::SetCaptureDevice() {
  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::SetCaptureDevice()";

////// open
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

  hr = GetDeviceFilter(device_id_,
                      capture_filter_.GetAddressOf());
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

  // DebugBreak();

#ifdef ALAX_AUDIO_TEST
  output_audio_capture_pin_ = GetPin(capture_filter_.Get(), PINDIR_OUTPUT,
      GUID_NULL, GUID_NULL);
#else 
  hr = capture_graph_builder_->FindPin(capture_filter_.Get(), PINDIR_OUTPUT,
      &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, TRUE, 
      0, &output_video_capture_pin_);

  DLOG_IF_FAILED_WITH_HRESULT("Failed to find audio output pin", hr);
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
  ResetFormat(format);

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

  ResetFormat(format);

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

void DirectSoundAudioInputStream::ResetFormat(WAVEFORMATEX* request_format) {
  DCHECK(!is_loopback_device_); 

  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::ResetFormat()";

  // Ideally, we want a 1:1 ratio between the buffers we get and the buffers
  // we give to OnData so that each buffer we receive from the OS can be
  // directly converted to a buffer that matches with what was asked for.
  const double buffer_ratio =
    format_.Format.nSamplesPerSec / static_cast<double>(packet_size_frames_);
  double new_frames_per_buffer = request_format->nSamplesPerSec / buffer_ratio;

  LOG(INFO) << "buffer_ratio: " << buffer_ratio << ", new_frames_per_buffer: " << new_frames_per_buffer;

  const auto input_layout = GuessChannelLayout(request_format->nChannels);
  DCHECK_NE(CHANNEL_LAYOUT_UNSUPPORTED, input_layout);
  const auto output_layout = GuessChannelLayout(params_.channels());
  DCHECK_NE(CHANNEL_LAYOUT_UNSUPPORTED, output_layout);

  LOG(INFO) << "Input: nSamplesPerSec: " << request_format->nSamplesPerSec << ", wBitsPerSample: " << request_format->wBitsPerSample << ", Channels: " << request_format->nChannels;
  LOG(INFO) << "Output: nSamplesPerSec: " << params_.sample_rate() << ", wBitsPerSample: " << params_.bits_per_sample() << ", Channels: " << params_.channels();

  const AudioParameters input(AudioParameters::AUDIO_PCM_LOW_LATENCY,
      input_layout,
      request_format->nSamplesPerSec,
      request_format->wBitsPerSample,
      static_cast<int>(new_frames_per_buffer));

  const AudioParameters output(AudioParameters::AUDIO_PCM_LOW_LATENCY,
      output_layout, params_.sample_rate(),
      params_.bits_per_sample(), packet_size_frames_);

  converter_.reset(new AudioConverter(input, output, false));
  converter_->AddInput(this);
  converter_->PrimeWithSilence();
  convert_bus_ = AudioBus::Create(output);

  // Now change the format we're going to ask for to better match with what
  // the OS can provide.  If we succeed in opening the stream with these
  // params, we can take care of the required resampling.
  format_.Format.wBitsPerSample = request_format->wBitsPerSample;
  format_.Format.nSamplesPerSec = request_format->nSamplesPerSec;
  format_.Format.nChannels = request_format->nChannels;
  format_.Format.nBlockAlign = (format_.Format.wBitsPerSample / 8) * format_.Format.nChannels;
  format_.Format.nAvgBytesPerSec = format_.Format.nSamplesPerSec * format_.Format.nBlockAlign;

  format_.Samples.wValidBitsPerSample = request_format->wBitsPerSample;

  LOG(INFO) << "Will convert audio from: bits: " << format_.Format.wBitsPerSample
    << ", sample rate: " << format_.Format.nSamplesPerSec
    << ", channels: " << format_.Format.nChannels
    << ", block align: " << format_.Format.nBlockAlign
    << ", avg bytes per sec: " << format_.Format.nAvgBytesPerSec;

  // Update our packet size assumptions based on the new format.
  const auto new_bytes_per_buffer =
    static_cast<int>(new_frames_per_buffer) * format_.Format.nBlockAlign;
  packet_size_frames_ = new_bytes_per_buffer / format_.Format.nBlockAlign;
  packet_size_bytes_ = new_bytes_per_buffer;
  frame_size_ = format_.Format.nBlockAlign;

  imperfect_buffer_size_conversion_ =
    std::modf(new_frames_per_buffer, &new_frames_per_buffer) != 0.0;

  if (imperfect_buffer_size_conversion_) {
    LOG(INFO) << "Audio capture data conversion: Need to inject fifo";
  }
}

HRESULT DirectSoundAudioInputStream::InitializeAudioEngine() {
  return S_OK;
}

void DirectSoundAudioInputStream::ReportOpenResult() const {
  DCHECK(!opened_);  // This method must be called before we set this flag.
  UMA_HISTOGRAM_ENUMERATION("Media.Audio.Capture.Win.Open", open_result_,
                            OPEN_RESULT_MAX + 1);
}

double DirectSoundAudioInputStream::ProvideInput(AudioBus* audio_bus,
                                            uint32_t frames_delayed) {
  fifo_->Consume()->CopyTo(audio_bus);
  return 1.0;
}

// Finds an IPin on an IBaseFilter given the direction, Category and/or Major
// Type. If either |category| or |major_type| are GUID_NULL, they are ignored.
// static
ScopedComPtr<IPin> DirectSoundAudioInputStream::GetPin(IBaseFilter* filter,
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


void DirectSoundAudioInputStream::ScopedMediaType::Free() {
  if (!media_type_)
    return;

  DeleteMediaType(media_type_);
  media_type_ = NULL;
}

AM_MEDIA_TYPE** DirectSoundAudioInputStream::ScopedMediaType::Receive() {
  DCHECK(!media_type_);
  return &media_type_;
}

// Release the format block for a media type.
// http://msdn.microsoft.com/en-us/library/dd375432(VS.85).aspx
void DirectSoundAudioInputStream::ScopedMediaType::FreeMediaType(AM_MEDIA_TYPE* mt) {
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
void DirectSoundAudioInputStream::ScopedMediaType::DeleteMediaType(
    AM_MEDIA_TYPE* mt) {
  if (mt != NULL) {
    FreeMediaType(mt);
    CoTaskMemFree(mt);
  }
}

void DirectSoundAudioInputStream::FrameReceived(const uint8_t* buffer,
                                          int length,
                                          base::TimeDelta timestamp) {
  if (first_ref_time_.is_null()) {
    first_ref_time_ = base::TimeTicks::Now();
    LOG(INFO) << "frame received length: " << length;
  }

  base::TimeTicks reference_time = base::TimeTicks::Now();

  // There is a chance that the platform does not provide us with the timestamp,
  // in which case, we use reference time to calculate a timestamp.
  if (timestamp == media::kNoTimestamp)
    timestamp = base::TimeTicks::Now() - first_ref_time_;

  // |audio_samples_ready_event_| has been set.
  UINT32 num_frames_to_read = length / format_.Format.nBlockAlign; // (length / nBlockAlign)

  base::TimeTicks capture_time = first_ref_time_ + timestamp;

  // Adjust |capture_time| for the FIFO before pushing.
  capture_time -= AudioTimestampHelper::FramesToTime(
      fifo_->GetAvailableFrames(), format_.Format.nSamplesPerSec);

  fifo_->Push(buffer, num_frames_to_read, 
      format_.Format.wBitsPerSample / 8);

  // Deliver captured data to the registered consumer using a packet
  // size which was specified at construction.

  float volume = 1.0f;
  while (fifo_->available_blocks()) {
    if (converter_) {
      if (imperfect_buffer_size_conversion_ &&
          fifo_->available_blocks() == 1) {
        // Special case. We need to buffer up more audio before we can
        // convert or else we'll suffer an underrun.
        break;
      }
      converter_->Convert(convert_bus_.get());
      sink_->OnData(convert_bus_.get(), capture_time, volume);

      // Move the capture time forward for each vended block.
      capture_time += AudioTimestampHelper::FramesToTime(
          convert_bus_->frames(), format_.Format.nSamplesPerSec);
    } else {
      sink_->OnData(fifo_->Consume(), capture_time, volume);

      // Move the capture time forward for each vended block.
      capture_time += AudioTimestampHelper::FramesToTime(
          packet_size_frames_, format_.Format.nSamplesPerSec);
    }
  }
}


}  // namespace media
