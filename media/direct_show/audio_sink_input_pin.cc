// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/direct_show/audio_sink_input_pin.h"

#include <cstring>

// Avoid including strsafe.h via dshow as it will cause build warnings.
#define NO_DSHOW_STRSAFE
#include <dshow.h>
#include <stdint.h>

#include "base/logging.h"
#include "base/macros.h"
#include "media/base/timestamp_constants.h"

namespace media {

AudioSinkInputPin::AudioSinkInputPin(IBaseFilter* filter, AudioSinkFilterObserver* observer)
    : DirectShowPinBase(filter), observer_(observer) {
}

bool AudioSinkInputPin::IsMediaTypeValid(const AM_MEDIA_TYPE* media_type) {
  const GUID type = media_type->majortype;
  if (type != MEDIATYPE_Audio) {
    WCHAR guid_str[128];
    StringFromGUID2(type, guid_str, arraysize(guid_str));
    LOG(INFO) << "type != MEDIATYPE_Audio, " << guid_str;
    return false;
  }

  const GUID format_type = media_type->formattype;
  if (format_type != FORMAT_WaveFormatEx) {
    WCHAR guid_str[128];
    StringFromGUID2(format_type, guid_str, arraysize(guid_str));
    LOG(INFO) << "format_type != FORMAT_WaveFormatEx, " << guid_str;
    return false;
  }

  // Check for the sub types we support.
  const GUID sub_type = media_type->subtype;

  if (sub_type != MEDIASUBTYPE_PCM) {
    WCHAR guid_str[128];
    StringFromGUID2(sub_type, guid_str, arraysize(guid_str));
    LOG(INFO) << "sub_type != MEDIASUBTYPE_PCM, " << guid_str;
    return false;
  }

  WAVEFORMATEX* pvi =
      reinterpret_cast<WAVEFORMATEX*>(media_type->pbFormat);
  if (pvi == NULL) {
    LOG(INFO) << "pvi == NULL";
    return false;
  }

  if (pvi->nSamplesPerSec != 48000) {
    LOG(INFO) << "wfex->nSamplesPerSec != 48000";
    return false;
  }

  LOG(INFO) << "IsMediaTypeVaild";
  return true;
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

  observer_->AudioFrameReceived(buffer, length, timestamp);
  return S_OK;
}

AudioSinkInputPin::~AudioSinkInputPin() {
}

}  // namespace media
