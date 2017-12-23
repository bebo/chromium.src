// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/win/direct_show_input_win.h"

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/utf_string_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/trace_event/trace_event.h"
#include "media/audio/audio_device_description.h"
#include "media/audio/win/audio_manager_win.h"
#include "media/audio/win/core_audio_util_win.h"
#include "media/base/audio_block_fifo.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_timestamp_helper.h"
#include "media/base/channel_layout.h"
#include "media/base/limits.h"
#include "media/base/timestamp_constants.h"

#include "media/direct_show/direct_show_device_factory.h"
#include "media/direct_show/direct_show.h"



namespace media {


#if 0 // used?
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
#endif


DirectSoundAudioInputStream::DirectSoundAudioInputStream(AudioManagerWin* manager,
                                               const AudioParameters& params,
                                               const std::string& device_id)
    : manager_(manager), device_id_(device_id), params_(params) {
  DCHECK(manager_);
  DCHECK(!device_id_.empty());

  direct_show_.reset(DirectShowDeviceFactory::GetInstance()->GetController(device_id));

  friendly_name_ = "DirectShow";

  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::DirectSoundAudioInputStream "
    << params.AsHumanReadableString();

  frame_size_ = (params.bits_per_sample() / 8 * params.channels());
  packet_size_frames_ = params.GetBytesPerBuffer() / frame_size_;
  packet_size_bytes_ = params.GetBytesPerBuffer();

  LOG(INFO) << friendly_name_ << " Number of audio frames per packet: " << packet_size_frames_;

#if 0
  size_t capture_buffer_size = 
    std::max(2 * endpoint_buffer_size_frames_ * frame_size_,
      2 * packet_size_frames_ * frame_size_);
#endif

  // TODO: capture_buffer_size be more dynamic and do this in formatChanged
  size_t capture_buffer_size = 24 * 24 /*buffer duaration ms*/ * 48 /*sample rate*/ * frame_size_;
  int buffers_required = capture_buffer_size / packet_size_bytes_;
  if (converter_ && imperfect_buffer_size_conversion_)
    ++buffers_required;

  fifo_.reset(new AudioBlockFifo(params.channels(), packet_size_frames_,
                                 buffers_required));
}

DirectSoundAudioInputStream::~DirectSoundAudioInputStream() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

bool DirectSoundAudioInputStream::Open() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK_EQ(OPEN_RESULT_OK, open_result_);

  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::Open()";

  opened_ = true;

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

  StartAgc();

  direct_show_->RegisterObserver(this);

  started_ = true; // SUCCEEDED(hr);
}

void DirectSoundAudioInputStream::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(1) << "DirectSoundAudioInputStream::Stop()";
  LOG(INFO) << friendly_name_ << " DirectSoundAudioInputStream::Stop()";

  if (!started_)
    return;

  StopAgc();

  direct_show_->UnregisterObserver(this);

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
  return 0.0;
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
  return static_cast<double>(0.0f);
}

bool DirectSoundAudioInputStream::IsMuted() {
  DCHECK(opened_) << "Open() has not been called successfully";
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(3) << friendly_name_ << " DirectSoundAudioInputStream::IsMuted()";
  if (!opened_)
    return false;

  return false;
}

void DirectSoundAudioInputStream::AudioFrameReceived(const uint8_t* buffer,
                                          int length,
                                          base::TimeDelta timestamp) {
  static base::TimeDelta first_timestamp_;
  if (first_ref_time_.is_null()) {
    first_ref_time_ = base::TimeTicks::Now();
    first_timestamp_ = timestamp;
  }
  timestamp -= first_timestamp_;

  // There is a chance that the platform does not provide us with the timestamp,
  // in which case, we use reference time to calculate a timestamp.
  if (timestamp == media::kNoTimestamp)
    timestamp = base::TimeTicks::Now() - first_ref_time_;

  // |audio_samples_ready_event_| has been set.
  UINT32 num_frames_to_read = length / capture_format_->Format.nBlockAlign; // (length / nBlockAlign)

  static base::TimeDelta last_called;
  static uint64_t frame_count = 0;

  base::TimeTicks capture_time = first_ref_time_ + timestamp;

  LOG(INFO) << "frame received length: " << length << ", timestamp: " << timestamp << " delta: " << (timestamp - last_called) << ", capture_time: " << capture_time << ", first_ref_time: " << first_ref_time_;

  last_called = timestamp;

  // Adjust |capture_time| for the FIFO before pushing.
  capture_time -= AudioTimestampHelper::FramesToTime(
      fifo_->GetAvailableFrames(), capture_format_->Format.nSamplesPerSec);

  fifo_->Push(buffer, num_frames_to_read, 
      capture_format_->Format.wBitsPerSample / 8);

  // Deliver captured data to the registered consumer using a packet
  // size which was specified at construction.
  float volume = 0.0f;

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
          convert_bus_->frames(), capture_format_->Format.nSamplesPerSec);
    } else {
      sink_->OnData(fifo_->Consume(), capture_time, volume);

      // Move the capture time forward for each vended block.
      capture_time += AudioTimestampHelper::FramesToTime(
          packet_size_frames_, capture_format_->Format.nSamplesPerSec);
    }
  }
}

void DirectSoundAudioInputStream::FormatChanged(WAVEFORMATEXTENSIBLE* format) {
  capture_format_.reset(std::move(format));

  LOG(INFO) << friendly_name_ << " DirectShow::FormatChanged()";

  // Ideally, we want a 1:1 ratio between the buffers we get and the buffers
  // we give to OnData so that each buffer we receive from the OS can be
  // directly converted to a buffer that matches with what was asked for.
  const double buffer_ratio =
    capture_format_->Format.nSamplesPerSec / static_cast<double>(packet_size_frames_);
  double new_frames_per_buffer = capture_format_->Format.nSamplesPerSec / buffer_ratio;

  LOG(INFO) << "buffer_ratio: " << buffer_ratio << ", new_frames_per_buffer: " << new_frames_per_buffer;

  // TODO: check to see if we really need to convert
// if (need convert) {

  const auto input_layout = GuessChannelLayout(capture_format_->Format.nChannels);
  DCHECK_NE(CHANNEL_LAYOUT_UNSUPPORTED, input_layout);
  const auto output_layout = GuessChannelLayout(params_.channels());
  DCHECK_NE(CHANNEL_LAYOUT_UNSUPPORTED, output_layout);

  LOG(INFO) << "Input: nSamplesPerSec: " << capture_format_->Format.nSamplesPerSec << ", wBitsPerSample: " << capture_format_->Format.wBitsPerSample << ", Channels: " << capture_format_->Format.nChannels;
  LOG(INFO) << "Output: nSamplesPerSec: " << params_.sample_rate() << ", wBitsPerSample: " << params_.bits_per_sample() << ", Channels: " << params_.channels();

  const AudioParameters input(AudioParameters::AUDIO_PCM_LOW_LATENCY,
      input_layout,
      capture_format_->Format.nSamplesPerSec,
      capture_format_->Format.wBitsPerSample,
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

  LOG(INFO) << "Will convert audio from: bits: " << capture_format_->Format.wBitsPerSample
    << ", sample rate: " << capture_format_->Format.nSamplesPerSec
    << ", channels: " << capture_format_->Format.nChannels
    << ", block align: " << capture_format_->Format.nBlockAlign
    << ", avg bytes per sec: " << capture_format_->Format.nAvgBytesPerSec;

  // Update our packet size assumptions based on the new format.
  const auto new_bytes_per_buffer =
    static_cast<int>(new_frames_per_buffer) * capture_format_->Format.nBlockAlign;
  packet_size_frames_ = new_bytes_per_buffer / capture_format_->Format.nBlockAlign;
  packet_size_bytes_ = new_bytes_per_buffer;
  frame_size_ = capture_format_->Format.nBlockAlign;

  imperfect_buffer_size_conversion_ =
    std::modf(new_frames_per_buffer, &new_frames_per_buffer) != 0.0;

  if (imperfect_buffer_size_conversion_) {
    LOG(INFO) << "Audio capture data conversion: Need to inject fifo";
  }

  // }

  size_t capture_buffer_size = 24 * 24 /*buffer duaration ms*/ * 48 /*sample rate*/ * frame_size_;
  int buffers_required = capture_buffer_size / packet_size_bytes_;
  if (converter_ && imperfect_buffer_size_conversion_)
    ++buffers_required;

  fifo_.reset(new AudioBlockFifo(capture_format_->Format.nChannels, packet_size_frames_,
                                 buffers_required));
}

double DirectSoundAudioInputStream::ProvideInput(AudioBus* audio_bus,
                                                 uint32_t frames_delayed) {
  fifo_->Consume()->CopyTo(audio_bus);
  return 1.0;
}

}  // namespace media
