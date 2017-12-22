// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implement a DirectShow input pin used for receiving captured frames from
// a DirectShow Capture filter.

#ifndef MEDIA_AUDIO_WIN_SINK_INPUT_PIN_WIN_H_
#define MEDIA_AUDIO_WIN_SINK_INPUT_PIN_WIN_H_

#include "base/macros.h"
#include "media/direct_show/direct_show_pin_base.h"
#include "media/direct_show/audio_sink_filter.h"

namespace media {

// Input pin of the SinkFilter.
class AudioSinkInputPin : public DirectShowPinBase {
 public:
  AudioSinkInputPin(IBaseFilter* filter, AudioSinkFilterObserver* observer);

  // Implement PinBase.
  bool IsMediaTypeValid(const AM_MEDIA_TYPE* media_type) override;
  bool GetValidMediaType(int index, AM_MEDIA_TYPE* media_type) override;

  STDMETHOD(Receive)(IMediaSample* media_sample) override;

 private:
  ~AudioSinkInputPin() override;

  AudioSinkFilterObserver* observer_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(AudioSinkInputPin);
};

}  // namespace media

#endif  // MEDIA_AUDIO_WIN_SINK_INPUT_PIN_WIN_H_
