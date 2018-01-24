// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/x264_video_encode_accelerator_win.h"

#pragma warning(push)
#pragma warning(disable : 4800)  // Disable warning for added padding.

#include <codecapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <objbase.h>

#include <iterator>
#include <utility>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "base/win/registry.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_variant.h"
#include "base/win/windows_version.h"
#include "media/base/win/mf_helpers.h"
#include "media/base/win/mf_initializer.h"
#include "third_party/libyuv/include/libyuv.h"

#include "base/atomic_ref_count.h"
#include "base/bind.h"
#include "base/containers/circular_deque.h"
#include "base/memory/weak_ptr.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/win/scoped_comptr.h"
#include "media/video/video_encode_accelerator.h"
#include "base/time/time.h"

extern "C" {
#include "libavcodec/avcodec.h"
}

using base::win::RegKey;
using base::win::ScopedComPtr;
using media::mf::MediaBufferScopedPointer;

#define BVLOG VLOG

namespace media {
X264VideoEncodeAccelerator::X264VideoEncodeAccelerator()
    : implementation_name_("X264") {
  fmt_dict_opts = NULL;
  LOG(INFO) << "X264 Constructor.";
}

X264VideoEncodeAccelerator::~X264VideoEncodeAccelerator() {}

VideoEncodeAccelerator::SupportedProfiles
X264VideoEncodeAccelerator::GetSupportedProfiles() {
  LOG(INFO) << "X264 Getting supported profiles.";
  VideoEncodeAccelerator::SupportedProfiles profiles;
  VideoEncodeAccelerator::SupportedProfile profile;

  profile.profile = media::H264PROFILE_MAIN;
  profile.max_framerate_numerator = 60;
  profile.max_framerate_denominator = 1;
  profile.max_resolution.SetSize(1920, 1088);
  profiles.push_back(profile);
  LOG(INFO) << "Returning supported profiles.";
  return profiles;
}

bool X264VideoEncodeAccelerator::Initialize(VideoPixelFormat input_format,
                                            const gfx::Size& input_visible_size,
                                            VideoCodecProfile output_profile,
                                            uint32_t initial_bitrate,
                                            Client* client) {
  LOG(INFO) << "Initializing X264 encoder";
  client_ = client;
  codec_ = avcodec_find_encoder_by_name("libx264");
  if (!codec_) {
    LOG(ERROR) << "Failed to find x264 encoder during initialization.";
    return false;
  }
  // https://ffmpeg.org/doxygen/3.2/structAVCodecContext.html
  avc_context_ = avcodec_alloc_context3(codec_);
  width_ = input_visible_size.width();
  height_ = input_visible_size.height();
  avc_context_->width = width_;
  avc_context_->height = height_;
  avc_context_->bit_rate = initial_bitrate;
  // FIXME: Use actual format.
  avc_context_->pix_fmt = AV_PIX_FMT_YUV420P;
  avc_context_->max_b_frames = 3;
  // FIXME: Figure out how to set initial framerate.
  avc_context_->framerate.num = 60;

  // Reference for AvFormatContext options :
  // https://ffmpeg.org/doxygen/2.8/movenc_8c_source.html
  // The options seem like they are almost entirely for dumping to a video file.
  if (avcodec_open2(avc_context_, codec_, &fmt_dict_opts) < 0) {
    LOG(ERROR) << "Could not open codec";
    return false;
  };
    LOG(INFO) << "Initializing X264 encoder2";

  return true;
}

// Encodes the given frame.
// Parameters:
//  |frame| is the VideoFrame that is to be encoded.
//  |force_keyframe| forces the encoding of a keyframe for this frame.
void X264VideoEncodeAccelerator::Encode(const scoped_refptr<VideoFrame>& frame,
                                        bool force_keyframe) {
  // https://www.ffmpeg.org/doxygen/2.1/group__lavc__encoding.html
  AVFrame* av_frame = av_frame_alloc();

  // const VideoFrameMetadata* metadata = frame.get()->metadata();
  av_frame->key_frame = (int)force_keyframe;
  av_frame->width = width_;
  av_frame->height = height_;
  av_frame->reordered_opaque = frame->timestamp().InMilliseconds();

  // FIXME: format incorrect.
  av_frame->format = GoogleVideoFrameFormatToAVFormat(frame.get()->format());
  // FIXME: I'm not sure that this is the right way to copy the planes over.
  for (uint8_t i = 0; i < VideoFrame::kMaxPlanes; i++) {
    av_frame->linesize[i] = frame.get()->row_bytes(i);
    av_frame->data[i] =
        (uint8_t*)frame.get()->shared_memory_handle().GetHandle();
  }

  int err = avcodec_send_frame(avc_context_, av_frame);
  if (err < 0) {
    LOG(ERROR) << "avcodec_send_frame failed with status: " << err;
    return;
  }
  if (output_buffer_.size() == 0) {
    return;
  }
  AVPacket* receive_packet = NULL;

  // FIXME: Repeat this call until it returns AVERROR(EAGAIN) which means there
  // is no more data available, the chrome output buffer is full, or we get an
  // error.
  err = avcodec_receive_packet(avc_context_, receive_packet);
  if (err < 0) {
    // FIXME: Do we need to cleanup the allocated packet if the method fails.
    LOG(ERROR) << "avcodec_receive_packet failed with status: " << err;
    return;
  }
  // Copy from the packet we received to the output buffer.
  base::SharedMemoryHandle buffer_handle = output_buffer_.handle();
  HANDLE handle = buffer_handle.GetHandle();
  memcpy(handle, (void*)receive_packet->data, receive_packet->size);
  // FIXME: Figure out how to actually call this.  How are we supposed to know
  // whether arbitrary output data sent to us contains a keyframe? Fix the
  // timestamp.
  client_->BitstreamBufferReady(output_buffer_.id(), receive_packet->size,
                               force_keyframe, base::TimeDelta::FromMilliseconds(receive_packet->pts));
  av_packet_unref(receive_packet);
}

int X264VideoEncodeAccelerator::GoogleVideoFrameFormatToAVFormat(
    int frame_format) {
  // FIXME: todo.
  return AV_PIX_FMT_YUV420P;
}

// Send a bitstream buffer to the encoder to be used for storing future
// encoded output.  Each call here with a given |buffer| will cause the buffer
// to be filled once, then returned with BitstreamBufferReady().
// Parameters:
//  |buffer| is the bitstream buffer to use for output.
void X264VideoEncodeAccelerator::UseOutputBitstreamBuffer(
    const BitstreamBuffer& buffer) {
  // FIXME
  output_buffer_ = buffer;
  LOG(INFO) << "Received a bitstream buffer to hold encoded data.";
}

// Request a change to the encoding parameters.  This is only a request,
// fulfilled on a best-effort basis.
// Parameters:
//  |bitrate| is the requested new bitrate, in bits per second.
//  |framerate| is the requested new framerate, in frames per second.
void X264VideoEncodeAccelerator::RequestEncodingParametersChange(
    uint32_t bitrate,
    uint32_t framerate) {
  // FIXME: I couldn't quickly find a clear answer an how to dynamically change
  // the bitrate, especially after the somewhat recent libavcodec changes.
  avc_context_->bit_rate = bitrate;
  avc_context_->framerate.num = framerate;
}

// Destroys the encoder: all pending inputs and outputs are dropped
// immediately and the component is freed.  This call may asynchronously free
// system resources, but its client-visible effects are synchronous. After
// this method returns no more callbacks will be made on the client. Deletes
// |this| unconditionally, so make sure to drop all pointers to it!
void X264VideoEncodeAccelerator::Destroy() {
  // FIXME: Clean up all of the stuff. Basically nothing is freed right now.
  avcodec_free_context(&avc_context_);
  av_free(codec_);
}
}  // namespace media
