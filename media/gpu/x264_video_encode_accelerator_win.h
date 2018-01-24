// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef X264_VIDEO_ENCODE_ACCELERATOR_WIN_H_
#define X264_VIDEO_ENCODE_ACCELERATOR_WIN_H_

#include <mfapi.h>
#include <mfidl.h>
#include <stdint.h>
#include <strmif.h>

#include <memory>

#include "base/atomic_ref_count.h"
#include "base/bind.h"
#include "base/containers/circular_deque.h"
#include "base/memory/weak_ptr.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/win/scoped_comptr.h"
#include "media/video/video_encode_accelerator.h"

// ffmpeg binaries need to be built separately
extern "C" {
#include "libavcodec/avcodec.h"
}
namespace media {

// Video encoder interface.
class X264VideoEncodeAccelerator : public VideoEncodeAccelerator {
 public:
  X264VideoEncodeAccelerator();

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

  // FIXME: Do we need to do this?
  // virtual bool TryToSetupEncodeOnSeparateThread(
  //    const base::WeakPtr<Client>& encode_client,
  //    const scoped_refptr<base::SingleThreadTaskRunner>& encode_task_runner);

 protected:
  // Do not delete directly; use Destroy() or own it with a scoped_ptr, which
  // will Destroy() it properly by default.
  ~X264VideoEncodeAccelerator() override;

 private:
  int width_;
  int height_;
  std::string implementation_name_;
  AVCodec *codec_;
  AVCodecContext *avc_context_;
  AVDictionary *fmt_dict_opts;
  BitstreamBuffer output_buffer_;
  Client* client_;

  AVPacket* X264VideoEncodeAccelerator::VideoFrameToAVPacket(
    const scoped_refptr<VideoFrame>& frame);
  inline int X264VideoEncodeAccelerator::GoogleVideoFrameFormatToAVFormat(int frame_format);
};

}  // namespace media

#endif  // X264_VIDEO_ENCODE_ACCELERATOR_WIN_H_
