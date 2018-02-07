// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/ffmpeg_base_video_encode_accelerator_win.h"

#include <iterator>
#include <vector>
#include <mutex>

#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/win/registry.h"
#include "third_party/libyuv/include/libyuv.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
}

using base::win::RegKey;

#define BVLOG VLOG

namespace media {

namespace {

std::mutex logger_cb_mutex;

}  // namespace

class FFMpegBaseVideoEncodeAccelerator::EncoderQueueItem {
  public:
    EncoderQueueItem(base::TimeDelta timestamp) : timestamp(timestamp), packet(nullptr), offset(0) {};
    base::TimeDelta timestamp;
    std::unique_ptr<AVPacket> packet;
    size_t offset;
};

void av_log_callback(void* avcl, int level, const char* fmt, va_list vl) {
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
    LOG(INFO) << "ffmpeg " << buff;
    /* DVLOG(1) << "ffmpeg " << buff; */
  }
}

class FFMpegBaseVideoEncodeAccelerator::EncodeOutput {
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

struct FFMpegBaseVideoEncodeAccelerator::BitstreamBufferRef {
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

FFMpegBaseVideoEncodeAccelerator::FFMpegBaseVideoEncodeAccelerator(std::string ffmpeg_encoder_name, AVPixelFormat native_format)
    : frame_rate_(0),
      target_bitrate_(0),
      ffmpeg_encoder_name_(ffmpeg_encoder_name),
      native_format_(native_format),
      main_client_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      encoder_thread_("ffmpeg - " + ffmpeg_encoder_name),
      encoder_task_weak_factory_(this){
}

FFMpegBaseVideoEncodeAccelerator::~FFMpegBaseVideoEncodeAccelerator() {}

VideoEncodeAccelerator::SupportedProfiles
FFMpegBaseVideoEncodeAccelerator::GetSupportedProfiles() {
  LOG(INFO) << implementation_name_ << " Getting supported profiles.";
  VideoEncodeAccelerator::SupportedProfiles profiles;
  VideoEncodeAccelerator::SupportedProfile profile;

  profile.profile = H264PROFILE_BASELINE;
  profile.max_framerate_numerator = kMaxFrameRateNumerator;
  profile.max_framerate_denominator = kMaxFrameRateDenominator;
  profile.max_resolution.SetSize(1920, 1088);
  profiles.push_back(profile);

  profile.profile = H264PROFILE_MAIN;
  profiles.push_back(profile);

  profile.profile = H264PROFILE_HIGH;
  profiles.push_back(profile);

  return profiles;
}


bool FFMpegBaseVideoEncodeAccelerator::Initialize(
    VideoPixelFormat format,
    const gfx::Size& input_visible_size,
    VideoCodecProfile output_profile,
    uint32_t initial_bitrate,
    Client* client) {

  LOG(INFO) << __func__ << ": input_format=" << VideoPixelFormatToString(format)
           << ", input_visible_size=" << input_visible_size.ToString()
           << ", output_profile=" << output_profile
           << ", initial_bitrate=" << initial_bitrate;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());

  if (PIXEL_FORMAT_I420 != format) {
    DLOG(ERROR) << "Input format not supported= "
                << VideoPixelFormatToString(format);
    return false;
  }

  if (initial_bitrate == 300000) {
    initial_bitrate = kDefaultTargetBitrate;
  }

  // FIXME
  /* if (GetH264VProfile(output_profile) == eAVEncH264VProfile_unknown) { */
  /*   DLOG(ERROR) << "Output profile not supported= " << output_profile; */
  /*   return false; */
  /* } */

  if (!encoder_thread_.Start()) {
    DLOG(ERROR) << "Failed spawning encoder thread.";
    return false;
  }
  encoder_thread_task_runner_ = encoder_thread_.task_runner();

  main_client_weak_factory_.reset(new base::WeakPtrFactory<Client>(client));
  main_client_ = main_client_weak_factory_->GetWeakPtr();
  input_visible_size_ = input_visible_size;

  bitstream_buffer_size_ = input_visible_size.GetArea();

  if (native_format_ == AV_PIX_FMT_YUV420P) {

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

  } else if (native_format_ == AV_PIX_FMT_NV12) {

    u_plane_offset_ = VideoFrame::PlaneSize(PIXEL_FORMAT_NV12,
                                            VideoFrame::kYPlane,
                                            input_visible_size_).GetArea();
    y_stride_ = VideoFrame::RowBytes(VideoFrame::kYPlane, PIXEL_FORMAT_NV12,
                                     input_visible_size_.width());
    u_stride_ = VideoFrame::RowBytes(VideoFrame::kUPlane, PIXEL_FORMAT_NV12,
                                     input_visible_size_.width());
  } else {
    LOG(ERROR) << "ENCODER NATIVE FORMAT NOT IMPLEMENTED: " << native_format_;
  }

  LOG(INFO) << "u_plane_offset: " << u_plane_offset_
    << " y_stride: " << y_stride_
    << " u_stride: " <<  u_stride_;

  // Pin all client callbacks to the main task runner initially. It can be
  // reassigned by TryToSetupEncodeOnSeparateThread().
  if (!encode_client_task_runner_) {
    encode_client_task_runner_ = main_client_task_runner_;
    encode_client_ = main_client_;
  }

  avcodec_register_all();

  av_log_set_callback(av_log_callback);

  codec_ = avcodec_find_encoder_by_name(ffmpeg_encoder_name_.c_str());
  if (codec_ == NULL) {
    LOG(ERROR) << "Failed to find " << ffmpeg_encoder_name_ << " encoder during initialization.";
    return false;
  }

  avc_context_ = avcodec_alloc_context3(codec_);

  ConfigureFromRegistry();

  // https://ffmpeg.org/doxygen/3.2/structAVCodecContext.html
  avc_context_->width = input_visible_size.width();
  avc_context_->height = input_visible_size.height();

  avc_context_->pix_fmt = native_format_;

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
            << ffmpeg_encoder_name_;

  implementation_name_ = "ffmpeg - " + ffmpeg_encoder_name_;
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

void FFMpegBaseVideoEncodeAccelerator::SetFrameRate(uint32_t framerate) {
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

void FFMpegBaseVideoEncodeAccelerator::SetBitRate(uint32_t bitrate) {
  if (target_bitrate_ == bitrate) {
    return;
  }

  avc_context_->bit_rate = bitrate;
  /* avc_context_->rc_min_rate = bitrate / 2; */
  /* avc_context_->rc_max_rate = bitrate; */
  /* avc_context_->rc_min_rate = bitrate * 0.8; */

  LOG(INFO) << "changed bitrate: " << target_bitrate_ << " -> " << bitrate;
  target_bitrate_ = bitrate;
}

void FFMpegBaseVideoEncodeAccelerator::Encode(
    const scoped_refptr<VideoFrame>& frame,
    bool force_keyframe) {
  DVLOG(3) << __func__;
  DCHECK(encode_client_task_runner_->BelongsToCurrentThread());

  encoder_thread_task_runner_->PostTask(
      FROM_HERE, base::Bind(&FFMpegBaseVideoEncodeAccelerator::EncodeTask,
                            encoder_task_weak_factory_.GetWeakPtr(), frame,
                            force_keyframe));
}

void FFMpegBaseVideoEncodeAccelerator::DrainEncoder() {

  ReceiveAllPackets();
  SendAllPackets();
  while(ReceiveAllPackets()) {
    SendAllPackets();
  }
}
bool FFMpegBaseVideoEncodeAccelerator::ReceiveAllPackets() {
  bool got_packet = false;
  while(ReceivePacket()) {
    got_packet = true;
  }
  return got_packet;
}

bool FFMpegBaseVideoEncodeAccelerator::ReceivePacket() {

  std::unique_ptr<AVPacket> receive_packet = base::MakeUnique<AVPacket>();
  av_init_packet(receive_packet.get());

  int err = avcodec_receive_packet(avc_context_, receive_packet.get());
  if (err == AVERROR(EAGAIN)) {
    return false;
  } else if (err < 0) {
    LOG(ERROR) << "avcodec_receive_packet failed with status: " << err;
    return false;
  }
  auto it = encoder_queue_.find(receive_packet->pts);
  if (it == encoder_queue_.end()) {
    LOG(ERROR) << "dropping unknown packet from encoder: " << receive_packet->pts;
    av_packet_unref(receive_packet.get());
    return false;
  }
  it->second->packet = std::move(receive_packet);

  encoder_output_queue_.push_back(std::move(it->second));
  encoder_queue_.erase(it);

  return true;
}


bool FFMpegBaseVideoEncodeAccelerator::SendAllPackets() {
  bool got_packet = false;
  while(SendPacket()) {
    got_packet = true;
  }
  return got_packet;
}

bool FFMpegBaseVideoEncodeAccelerator::SendPacket() {

  if (bitstream_buffer_queue_.empty()) {
    return false;
  }

  if (encoder_output_queue_.empty()) {
    return false;
  }

  std::unique_ptr<EncoderQueueItem>
    queue_item = std::move(encoder_output_queue_.front());

  /* std::unique_ptr<AVPacket> receive_packet = std::move(it->second.packet) */

  bool is_keyframe = queue_item->packet->flags & AV_PKT_FLAG_KEY;
  base::TimeDelta timestamp = queue_item->timestamp;

  std::unique_ptr<FFMpegBaseVideoEncodeAccelerator::BitstreamBufferRef> buffer_ref =
      std::move(bitstream_buffer_queue_.front());
  bitstream_buffer_queue_.pop_front();

  size_t size = queue_item->packet->size - queue_item->offset;
  if (buffer_ref->size < size) {
    size = buffer_ref->size;
  }

  memcpy(buffer_ref->shm->memory(), (void*)queue_item->packet->data, size);
  BVLOG(3) << "avcodec_receive_packet size: " << size
           << " keyframe: " << is_keyframe
           << " pts: " << queue_item->packet->pts
           << " timestamp: " << timestamp;
  encode_client_task_runner_->PostTask(
      FROM_HERE, 
      base::Bind(&Client::BitstreamBufferReady, encode_client_,
                 buffer_ref->id, size, is_keyframe, timestamp));

  size_t packet_size = queue_item->packet->size;
  if (queue_item->offset + size < packet_size) {
    queue_item->offset += size;
    encoder_output_queue_.front() = std::move(queue_item);
  } else {
    av_packet_unref(queue_item->packet.get());
    encoder_output_queue_.pop_front();
  }
  return true;
}

// Send a bitstream buffer to the encoder to be used for storing future
// encoded output.  Each call here with a given |buffer| will cause the buffer
// to be filled once, then returned with BitstreamBufferReady().
// Parameters:
//  |buffer| is the bitstream buffer to use for output.
void FFMpegBaseVideoEncodeAccelerator::UseOutputBitstreamBuffer(
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
      base::Bind(&FFMpegBaseVideoEncodeAccelerator::UseOutputBitstreamBufferTask,
                 encoder_task_weak_factory_.GetWeakPtr(),
                 base::Passed(&buffer_ref)));
}

// Request a change to the encoding parameters.  This is only a request,
// fulfilled on a best-effort basis.
// Parameters:
//  |bitrate| is the requested new bitrate, in bits per second.
//  |framerate| is the requested new framerate, in frames per second.
void FFMpegBaseVideoEncodeAccelerator::RequestEncodingParametersChange(
    uint32_t bitrate,
    uint32_t framerate) {
  DVLOG(3) << __func__ << ": bitrate=" << bitrate
           << ": framerate=" << framerate;
  DCHECK(encode_client_task_runner_->BelongsToCurrentThread());

  encoder_thread_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(
          &FFMpegBaseVideoEncodeAccelerator::RequestEncodingParametersChangeTask,
          encoder_task_weak_factory_.GetWeakPtr(), bitrate, framerate));
}

// Destroys the encoder: all pending inputs and outputs are dropped
// immediately and the component is freed.  This call may asynchronously free
// system resources, but its client-visible effects are synchronous. After
// this method returns no more callbacks will be made on the client. Deletes
// |this| unconditionally, so make sure to drop all pointers to it!
void FFMpegBaseVideoEncodeAccelerator::Destroy() {
  DVLOG(3) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());

  // Cancel all callbacks.
  main_client_weak_factory_.reset();

  if (encoder_thread_.IsRunning()) {
    encoder_thread_task_runner_->PostTask(
        FROM_HERE, base::Bind(&FFMpegBaseVideoEncodeAccelerator::DestroyTask,
                              encoder_task_weak_factory_.GetWeakPtr()));
    encoder_thread_.Stop();
  }

  delete this;
}

bool FFMpegBaseVideoEncodeAccelerator::TryToSetupEncodeOnSeparateThread(
    const base::WeakPtr<Client>& encode_client,
    const scoped_refptr<base::SingleThreadTaskRunner>& encode_task_runner) {
  DVLOG(3) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());
  encode_client_ = encode_client;
  encode_client_task_runner_ = encode_task_runner;
  return true;
}

void FFMpegBaseVideoEncodeAccelerator::NotifyError(
    VideoEncodeAccelerator::Error error) {
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread() ||
         encode_client_task_runner_->BelongsToCurrentThread());
  main_client_task_runner_->PostTask(
      FROM_HERE, base::Bind(&Client::NotifyError, main_client_, error));
}

void FFMpegBaseVideoEncodeAccelerator::EncodeTask(scoped_refptr<VideoFrame> frame,
                                            bool force_keyframe) {
  // https://www.ffmpeg.org/doxygen/2.1/group__lavc__encoding.html
  AVFrame* av_frame = av_frame_alloc();

  // const VideoFrameMetadata* metadata = frame.get()->metadata();
  av_frame->key_frame = (int)force_keyframe;
  av_frame->width = input_visible_size_.width();
  av_frame->height = input_visible_size_.height();

  uint32_t pts = kMaxFrameRateNumerator * (frame->timestamp().InMicroseconds() + 1)
    / base::Time::kMicrosecondsPerSecond;

  if (pts == last_pts_) {
    pts++;
  }
  last_pts_ = pts;
  encoder_queue_[pts] = base::MakeUnique<EncoderQueueItem>(frame->timestamp());
  av_frame->pts = pts;
  BVLOG(3) << __func__
    << " PTS: " << pts  
    << " i: " << pts % kMaxFrameRateNumerator 
    << " microseconds: " << frame->timestamp().InMicroseconds()
    << " frame->timestamp(): " << frame->timestamp();

  av_frame->format = native_format_;

  // TODO: better use an input buffer pool
  // QSV needs 64 byte alignment
  if (av_frame_get_buffer(av_frame, 64) < 0) {
    LOG(ERROR) << "FAILED TO ALLOCATE BUFFER";
    return;
  }

  if (native_format_ == AV_PIX_FMT_YUV420P) {

    libyuv::I420Copy(frame->visible_data(VideoFrame::kYPlane),
                     frame->stride(VideoFrame::kYPlane),
                     frame->visible_data(VideoFrame::kUPlane),
                     frame->stride(VideoFrame::kUPlane),
                     frame->visible_data(VideoFrame::kVPlane),
                     frame->stride(VideoFrame::kVPlane),
                     av_frame->data[0],
                     av_frame->linesize[0],
                     av_frame->data[1],
                     av_frame->linesize[1],
                     av_frame->data[2],
                     av_frame->linesize[2],
                     input_visible_size_.width(),
                     input_visible_size_.height());

  } else if (native_format_ == AV_PIX_FMT_NV12) {

    libyuv::I420ToNV12(frame->visible_data(VideoFrame::kYPlane),
                     frame->stride(VideoFrame::kYPlane),
                     frame->visible_data(VideoFrame::kUPlane),
                     frame->stride(VideoFrame::kUPlane),
                     frame->visible_data(VideoFrame::kVPlane),
                     frame->stride(VideoFrame::kVPlane),
                     av_frame->data[0],
                     av_frame->linesize[0],
                     av_frame->data[1],
                     av_frame->linesize[1],
                     input_visible_size_.width(),
                     input_visible_size_.height());
  } else {
    LOG(ERROR) << "ENCODER NATIVE FORMAT NOT IMPLEMENTED: " << native_format_;
  }

  int err = avcodec_send_frame(avc_context_, av_frame);
  av_frame_free(&av_frame);
  if (err < 0) {
    LOG(ERROR) << "avcodec_send_frame failed with status: " << err;
    return;
  }
  DrainEncoder();
}

void FFMpegBaseVideoEncodeAccelerator::UseOutputBitstreamBufferTask(
    std::unique_ptr<BitstreamBufferRef> buffer_ref) {
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  bitstream_buffer_queue_.push_back(std::move(buffer_ref));
  SendAllPackets();
}

void FFMpegBaseVideoEncodeAccelerator::ReturnBitstreamBuffer(
    std::unique_ptr<EncodeOutput> encode_output,
    std::unique_ptr<FFMpegBaseVideoEncodeAccelerator::BitstreamBufferRef>
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

void FFMpegBaseVideoEncodeAccelerator::RequestEncodingParametersChangeTask(
    uint32_t bitrate,
    uint32_t framerate) {
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

void FFMpegBaseVideoEncodeAccelerator::DestroyTask() {
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  encoder_task_weak_factory_.InvalidateWeakPtrs();
  ReleaseEncoderResources();
}

void FFMpegBaseVideoEncodeAccelerator::FlushEncoder() {
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());
  LOG(INFO) << __func__;

  int err = avcodec_send_frame(avc_context_, nullptr);
  if (err < 0) {
    LOG(ERROR) << "avcodec_send_frame failed with status: " << err;
  }

  while(true) {

    std::unique_ptr<AVPacket> receive_packet = base::MakeUnique<AVPacket>();
    av_init_packet(receive_packet.get());

    int err = avcodec_receive_packet(avc_context_, receive_packet.get());
    if (err == AVERROR_EOF) {
      LOG(INFO) << "avcodec flushed";
      break;
    } else if (err == AVERROR(EAGAIN)) {
      LOG(ERROR) << "avcodec_receive_packet failed with status: EAGAIN";
      break;
    } else if (err < 0) {
      LOG(ERROR) << "avcodec_receive_packet failed with status: " << err;
      break;
    }
    av_packet_unref(receive_packet.get());
  }
  avcodec_flush_buffers(avc_context_);
}

void FFMpegBaseVideoEncodeAccelerator::ReleaseEncoderResources() {
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());
  LOG(INFO) << __func__;
  FlushEncoder();

  for (auto &it: encoder_output_queue_) {
    if (it->packet.get() != nullptr) {
      av_packet_unref(it->packet.get());
    }
  }
  encoder_output_queue_.erase(
      encoder_output_queue_.begin(),
      encoder_output_queue_.end());
  encoder_output_queue_.clear();

  for (auto &it: encoder_queue_) {
    if (it.second->packet.get() != nullptr) {
      av_packet_unref(it.second->packet.get());
    }
  }
  encoder_queue_.erase(
      encoder_queue_.begin(),
      encoder_queue_.end());
  encoder_queue_.clear();

  avcodec_free_context(&avc_context_);
  av_free(codec_);
}

}  // namespace media
