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

void AudioSinkInputPin::SetRequestedMediaFormat(WAVEFORMATEX format) {
  requested_format_ = format;
}

bool AudioSinkInputPin::IsMediaTypeValid(const AM_MEDIA_TYPE* media_type) {
  const GUID type = media_type->majortype;
  if (type != MEDIATYPE_Audio) {
    return false;
  }

  const GUID format_type = media_type->formattype;
  if (format_type != FORMAT_WaveFormatEx) {
    return false;
  }

  // Check for the sub types we support.
  const GUID sub_type = media_type->subtype;

  // for now let's prefer PCM only
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

  LOG(INFO) << "IsMediaTypeVaild";
  return true;
}

bool AudioSinkInputPin::GetValidMediaType(int index, AM_MEDIA_TYPE* media_type) { 
  if (media_type->cbFormat < sizeof(WAVEFORMATEX)) {
    return false;
  }

  media_type->majortype = MEDIATYPE_Audio;
  media_type->subtype = MEDIASUBTYPE_PCM;
  media_type->formattype = FORMAT_WaveFormatEx;
  media_type->bFixedSizeSamples = TRUE;
  media_type->bTemporalCompression = FALSE;
  media_type->cbFormat = sizeof(WAVEFORMATEX);

  WAVEFORMATEX* const format = reinterpret_cast<WAVEFORMATEX*>(media_type->pbFormat);

  // we only take in PCM
  switch (index) {
    case 0: {
      format->wFormatTag = WAVE_FORMAT_PCM;
      format->nSamplesPerSec = requested_format_.nSamplesPerSec;
      format->wBitsPerSample = requested_format_.wBitsPerSample;
      format->nChannels = requested_format_.nChannels;
      format->nBlockAlign = requested_format_.nBlockAlign;
      format->nAvgBytesPerSec = requested_format_.nAvgBytesPerSec;
      format->cbSize = 0;
      break;
    }
    default:
      return false;
  }

  return true;
}

HRESULT AudioSinkInputPin::Receive(IMediaSample* sample) {
  const int length = sample->GetActualDataLength();

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
