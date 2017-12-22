// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Implementation of AudioInputStream for Windows using Windows Core Audio
// WASAPI for low latency capturing.
//
#ifndef MEDIA_AUDIO_WIN_AUDIO_DIRECTSOUND_INPUT_WIN_H_
#define MEDIA_AUDIO_WIN_AUDIO_DIRECTSOUND_INPUT_WIN_H_

/* #include <Audioclient.h> */
/* #include <MMDeviceAPI.h> */
#include <endpointvolume.h>
#include <stddef.h>
#include <stdint.h>

#undef AM_KSCATEGORY_AUDIO
#include <dshow.h>

#include <memory>
#include <string>

#include "base/compiler_specific.h"
#include "base/macros.h"
#include "base/sequence_checker.h"
#include "media/audio/agc_audio_stream.h"
#include "media/base/audio_converter.h"
#include "media/base/audio_parameters.h"
#include "media/base/media_export.h"
#include "media/direct_show/audio_sink_filter_observer.h"
#include "media/direct_show/direct_show.h"

namespace media {

class AudioBlockFifo;
class AudioBus;
class AudioManagerWin;

// AudioInputStream implementation using Windows Core Audio APIs.
class MEDIA_EXPORT DirectSoundAudioInputStream
    : public AgcAudioStream<AudioInputStream>,
      public AudioConverter::InputCallback,
      public AudioSinkFilterObserver {
 public:
  // The ctor takes all the usual parameters, plus |manager| which is the
  // the audio manager who is creating this object.
  DirectSoundAudioInputStream(AudioManagerWin* manager,
                         const AudioParameters& params,
                         const std::string& device_id);

  // The dtor is typically called by the AudioManager only and it is usually
  // triggered by calling AudioInputStream::Close().
  ~DirectSoundAudioInputStream() override;

  // Implementation of AudioInputStream.
  bool Open() override;
  void Start(AudioInputCallback* callback) override;
  void Stop() override;
  void Close() override;
  double GetMaxVolume() override;
  void SetVolume(double volume) override;
  double GetVolume() override;
  bool IsMuted() override;

  bool started() const { return started_; }

 private:

  // AudioConverter::InputCallback implementation.
  double ProvideInput(AudioBus* audio_bus, uint32_t frames_delayed) override;


  // AudioSinkFilterObserverer
  void AudioFrameReceived(const uint8_t* buffer,
                         int length,
                         base::TimeDelta timestamp) override;

  void FormatChanged(WAVEFORMATEXTENSIBLE *format) override;

  // Used to track down where we fail during initialization which at the
  // moment seems to be happening frequently and we're not sure why.
  // The reason might be expected (e.g. trying to open "default" on a machine
  // that has no audio devices).
  // Note: This enum is used to record a histogram value and should not be
  // re-ordered.
  enum StreamOpenResult {
    OPEN_RESULT_OK = 0,
    OPEN_RESULT_CREATE_INSTANCE = 1,
    OPEN_RESULT_NO_ENDPOINT = 2,
    OPEN_RESULT_NO_STATE = 3,
    OPEN_RESULT_DEVICE_NOT_ACTIVE = 4,
    OPEN_RESULT_ACTIVATION_FAILED = 5,
    OPEN_RESULT_FORMAT_NOT_SUPPORTED = 6,
    OPEN_RESULT_AUDIO_CLIENT_INIT_FAILED = 7,
    OPEN_RESULT_GET_BUFFER_SIZE_FAILED = 8,
    OPEN_RESULT_LOOPBACK_ACTIVATE_FAILED = 9,
    OPEN_RESULT_LOOPBACK_INIT_FAILED = 10,
    OPEN_RESULT_SET_EVENT_HANDLE = 11,
    OPEN_RESULT_NO_CAPTURE_CLIENT = 12,
    OPEN_RESULT_NO_AUDIO_VOLUME = 13,
    OPEN_RESULT_OK_WITH_RESAMPLING = 14,
    OPEN_RESULT_MAX = OPEN_RESULT_OK_WITH_RESAMPLING
  };

  // Our creator, the audio manager needs to be notified when we close.
  AudioManagerWin* const manager_;

  std::unique_ptr<DirectShow> direct_show_;

  bool opened_ = false;
  bool started_ = false;
  StreamOpenResult open_result_ = OPEN_RESULT_OK;


  // Size in bytes of each audio frame (4 bytes for 16-bit stereo PCM)
  size_t frame_size_ = 0;

  // Size in audio frames of each audio packet where an audio packet
  // is defined as the block of data which the user received in each
  // OnData() callback.
  size_t packet_size_frames_ = 0;

  // Size in bytes of each audio packet.
  size_t packet_size_bytes_ = 0;

  // Length of the audio endpoint buffer.
  uint32_t endpoint_buffer_size_frames_ = 0;

  // Contains the unique name of the selected endpoint device.
  // Note that AudioDeviceDescription::kDefaultDeviceId represents the default
  // device role and is not a valid ID as such.
  std::string device_id_;
  bool is_loopback_device_;

  std::string friendly_name_;

  // Pointer to the object that will receive the recorded audio samples.
  AudioInputCallback* sink_ = nullptr;

  // Used for the captured audio on the callback thread.
  std::unique_ptr<AudioBlockFifo> fifo_;

  std::unique_ptr<WAVEFORMATEXTENSIBLE> capture_format_;

  // If the caller requires resampling (should only be in exceptional cases and
  // ideally, never), we support using an AudioConverter.
  std::unique_ptr<AudioConverter> converter_;
  std::unique_ptr<AudioBus> convert_bus_;
  bool imperfect_buffer_size_conversion_ = false;

  const AudioParameters params_;

  base::TimeTicks first_ref_time_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(DirectSoundAudioInputStream);
};

}  // namespace media

#endif  // MEDIA_AUDIO_WIN_AUDIO_DIRECTSOUND_INPUT_WIN_H_
