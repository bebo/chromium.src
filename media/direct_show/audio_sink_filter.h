// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implement a DirectShow sink filter used for receiving captured frames from
// a DirectShow Capture filter.

#ifndef MEDIA_AUDIO_WIN_SINK_FILTER_WIN_H_
#define MEDIA_AUDIO_WIN_SINK_FILTER_WIN_H_

#include <windows.h>
#include <stddef.h>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "media/direct_show/direct_show_filter_base.h"
#include "media/direct_show/audio_sink_filter_observer.h"

namespace media {

class AudioSinkInputPin;

class __declspec(uuid("88cdbbdc-a73b-4afa-acbf-15d5e2ce12d4")) AudioSinkFilter
    : public AudioFilterBase {
 public:
  explicit AudioSinkFilter(AudioSinkFilterObserver* observer);

  // Implement AudioFilterBase.
  size_t NoOfPins() override;
  IPin* GetPin(int index) override;

  STDMETHOD(GetClassID)(CLSID* clsid) override;

 private:
  ~AudioSinkFilter() override;

  scoped_refptr<AudioSinkInputPin> input_pin_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(AudioSinkFilter);
};

}  // namespace media

#endif  // MEDIA_AUDIO_WIN_SINK_FILTER_WIN_H_
