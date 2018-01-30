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

#include <thread>
#include <mutex>

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
#include "base/time/time.h"
#include "base/win/scoped_comptr.h"
#include "media/video/video_encode_accelerator.h"

extern "C" {

#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#include "libavutil/rational.h"
}

using base::win::RegKey;

#define BVLOG VLOG

namespace media {

namespace {

const int32_t kDefaultTargetBitrate = 6000000;
const size_t kMaxFrameRateNumerator = 60;
const size_t kMaxFrameRateDenominator = 1;
const size_t kNumInputBuffers = 6;
const size_t kMaxKeyFrameInterval = 15;  // seconds

std::mutex logger_cb_mutex ;


}  // namespace

class X264VideoEncodeAccelerator::EncodeOutput {
 public:
  EncodeOutput(uint32_t size, bool key_frame, base::TimeDelta timestamp)
      : keyframe(key_frame), capture_timestamp(timestamp), data_(size) {}

  uint8_t* memory() { return data_.data(); }

  int size() const { return static_cast<int>(data_.size()); }

  const bool keyframe;
  const base::TimeDelta capture_timestamp;

 private:
  std::vector<uint8_t> data_;

  DISALLOW_COPY_AND_ASSIGN(EncodeOutput);
};

struct X264VideoEncodeAccelerator::BitstreamBufferRef {
  BitstreamBufferRef(int32_t id,
                     std::unique_ptr<base::SharedMemory> shm,
                     size_t size)
      : id(id), shm(std::move(shm)), size(size) {}
  const int32_t id;
  const std::unique_ptr<base::SharedMemory> shm;
  const size_t size;

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(BitstreamBufferRef);
};

X264VideoEncodeAccelerator::X264VideoEncodeAccelerator()
    : main_client_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      encoder_thread_("FFMPEGx264EncoderThread"),
      encoder_task_weak_factory_(this),
      target_bitrate_(0),
      frame_rate_(0) {
  LOG(INFO) << "X264 Constructor. " << __func__;
}

X264VideoEncodeAccelerator::~X264VideoEncodeAccelerator() {}

VideoEncodeAccelerator::SupportedProfiles
X264VideoEncodeAccelerator::GetSupportedProfiles() {
  LOG(INFO) << "X264 Getting supported profiles.";
  VideoEncodeAccelerator::SupportedProfiles profiles;
  VideoEncodeAccelerator::SupportedProfile profile;

  profile.profile = H264PROFILE_BASELINE;
  profile.max_framerate_numerator = 60;
  profile.max_framerate_denominator = 1;
  profile.max_resolution.SetSize(1920, 1088);
  profiles.push_back(profile);
  LOG(INFO) << "Returning supported profiles.";
  return profiles;
}

void X264VideoEncodeAccelerator::ConfigureFromRegistry() {
  RegKey beboKey(HKEY_CURRENT_USER, L"SOFTWARE\\Bebo\\App", KEY_READ);

  std::string preset = "veryfast";
  std::string tune = "zerolatency";
  std::string x264_params = "";
  // std::string x264_params = "vbv-maxrate=7000:vbv-bufsize=14000:rc-lookahead=3:sync-lookahead=-1:sliced-threads=0:threads=16"; // VBV
  // std::string x264_params = "b-frames=3:ref=1:nal-hrd=cbr:force-cfr=1";  // CBR
  DWORD max_b_frames = 0;
  DWORD rc_buffer_size = 12000000;
  DWORD crf = 0;
  DWORD cqp = 0;
  DWORD global_quality = 0;
  DWORD twopass = 0;

  if (beboKey.Valid()) {
    if (beboKey.HasValue(L"preset")) {
      std::wstring value;
      beboKey.ReadValue(L"preset", &value);
      preset = base::WideToUTF8(value);
    }

    if (beboKey.HasValue(L"max_b_frames")) {
      beboKey.ReadValueDW(L"max_b_frames", &max_b_frames);
    }

    if (beboKey.HasValue(L"rc_buffer_size")) {
      beboKey.ReadValueDW(L"rc_buffer_size", &rc_buffer_size);
    }

    if (beboKey.HasValue(L"crf")) {
      beboKey.ReadValueDW(L"crf", &crf);
    }

    if (beboKey.HasValue(L"cqp")) {
       beboKey.ReadValueDW(L"cqp", &cqp);
    }

    if (beboKey.HasValue(L"x264-params")) {
      std::wstring value;
      beboKey.ReadValue(L"x264-params", &value);
      x264_params = base::WideToUTF8(value);
    }

    if (beboKey.HasValue(L"global_quality")) {
      beboKey.ReadValueDW(L"global_quality", &global_quality);
    }

    if (beboKey.HasValue(L"twopass")) {
      beboKey.ReadValueDW(L"twopass", &twopass);
    }

    if (beboKey.HasValue(L"tune")) {
      std::wstring value;
      beboKey.ReadValue(L"tune", &value);
      tune = base::WideToUTF8(value);
    }
  }

  if (codec_->id == AV_CODEC_ID_H264) {
    av_opt_set(avc_context_->priv_data, "preset", preset.c_str(), 0);
    LOG(INFO) << "preset: " << preset;
  }

  if (tune.length() > 0) {
    av_opt_set(avc_context_->priv_data, "tune", tune.c_str(), 0);
    LOG(INFO) << "tune: " << tune;
  }

  avc_context_->max_b_frames = max_b_frames;
  LOG(INFO) << "max_b_frames: " << avc_context_->max_b_frames;

  if (crf > 0) {
    av_opt_set_int(avc_context_->priv_data, "crf", crf, 0);
    LOG(INFO) << "crf: " << crf;
  } else if (cqp > 0) {
    av_opt_set_int(avc_context_->priv_data, "cqp", crf, 0);
    LOG(INFO) << "cqp: " << crf;
  } else {
    /* av_opt_set_int(avc_context_->priv_data, "cbr", true, 0); */
    LOG(INFO) << "cbr: true (implicit)";
    /* LOG(INFO) << "nal-hrd: cbr"; */
  }

  if (rc_buffer_size > 0) {
    avc_context_->rc_buffer_size = rc_buffer_size;
    /* avc_context_->rc_initial_buffer_occupancy  = rc_buffer_size / 2; */
    LOG(INFO) << "rc_buffer_size: " << rc_buffer_size;
  }

  if (x264_params.length() > 3) {
    av_opt_set(avc_context_->priv_data, "x264-params", x264_params.c_str(), 0);
    LOG(INFO) << "x264-params: " << x264_params.c_str();
  }

  if (global_quality > 0) {
    avc_context_->global_quality = global_quality;
    LOG(INFO) << "global_quality: " << global_quality;
  }

  if (twopass > 0) {
    av_opt_set_int(avc_context_->priv_data, "2pass", twopass, 0);
    LOG(INFO) << "2pass: " << twopass;
  }

  if (x264_params.length() > 3)  {
    av_opt_set(avc_context_->priv_data,
               "x264-params", x264_params.c_str(), 0);
    LOG(INFO) << "x264-params: " << x264_params.c_str();
  }
}


void av_log_callback(void *avcl, int level, const char *fmt, va_list vl) {

  std::lock_guard<std::mutex> lock(logger_cb_mutex);

  char buff[4096];
  vsnprintf(buff, sizeof(buff), fmt, vl);
  if (level == AV_LOG_ERROR) {
    LOG(ERROR) << "ffmpeg " << buff;
  } else if (level == AV_LOG_WARNING) {
    LOG(WARNING) << "ffmpeg " << buff;
  } else if (level == AV_LOG_INFO) {
    LOG(INFO) << "ffmpeg " << buff;
  } else {
    DVLOG(1) << "ffmpeg " << buff;
  }
}

bool X264VideoEncodeAccelerator::Initialize(VideoPixelFormat format,
                                            const gfx::Size& input_visible_size,
                                            VideoCodecProfile output_profile,
                                            uint32_t initial_bitrate,
                                            Client* client) {
  LOG(INFO) << "Initializing X264 encoder " << __func__;
  avcodec_register_all();
  LOG(INFO) << "X264 Enumerating Codecs.";
  AVCodec* current_codec = NULL;
  current_codec = av_codec_next(current_codec);
  while (current_codec != NULL) {
    if (av_codec_is_encoder(current_codec)) {
      LOG(INFO) << "Found Encoder: " << current_codec->name;
    } else {
      LOG(INFO) << "Not an encoder: " << current_codec->name;
    }
    current_codec = av_codec_next(current_codec);
  }
  LOG(INFO) << "Finished Enumerating Codecs.";

  if (initial_bitrate == 300000) {
    initial_bitrate = kDefaultTargetBitrate;
  }

  if (PIXEL_FORMAT_I420 != format) {
    LOG(ERROR) << "Input format not supported= "
               << VideoPixelFormatToString(format);
    return false;
  }

  encoder_thread_.init_com_with_mta(false);  // TODO: this seems odd
  if (!encoder_thread_.Start()) {
    DLOG(ERROR) << "Failed spawning encoder thread.";
    return false;
  }
  encoder_thread_task_runner_ = encoder_thread_.task_runner();

  main_client_weak_factory_.reset(new base::WeakPtrFactory<Client>(client));
  main_client_ = main_client_weak_factory_->GetWeakPtr();

  input_visible_size_ = input_visible_size;

  bitstream_buffer_size_ = input_visible_size.GetArea();

  u_plane_offset_ =
      VideoFrame::PlaneSize(PIXEL_FORMAT_I420, VideoFrame::kYPlane,
                            input_visible_size_)
          .GetArea();
  v_plane_offset_ = u_plane_offset_ + VideoFrame::PlaneSize(PIXEL_FORMAT_I420,
                                                            VideoFrame::kUPlane,
                                                            input_visible_size_)
                                          .GetArea();
  y_stride_ = VideoFrame::RowBytes(VideoFrame::kYPlane, PIXEL_FORMAT_I420,
                                   input_visible_size_.width());
  u_stride_ = VideoFrame::RowBytes(VideoFrame::kUPlane, PIXEL_FORMAT_I420,
                                   input_visible_size_.width());
  v_stride_ = VideoFrame::RowBytes(VideoFrame::kVPlane, PIXEL_FORMAT_I420,
                                   input_visible_size_.width());

  // Pin all client callbacks to the main task runner initially. It can be
  // reassigned by TryToSetupEncodeOnSeparateThread().
  if (!encode_client_task_runner_) {
    encode_client_task_runner_ = main_client_task_runner_;
    encode_client_ = main_client_;
  }

  avcodec_register_all();

  av_log_set_callback(av_log_callback);

  codec_ = avcodec_find_encoder_by_name("libx264");
  if (codec_ == NULL) {
    LOG(ERROR) << "Failed to find x264 encoder during initialization.";
    return false;
  }

  avc_context_ = avcodec_alloc_context3(codec_);

  ConfigureFromRegistry();

  // https://ffmpeg.org/doxygen/3.2/structAVCodecContext.html
  avc_context_->width = input_visible_size.width();
  avc_context_->height = input_visible_size.height();

  avc_context_->pix_fmt = AV_PIX_FMT_YUV420P;
  avc_context_->framerate.num = kMaxFrameRateNumerator;
  avc_context_->framerate.den = kMaxFrameRateDenominator;
  avc_context_->time_base.num = kMaxFrameRateDenominator;
  avc_context_->time_base.den = kMaxFrameRateNumerator;

  SetBitRate(initial_bitrate);
  SetFrameRate(kMaxFrameRateNumerator / kMaxFrameRateDenominator);

  // Reference for AvFormatContext options :
  // https://ffmpeg.org/doxygen/2.8/movenc_8c_source.html
  // The options seem like they are almost entirely for dumping to a video file.
  int ret = avcodec_open2(avc_context_, codec_, NULL);
  if (ret < 0) {
    LOG(ERROR) << "Could not open codec: " << ret;
    return false;
  };
  LOG(INFO) << "Initialized x264 encoder with codec "
            << "libx264";

  implementation_name_ = "ffmpeg - libx264";
  main_client_task_runner_->PostTask(
      FROM_HERE, base::Bind(&Client::SetImplementationName, main_client_,
                            implementation_name_));
  VLOG(3) << "Posting SetImplementationName: " << implementation_name_;

  main_client_task_runner_->PostTask(
      FROM_HERE, base::Bind(&Client::RequireBitstreamBuffers, main_client_,
                            kNumInputBuffers, input_visible_size_,
                            bitstream_buffer_size_));
  return true;
}

void X264VideoEncodeAccelerator::SetFrameRate(uint32_t framerate) {
  uint32_t old_frame_rate = frame_rate_;
  frame_rate_ =
      framerate
          ? std::min(framerate, static_cast<uint32_t>(kMaxFrameRateNumerator))
          : 1;

  if (old_frame_rate == frame_rate_) {
    return;
  }

  avc_context_->framerate.num = frame_rate_;
  avc_context_->keyint_min = frame_rate_;
  avc_context_->gop_size = frame_rate_ * kMaxKeyFrameInterval;
  LOG(INFO) << "changed framerate: " << old_frame_rate << " -> " << frame_rate_;
}

void X264VideoEncodeAccelerator::SetBitRate(uint32_t bitrate) {
  if (target_bitrate_ == bitrate) {
    return;
  }

  avc_context_->bit_rate = bitrate;
  avc_context_->rc_min_rate = bitrate / 2;
  avc_context_->rc_max_rate = bitrate;

  LOG(INFO) << "changed bitrate: " << target_bitrate_ << " -> " << bitrate;
  target_bitrate_ = bitrate;
}

void X264VideoEncodeAccelerator::Encode(const scoped_refptr<VideoFrame>& frame,
                                        bool force_keyframe) {
  DVLOG(3) << __func__;
  DCHECK(encode_client_task_runner_->BelongsToCurrentThread());

  encoder_thread_task_runner_->PostTask(
      FROM_HERE, base::Bind(&X264VideoEncodeAccelerator::EncodeTask,
                            encoder_task_weak_factory_.GetWeakPtr(), frame,
                            force_keyframe));
}

void X264VideoEncodeAccelerator::DrainEncoder() {
  while (!bitstream_buffer_queue_.empty()) {
    AVPacket receive_packet = {0};
    av_init_packet(&receive_packet);

    int err = avcodec_receive_packet(avc_context_, &receive_packet);
    if (err == AVERROR(EAGAIN)) {
      break;
    } else if (err < 0) {
      LOG(ERROR) << "avcodec_receive_packet failed with status: " << err;
      return;
    }

    bool is_keyframe = receive_packet.flags & AV_PKT_FLAG_KEY;

    std::unique_ptr<X264VideoEncodeAccelerator::BitstreamBufferRef> buffer_ref =
        std::move(bitstream_buffer_queue_.front());
    bitstream_buffer_queue_.pop_front();

    if (buffer_ref->size < receive_packet.size) {
      memcpy(buffer_ref->shm->memory(), (void*)receive_packet.data,
             buffer_ref->size);
      LOG(INFO) << "avcodec_receive_packet size: " << buffer_ref->size
                << " keyframe: " << is_keyframe
                << " pts: " << receive_packet.pts;
      encode_client_task_runner_->PostTask(
          FROM_HERE,
          base::Bind(&Client::BitstreamBufferReady, encode_client_,
                     buffer_ref->id, buffer_ref->size, is_keyframe,
                     base::TimeDelta::FromMilliseconds(receive_packet.pts)));
      int32_t copied = buffer_ref->size;

      while (copied < receive_packet.size) {
        // FIXME - need to have an extra output buffer queue for this ...
        if (bitstream_buffer_queue_.empty()) {
          LOG(ERROR) << "NO BUFFER FOR NEXT FRAME";
          av_packet_unref(&receive_packet);
          return;
        }

        buffer_ref = std::move(bitstream_buffer_queue_.front());
        bitstream_buffer_queue_.pop_front();

        uint32_t copy_size = receive_packet.size - copied;
        if (copy_size > buffer_ref->size) {
          copy_size = buffer_ref->size;
        }

        memcpy(buffer_ref->shm->memory(), (void*)(receive_packet.data + copied),
               copy_size);
        LOG(INFO) << "avcodec_receive_packet size: " << copy_size
                  << " keyframe: " << is_keyframe
                  << " pts: " << receive_packet.pts;
        encode_client_task_runner_->PostTask(
            FROM_HERE,
            base::Bind(&Client::BitstreamBufferReady, encode_client_,
                       buffer_ref->id, copy_size, is_keyframe,
                       base::TimeDelta::FromMilliseconds(receive_packet.pts)));
        copied += copy_size;
      }
      av_packet_unref(&receive_packet);
      return;
    }
    memcpy(buffer_ref->shm->memory(), (void*)receive_packet.data,
           receive_packet.size);

    BVLOG(2) << "avcodec_receive_packet size: " << receive_packet.size
             << " keyframe: " << is_keyframe << " pts: " << receive_packet.pts;

    encode_client_task_runner_->PostTask(
        FROM_HERE,
        base::Bind(&Client::BitstreamBufferReady, encode_client_,
                   buffer_ref->id, receive_packet.size, is_keyframe,
                   base::TimeDelta::FromMilliseconds(receive_packet.pts)));
    av_packet_unref(&receive_packet);
  }
}

// Send a bitstream buffer to the encoder to be used for storing future
// encoded output.  Each call here with a given |buffer| will cause the buffer
// to be filled once, then returned with BitstreamBufferReady().
// Parameters:
//  |buffer| is the bitstream buffer to use for output.
void X264VideoEncodeAccelerator::UseOutputBitstreamBuffer(
    const BitstreamBuffer& buffer) {
  DVLOG(3) << __func__ << ": buffer size=" << buffer.size();
  DCHECK(encode_client_task_runner_->BelongsToCurrentThread());

  if (buffer.size() < bitstream_buffer_size_) {
    LOG(ERROR) << "Output BitstreamBuffer isn't big enough: " << buffer.size()
               << " vs. " << bitstream_buffer_size_;
    NotifyError(kInvalidArgumentError);
    return;
  }

  std::unique_ptr<base::SharedMemory> shm(
      new base::SharedMemory(buffer.handle(), false));
  if (!shm->Map(buffer.size())) {
    LOG(ERROR) << "Failed mapping shared memory.";
    NotifyError(kPlatformFailureError);
    return;
  }

  std::unique_ptr<BitstreamBufferRef> buffer_ref(
      new BitstreamBufferRef(buffer.id(), std::move(shm), buffer.size()));
  encoder_thread_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&X264VideoEncodeAccelerator::UseOutputBitstreamBufferTask,
                 encoder_task_weak_factory_.GetWeakPtr(),
                 base::Passed(&buffer_ref)));
}

// Request a change to the encoding parameters.  This is only a request,
// fulfilled on a best-effort basis.
// Parameters:
//  |bitrate| is the requested new bitrate, in bits per second.
//  |framerate| is the requested new framerate, in frames per second.
void X264VideoEncodeAccelerator::RequestEncodingParametersChange(
    uint32_t bitrate,
    uint32_t framerate) {
  DVLOG(3) << __func__ << ": bitrate=" << bitrate
           << ": framerate=" << framerate;
  DCHECK(encode_client_task_runner_->BelongsToCurrentThread());

  encoder_thread_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(
          &X264VideoEncodeAccelerator::RequestEncodingParametersChangeTask,
          encoder_task_weak_factory_.GetWeakPtr(), bitrate, framerate));
}

// Destroys the encoder: all pending inputs and outputs are dropped
// immediately and the component is freed.  This call may asynchronously free
// system resources, but its client-visible effects are synchronous. After
// this method returns no more callbacks will be made on the client. Deletes
// |this| unconditionally, so make sure to drop all pointers to it!
void X264VideoEncodeAccelerator::Destroy() {
  // FIXME: Clean up all of the stuff. Basically nothing is freed right now.
  LOG(INFO) << "X264 Encoder " << __func__;

  // avcodec_free_context(&avc_context_);
  // av_free(codec_);
  DVLOG(3) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());

  // Cancel all callbacks.
  main_client_weak_factory_.reset();

  if (encoder_thread_.IsRunning()) {
    encoder_thread_task_runner_->PostTask(
        FROM_HERE, base::Bind(&X264VideoEncodeAccelerator::DestroyTask,
                              encoder_task_weak_factory_.GetWeakPtr()));
    encoder_thread_.Stop();
  }

  delete this;
}

bool X264VideoEncodeAccelerator::TryToSetupEncodeOnSeparateThread(
    const base::WeakPtr<Client>& encode_client,
    const scoped_refptr<base::SingleThreadTaskRunner>& encode_task_runner) {
  DVLOG(3) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());
  encode_client_ = encode_client;
  encode_client_task_runner_ = encode_task_runner;
  return true;
}

void X264VideoEncodeAccelerator::NotifyError(
    VideoEncodeAccelerator::Error error) {
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread() ||
         encode_client_task_runner_->BelongsToCurrentThread());
  main_client_task_runner_->PostTask(
      FROM_HERE, base::Bind(&Client::NotifyError, main_client_, error));
}

void X264VideoEncodeAccelerator::EncodeTask(scoped_refptr<VideoFrame> frame,
                                            bool force_keyframe) {
  // https://www.ffmpeg.org/doxygen/2.1/group__lavc__encoding.html
  AVFrame* av_frame = av_frame_alloc();

  // const VideoFrameMetadata* metadata = frame.get()->metadata();
  av_frame->key_frame = (int)force_keyframe;
  av_frame->width = input_visible_size_.width();
  av_frame->height = input_visible_size_.height();
  /* av_frame->reordered_opaque = frame->timestamp().InMilliseconds(); */
  av_frame->pts = frame->timestamp().InMilliseconds();

  av_frame->format = AV_PIX_FMT_YUV420P;

  // TODO: better use an input buffer pool
  if (av_frame_get_buffer(av_frame, 32) < 0) {
    LOG(ERROR) << "FAILED TO ALLOCATE BUFFER";
    return;
  }

  libyuv::I420Copy(frame->visible_data(VideoFrame::kYPlane),
                   frame->stride(VideoFrame::kYPlane),
                   frame->visible_data(VideoFrame::kUPlane),
                   frame->stride(VideoFrame::kUPlane),
                   frame->visible_data(VideoFrame::kVPlane),
                   frame->stride(VideoFrame::kVPlane), av_frame->data[0],
                   y_stride_, av_frame->data[1], u_stride_, av_frame->data[2],
                   v_stride_, input_visible_size_.width(),
                   input_visible_size_.height());

  int err = avcodec_send_frame(avc_context_, av_frame);
  av_frame_free(&av_frame);
  if (err < 0) {
    LOG(ERROR) << "avcodec_send_frame failed with status: " << err;
    return;
  }
  DrainEncoder();
}

void X264VideoEncodeAccelerator::UseOutputBitstreamBufferTask(
    std::unique_ptr<BitstreamBufferRef> buffer_ref) {
  BVLOG(3) << __func__;
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  // If there is already EncodeOutput waiting, copy its output first.
  if (!encoder_output_queue_.empty()) {
    std::unique_ptr<X264VideoEncodeAccelerator::EncodeOutput> encode_output =
        std::move(encoder_output_queue_.front());
    encoder_output_queue_.pop_front();
    ReturnBitstreamBuffer(std::move(encode_output), std::move(buffer_ref));
    return;
  }

  bitstream_buffer_queue_.push_back(std::move(buffer_ref));
}

void X264VideoEncodeAccelerator::ReturnBitstreamBuffer(
    std::unique_ptr<EncodeOutput> encode_output,
    std::unique_ptr<X264VideoEncodeAccelerator::BitstreamBufferRef>
        buffer_ref) {
  DVLOG(3) << __func__;
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  memcpy(buffer_ref->shm->memory(), encode_output->memory(),
         encode_output->size());
  encode_client_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&Client::BitstreamBufferReady, encode_client_, buffer_ref->id,
                 encode_output->size(), encode_output->keyframe,
                 encode_output->capture_timestamp));
}

void X264VideoEncodeAccelerator::RequestEncodingParametersChangeTask(
    uint32_t bitrate,
    uint32_t framerate) {
  BVLOG(3) << __func__;
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  if (bitrate == 300000) {
    // TODO figure out where this comes from
    LOG(INFO)
        << "ignoring initial wrong bitrate - CODECAPI_AVEncCommonMeanBitRate: "
        << bitrate;
    return;
  }
  SetFrameRate(framerate);
  SetBitRate(bitrate);
}

void X264VideoEncodeAccelerator::DestroyTask() {
  BVLOG(3) << __func__;
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  encoder_task_weak_factory_.InvalidateWeakPtrs();
  ReleaseEncoderResources();
}

void X264VideoEncodeAccelerator::ReleaseEncoderResources() {
  LOG(INFO) << __func__;

  //  encoder_.Reset();
}

}  // namespace media
