// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_MEDIA_FOUNDATION_VIDEO_ENCODE_ACCELERATOR_WIN_H_
#define MEDIA_GPU_MEDIA_FOUNDATION_VIDEO_ENCODE_ACCELERATOR_WIN_H_

#include <mfapi.h>
#include <mfidl.h>
#include <stdint.h>
#include <strmif.h>
#include <wrl/client.h>

#include <memory>

#include "base/atomic_ref_count.h"
#include "base/bind.h"
#include "base/containers/circular_deque.h"
#include "base/memory/weak_ptr.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "media/gpu/media_gpu_export.h"
#include "media/video/video_encode_accelerator.h"

namespace media {

// Media Foundation implementation of the VideoEncodeAccelerator interface for
// Windows.
// This class saves the task runner on which it is constructed and runs client
// callbacks using that same task runner. If TryToSetupEncodeOnSeparateThread()
// is called, it uses the given |encode_task_runner| instead to return encoded
// data. This class has DCHECKs to makes sure that methods are called in the
// correct task runners. It starts an internal encoder thread on which
// VideoEncodeAccelerator implementation tasks are posted.
class MEDIA_GPU_EXPORT MediaFoundationVideoEncodeAccelerator
    : public VideoEncodeAccelerator, IMFAsyncCallback {
 public:
  // If |compatible_with_win7| is true, MediaFoundationVideoEncoderAccelerator
  // works on Windows 7. Some attributes of the encoder are not supported on old
  // systems, which may impact the performance or quality of the output.
  explicit MediaFoundationVideoEncodeAccelerator(bool compatible_with_win7);

  // VideoEncodeAccelerator implementation.
  VideoEncodeAccelerator::SupportedProfiles GetSupportedProfiles() override;
  bool Initialize(VideoPixelFormat input_format,
                  const gfx::Size& input_visible_size,
                  VideoCodecProfile output_profile,
                  uint32_t initial_bitrate,
                  Client* client) override;
  void Encode(const scoped_refptr<VideoFrame>& frame,
              bool force_keyframe) override;
  void UseOutputBitstreamBuffer(const BitstreamBuffer& buffer) override;
  void RequestEncodingParametersChange(uint32_t bitrate,
                                       uint32_t framerate) override;
  void Destroy() override;
  bool TryToSetupEncodeOnSeparateThread(
      const base::WeakPtr<Client>& encode_client,
      const scoped_refptr<base::SingleThreadTaskRunner>& encode_task_runner)
      override;

  // Preload dlls required for encoding. Returns true if all required dlls are
  // correctly loaded.
  static bool PreSandboxInitialization();

  // IMFAsyncCallback
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP GetParameters(DWORD * pdwFlags, DWORD * pdwQueue);
  STDMETHODIMP Invoke(IMFAsyncResult * pAsyncResult);

 protected:
  ~MediaFoundationVideoEncodeAccelerator() override;

 private:
  // Holds output buffers coming from the client ready to be filled.
  struct BitstreamBufferRef;

  // Holds output buffers coming from the encoder.
  class EncodeOutput;

  // Creates an hardware encoder backed IMFTransform instance on |encoder_|.
  bool CreateHardwareEncoderMFT();

  // Initializes and allocates memory for input and output samples.
  bool InitializeInputOutputSamples(VideoCodecProfile output_profile);

  // Initialize Async Event Generator
  bool InitializeEventGenerator();

  // Initializes encoder parameters for real-time use.
  bool SetEncoderModes();

  // Returns true if we can initialize input and output samples with the given
  // frame size, otherwise false.
  bool IsResolutionSupported(const gfx::Size& size);

  // Helper function to notify the client of an error on
  // |main_client_task_runner_|.
  void NotifyError(VideoEncodeAccelerator::Error error);

  // Encoding tasks to be run on |encoder_thread_|.
  void EncodeTask(scoped_refptr<VideoFrame> frame, bool force_keyframe);

  void QueueFrame(scoped_refptr<VideoFrame> frame, bool force_keyframe);
  bool DrainEvents();
  void ProcessInput();
  bool ProcessEvent(Microsoft::WRL::ComPtr<IMFMediaEvent> event);

  // Checks for and copies encoded output on |encoder_thread_|.
  void ProcessOutput();
  void ProcessInputOutput();

  // Inserts the output buffers for reuse on |encoder_thread_|.
  void UseOutputBitstreamBufferTask(
      std::unique_ptr<BitstreamBufferRef> buffer_ref);

  // Copies EncodeOutput into a BitstreamBuffer and returns it to the
  // |encode_client_|.
  void ReturnBitstreamBuffer(
      std::unique_ptr<EncodeOutput> encode_output,
      std::unique_ptr<MediaFoundationVideoEncodeAccelerator::BitstreamBufferRef>
          buffer_ref);

  // Changes encode parameters on |encoder_thread_|.
  void RequestEncodingParametersChangeTask(uint32_t bitrate,
                                           uint32_t framerate);

  // Destroys encode session on |encoder_thread_|.
  void DestroyTask();

  // Releases resources encoder holds.
  void ReleaseEncoderResources();

  const bool compatible_with_win7_;

  // Bitstream buffers ready to be used to return encoded output as a FIFO.
  base::circular_deque<std::unique_ptr<BitstreamBufferRef>>
      bitstream_buffer_queue_;

  // EncodeOutput needs to be copied into a BitstreamBufferRef as a FIFO.
  base::circular_deque<std::unique_ptr<EncodeOutput>> encoder_output_queue_;

  // Input samples waiting to be processed by the encoder
  std::deque<Microsoft::WRL::ComPtr<IMFSample>> input_sample_queue_;

  gfx::Size input_visible_size_;
  size_t bitstream_buffer_size_;
  uint32_t frame_rate_;
  uint32_t target_bitrate_;
  DWORD AVEncCommonMaxBitRate_;
  size_t u_plane_offset_;
  size_t v_plane_offset_;
  size_t y_stride_;
  size_t u_stride_;
  size_t v_stride_;

  // debug counters
  uint64_t dropped_input_cnt_;
  uint64_t dropped_bitstream_queue_cnt_;

  std::atomic<bool> alive_;
  std::atomic<uint32_t> input_events_;
  std::atomic<uint32_t> output_events_;
  HANDLE drained_;

  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> imf_media_event_generator_;
  Microsoft::WRL::ComPtr<IMFTransform> encoder_;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api_;

  std::string implementation_name_;

  DWORD input_stream_id_;
  DWORD output_stream_id_;

  base::AtomicRefCount ref_count_;

  Microsoft::WRL::ComPtr<IMFMediaType> imf_input_media_type_;
  Microsoft::WRL::ComPtr<IMFMediaType> imf_output_media_type_;

  Microsoft::WRL::ComPtr<IMFSample> GetInputSample();
  Microsoft::WRL::ComPtr<IMFSample> GetOutputSample();

  std::list<Microsoft::WRL::ComPtr<IMFSample>> input_sample_pool_;
  std::list<Microsoft::WRL::ComPtr<IMFSample>> output_sample_pool_;

  bool encoder_provides_samples_;

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
  base::WeakPtrFactory<MediaFoundationVideoEncodeAccelerator>
      encoder_task_weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(MediaFoundationVideoEncodeAccelerator);
};

}  // namespace media

#endif  // MEDIA_GPU_MEDIA_FOUNDATION_VIDEO_ENCODE_ACCELERATOR_WIN_H_
