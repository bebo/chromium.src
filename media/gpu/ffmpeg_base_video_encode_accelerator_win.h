// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FFMPEG_BASE_VIDEO_ENCODE_ACCELERATOR_WIN_H_
#define FFMPEG_BASE_VIDEO_ENCODE_ACCELERATOR_WIN_H_

#include <memory>

#include "base/atomic_ref_count.h"
#include "base/bind.h"
#include "base/containers/circular_deque.h"
#include "base/memory/weak_ptr.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "media/video/video_encode_accelerator.h"

extern "C" {
#include "libavcodec/avcodec.h"
}
namespace media {

const size_t kMaxFrameRateNumerator = 60;
const int32_t kDefaultTargetBitrate = 6000000;
const size_t kMaxFrameRateDenominator = 1;
const size_t kNumInputBuffers = 6;
const size_t kMaxKeyFrameInterval = 15;  // seconds

class EncoderQueueItem {
  public:
    EncoderQueueItem(base::TimeDelta timestamp) : packet(nullptr), timestamp(timestamp), offset(0) {};
    base::TimeDelta timestamp; uint64_t offset;
    std::unique_ptr<AVPacket> packet;
};

// Video encoder interface.
class FFMpegBaseVideoEncodeAccelerator : public VideoEncodeAccelerator {
 public:
  FFMpegBaseVideoEncodeAccelerator();
  FFMpegBaseVideoEncodeAccelerator(std::string ffmpeg_encoder_name);

  // Video encoder functions.

  // Returns a list of the supported codec profiles of the video encoder. This
  // can be called before Initialize().
  VideoEncodeAccelerator::SupportedProfiles GetSupportedProfiles() override;

  // Initializes the video encoder with specific configuration.  Called once per
  // encoder construction.  This call is synchronous and returns true iff
  // initialization is successful.
  // TODO(mcasas): Update to asynchronous, https://crbug.com/744210.
  // Parameters:
  //  |input_format| is the frame format of the input stream (as would be
  //  reported by VideoFrame::format() for frames passed to Encode()).
  //  |input_visible_size| is the resolution of the input stream (as would be
  //  reported by VideoFrame::visible_rect().size() for frames passed to
  //  Encode()).
  //  |output_profile| is the codec profile of the encoded output stream.
  //  |initial_bitrate| is the initial bitrate of the encoded output stream,
  //  in bits per second.
  //  |client| is the client of this video encoder.  The provided pointer must
  //  be valid until Destroy() is called.
  // TODO(sheu): handle resolution changes.  http://crbug.com/249944
  bool Initialize(VideoPixelFormat input_format,
                  const gfx::Size& input_visible_size,
                  VideoCodecProfile output_profile,
                  uint32_t initial_bitrate,
                  Client* client) override;

  // Encodes the given frame.
  // Parameters:
  //  |frame| is the VideoFrame that is to be encoded.
  //  |force_keyframe| forces the encoding of a keyframe for this frame.
  void Encode(const scoped_refptr<VideoFrame>& frame,
              bool force_keyframe) override;

  // Send a bitstream buffer to the encoder to be used for storing future
  // encoded output.  Each call here with a given |buffer| will cause the buffer
  // to be filled once, then returned with BitstreamBufferReady().
  // Parameters:
  //  |buffer| is the bitstream buffer to use for output.
  void UseOutputBitstreamBuffer(const BitstreamBuffer& buffer) override;

  // Request a change to the encoding parameters.  This is only a request,
  // fulfilled on a best-effort basis.
  // Parameters:
  //  |bitrate| is the requested new bitrate, in bits per second.
  //  |framerate| is the requested new framerate, in frames per second.
  void RequestEncodingParametersChange(uint32_t bitrate,
                                       uint32_t framerate) override;

  // Destroys the encoder: all pending inputs and outputs are dropped
  // immediately and the component is freed.  This call may asynchronously free
  // system resources, but its client-visible effects are synchronous. After
  // this method returns no more callbacks will be made on the client. Deletes
  // |this| unconditionally, so make sure to drop all pointers to it!
  void Destroy() override;

  // Encode tasks include these methods that are used frequently during the
  // session: Encode(), UseOutputBitstreamBuffer(),
  // RequestEncodingParametersChange(), Client::BitstreamBufferReady().
  // If the Client can support running these on a separate thread, it may
  // call this method to try to set up the VEA implementation to do so.
  //
  // If the VEA can support this as well, return true, otherwise return false.
  // If true is returned, the client may submit each of these calls on
  // |encode_task_runner|, and then expect Client::BitstreamBufferReady() to be
  // called on |encode_task_runner| as well; called on |encode_client|, instead
  // of |client| provided to Initialize().
  //
  // One application of this is offloading the GPU main thread. This helps
  // reduce latency and jitter by avoiding the wait.

  bool TryToSetupEncodeOnSeparateThread(
      const base::WeakPtr<Client>& encode_client,
      const scoped_refptr<base::SingleThreadTaskRunner>& encode_task_runner)
      override;


 protected:
  // Do not delete directly; use Destroy() or own it with a scoped_ptr, which
  // will Destroy() it properly by default.
  ~FFMpegBaseVideoEncodeAccelerator() override;

  // Holds output buffers coming from the client ready to be filled.
  struct BitstreamBufferRef;

  // Holds output buffers coming from the encoder.
  class EncodeOutput;

  // Encoding tasks to be run on |encoder_thread_|.
  void EncodeTask(scoped_refptr<VideoFrame> frame, bool force_keyframe);

  // Helper function to notify the client of an error on
  // |main_client_task_runner_|.
  void NotifyError(VideoEncodeAccelerator::Error error);

  // Inserts the output buffers for reuse on |encoder_thread_|.
  void UseOutputBitstreamBufferTask(
      std::unique_ptr<BitstreamBufferRef> buffer_ref);

  // Copies EncodeOutput into a BitstreamBuffer and returns it to the
  // |encode_client_|.
  void ReturnBitstreamBuffer(
      std::unique_ptr<EncodeOutput> encode_output,
      std::unique_ptr<FFMpegBaseVideoEncodeAccelerator::BitstreamBufferRef>
          buffer_ref);

  // Changes encode parameters on |encoder_thread_|.
  void RequestEncodingParametersChangeTask(uint32_t bitrate,
                                           uint32_t framerate);

  // Destroys encode session on |encoder_thread_|.
  void DestroyTask();

  // Releases resources encoder holds.
  void ReleaseEncoderResources();

  // Bitstream buffers ready to be used to return encoded output as a FIFO.
  base::circular_deque<std::unique_ptr<BitstreamBufferRef>>
      bitstream_buffer_queue_;

  // EncodeOutput needs to be copied into a BitstreamBufferRef as a FIFO.
  base::circular_deque<std::unique_ptr<EncoderQueueItem>> encoder_output_queue_;

  /* uint64_t pts_timestamps_[kMaxFrameRateNumerator] = {0}; */
  std::map<uint32_t, std::unique_ptr<EncoderQueueItem>> encoder_queue_;
  uint32_t last_pts_ = {0};

  void DrainEncoder();
  bool ReceivePacket();
  bool ReceiveAllPackets();
  bool SendPacket();
  bool SendAllPackets();
  virtual void ConfigureFromRegistry() = 0;
  void SetFrameRate(uint32_t framerate);
  void SetBitRate(uint32_t bitrate);

  gfx::Size input_visible_size_;
  size_t bitstream_buffer_size_;
  uint32_t frame_rate_;
  uint32_t target_bitrate_;
  size_t u_plane_offset_;
  size_t v_plane_offset_;
  size_t y_stride_;
  size_t u_stride_;
  size_t v_stride_;
  std::string ffmpeg_encoder_name_;
  std::string implementation_name_;
  AVCodec *codec_;
  AVCodecContext *avc_context_;

  AVPacket* FFMpegBaseVideoEncodeAccelerator::VideoFrameToAVPacket(
    const scoped_refptr<VideoFrame>& frame);

  // To expose client callbacks from VideoEncodeAccelerator.
  // NOTE: all calls to this object *MUST* be executed on
  // |main_client_task_runner_|.
  base::WeakPtr<Client> main_client_;
  std::unique_ptr<base::WeakPtrFactory<Client>> main_client_weak_factory_;
  scoped_refptr<base::SingleThreadTaskRunner> main_client_task_runner_;

  // Used to run client callback BitstreamBufferReady() on
  // |encode_client_task_runner_| if given by
  // TryToSetupEncodeOnSeparateThread().
  base::WeakPtr<Client> encode_client_;
  scoped_refptr<base::SingleThreadTaskRunner> encode_client_task_runner_;

  // This thread services tasks posted from the VEA API entry points by the
  // GPU child thread and CompressionCallback() posted from device thread.
  base::Thread encoder_thread_;
  scoped_refptr<base::SingleThreadTaskRunner> encoder_thread_task_runner_;

  // Declared last to ensure that all weak pointers are invalidated before
  // other destructors run.
  base::WeakPtrFactory<FFMpegBaseVideoEncodeAccelerator>
      encoder_task_weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(FFMpegBaseVideoEncodeAccelerator);
};

}  // namespace media

#endif  // FFMPEG_BASE_VIDEO_ENCODE_ACCELERATOR_WIN_H_
