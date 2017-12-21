// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/direct_show/audio_sink_filter.h"

#include "base/logging.h"
#include "media/direct_show/audio_sink_input_pin.h"

namespace media {

AudioSinkFilterObserver::~AudioSinkFilterObserver() {
}

AudioSinkFilter::AudioSinkFilter(AudioSinkFilterObserver* observer) : input_pin_(NULL) {
  input_pin_ = new AudioSinkInputPin(this, observer);
}

size_t AudioSinkFilter::NoOfPins() {
  return 1;
}

IPin* AudioSinkFilter::GetPin(int index) {
  return index == 0 ? input_pin_.get() : NULL;
}

STDMETHODIMP AudioSinkFilter::GetClassID(CLSID* clsid) {
  *clsid = __uuidof(AudioSinkFilter);
  return S_OK;
}

AudioSinkFilter::~AudioSinkFilter() {
  input_pin_->SetOwner(NULL);
}

}  // namespace media
