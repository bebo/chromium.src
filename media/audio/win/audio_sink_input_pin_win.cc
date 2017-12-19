// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/win/audio_sink_input_pin_win.h"

#include <cstring>

// Avoid including strsafe.h via dshow as it will cause build warnings.
#define NO_DSHOW_STRSAFE
#include <dshow.h>
#include <stdint.h>

#include "base/logging.h"
#include "base/macros.h"
#include "media/base/timestamp_constants.h"

namespace media {

GUID kMediaSubTypeI420 = {0x30323449,
                          0x0000,
                          0x0010,
                          {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// UYVY synonym with BT709 color components, used in HD video. This variation
// might appear in non-USB capture cards and it's implemented as a normal YUV
// pixel format with the characters HDYC encoded in the first array word.
GUID kMediaSubTypeHDYC = {0x43594448,
                          0x0000,
                          0x0010,
                          {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

GUID kMediaSubTypeZ16 = {0x2036315a,
                         0x0000,
                         0x0010,
                         {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
GUID kMediaSubTypeINVZ = {0x5a564e49,
                          0x2d90,
                          0x4a58,
                          {0x92, 0x0b, 0x77, 0x3f, 0x1f, 0x2c, 0x55, 0x6b}};
GUID kMediaSubTypeY16 = {0x20363159,
                         0x0000,
                         0x0010,
                         {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

  
const REFERENCE_TIME kSecondsToReferenceTime = 10000000;

static DWORD GetArea(const BITMAPINFOHEADER& info_header) {
  return info_header.biWidth * info_header.biHeight;
}

AudioSinkInputPin::AudioSinkInputPin(IBaseFilter* filter, AudioSinkFilterObserver* observer)
    : AudioPinBase(filter), observer_(observer) {
}

bool AudioSinkInputPin::IsMediaTypeValid(const AM_MEDIA_TYPE* media_type) {
  const GUID type = media_type->majortype;
  if (type != MEDIATYPE_Audio) {
    LOG(INFO) << "type != MEDIATYPE_Audio";
    return false;
  }

  const GUID format_type = media_type->formattype;
  if (format_type != FORMAT_WaveFormatEx) {
    LOG(INFO) << "format_type != FORMAT_WaveFormatEx";
    return false;
  }

  // Check for the sub types we support.
  const GUID sub_type = media_type->subtype;
  WAVEFORMATEX* pvi =
      reinterpret_cast<WAVEFORMATEX*>(media_type->pbFormat);
  if (pvi == NULL) {
    LOG(INFO) << "pvi == NULL";
    return false;
  }

  // Store the incoming width and height.
#if 0
  resulting_format_.frame_size.SetSize(pvi->bmiHeader.biWidth,
                                       abs(pvi->bmiHeader.biHeight));
  if (pvi->AvgTimePerFrame > 0) {
    resulting_format_.frame_rate =
        static_cast<int>(kSecondsToReferenceTime / pvi->AvgTimePerFrame);
  } else {
    resulting_format_.frame_rate = requested_frame_rate_;
  }
#endif

  LOG(INFO) << "IsMediaTypeVaild";
  if (sub_type == MEDIASUBTYPE_PCM) {
    return true;
  }

  return false;
}

bool AudioSinkInputPin::GetValidMediaType(int index, AM_MEDIA_TYPE* media_type) { 
  LOG(INFO) << "GetValidMediaType, index: " << index;

  if (media_type->cbFormat < sizeof(WAVEFORMATEX)) {
    LOG(INFO) << "media_type->cbFormat < sizeof(WAVEFORMATEX)";
    return false;
  }

  media_type->majortype = MEDIATYPE_Audio;
  media_type->subtype = MEDIASUBTYPE_PCM;
  media_type->formattype = FORMAT_WaveFormatEx;
  media_type->bFixedSizeSamples = TRUE;
  media_type->bTemporalCompression = FALSE;
  media_type->cbFormat = sizeof(WAVEFORMATEX);

  WAVEFORMATEX* const format = reinterpret_cast<WAVEFORMATEX*>(media_type->pbFormat);
  format->wFormatTag = WAVE_FORMAT_PCM;
  format->nSamplesPerSec = 48000; // params.sample_rate();
  format->wBitsPerSample = 16; // params.bits_per_sample();
  format->nChannels = 2; // params.channels();
  format->nBlockAlign = (format->wBitsPerSample / 8) * format->nChannels;
  format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
  format->cbSize = 0;

  if (index == 1) return false;

  return true;
}

HRESULT AudioSinkInputPin::Receive(IMediaSample* sample) {
  const int length = sample->GetActualDataLength();
  LOG(INFO) << "Receive, length: " << length;

#if 0
  if (length <= 0 ||
      static_cast<size_t>(length) < resulting_format_.ImageAllocationSize()) {
    DLOG(WARNING) << "Wrong media sample length: " << length;
    return S_FALSE;
  }
#endif

  uint8_t* buffer = nullptr;
  if (FAILED(sample->GetPointer(&buffer)))
    return S_FALSE;

  REFERENCE_TIME start_time, end_time;
  base::TimeDelta timestamp = media::kNoTimestamp;
  if (SUCCEEDED(sample->GetTime(&start_time, &end_time))) {
    DCHECK(start_time <= end_time);
    timestamp = base::TimeDelta::FromMicroseconds(start_time / 10);
  }

  observer_->FrameReceived(buffer, length, timestamp);
  return S_OK;
}

AudioSinkInputPin::~AudioSinkInputPin() {
}

}  // namespace media
