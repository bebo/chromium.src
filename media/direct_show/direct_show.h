
#ifndef MEDIA_DIRECT_SHOW_DIRECT_SHOW_H_
#define MEDIA_DIRECT_SHOW_DIRECT_SHOW_H_

#include <memory>

#include <stddef.h>
#include <stdint.h>

#include <dshow.h>
#include <mmreg.h>

#include "base/atomic_ref_count.h"
#include "base/compiler_specific.h"
#include "base/macros.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"
#include "base/threading/simple_thread.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_com_initializer.h"
#include "base/win/scoped_comptr.h"
#include "base/win/scoped_variant.h"
#include "base/win/scoped_handle.h"

#include "media/base/media_export.h"
#include "media/base/audio_parameters.h"
#include "media/direct_show/video_sink_filter.h"
#include "media/direct_show/video_sink_input_pin.h"
#include "media/direct_show/audio_sink_filter.h"
#include "media/direct_show/audio_sink_input_pin.h"
#include "media/direct_show/video_capture_types.h"
#include "media/direct_show/capability_list_win.h"

using media::directshow::DirectShowVideoCaptureFormat;

namespace base {
  template <typename Type>
  struct StaticMemorySingletonTraits;
}
namespace media {

struct VideoCaptureFormat;

class MEDIA_EXPORT DirectShow:
      public base::DelegateSimpleThread::Delegate,
      public AudioSinkFilterObserver,
      public VideoSinkFilterObserver {
  public:
    ~DirectShow();
    static bool IsDeviceWhiteListed(const std::string& name);
    static void GetDeviceVideoCapabilityList(
        const std::string& device_id,
        bool query_detailed_frame_rates,
        DirectShowVideoCapabilityList* out_capability_list);
    static void GetDeviceAudioCapabilityList(
        const std::string& device_id,
        DirectShowAudioCapabilityList* out_capability_list);

    void SetRequestedVideoFormat(VideoCaptureFormat params);
    void SetRequestedAudioFormat(AudioParameters params);
    void RegisterObserver(AudioSinkFilterObserver* observer);
    void UnregisterObserver(AudioSinkFilterObserver* observer);
    void RegisterObserver(VideoSinkFilterObserver* observer);
    void UnregisterObserver(VideoSinkFilterObserver* observer);

  // A utility class that wraps the AM_MEDIA_TYPE type and guarantees that
  // we free the structure when exiting the scope.  DCHECKing is also done to
  // avoid memory leaks.
  class ScopedMediaType {
    public:
      ScopedMediaType() : media_type_(NULL) {}
      ~ScopedMediaType() { Free(); }

      AM_MEDIA_TYPE* operator->() { return media_type_; }
      AM_MEDIA_TYPE* get() { return media_type_; }
      void Free();
      AM_MEDIA_TYPE** Receive();

   private:
    void FreeMediaType(AM_MEDIA_TYPE* mt);
    void DeleteMediaType(AM_MEDIA_TYPE* mt);

    AM_MEDIA_TYPE* media_type_;
  };

 private:
  friend class DirectShowDeviceFactory;
  DirectShow(std::string device_id);

  static VideoPixelFormat TranslateMediaSubtypeToPixelFormat(const GUID& sub_type);
  static void GetVideoPinCapabilityList(
      base::win::ScopedComPtr<IBaseFilter> capture_filter,
      base::win::ScopedComPtr<IPin> output_capture_pin,
      bool query_detailed_frame_rates,
      DirectShowVideoCapabilityList* out_capablility_list);
  static void GetAudioPinCapabilityList(
      base::win::ScopedComPtr<IBaseFilter> capture_filter,
      base::win::ScopedComPtr<IPin> output_capture_pin,
      DirectShowAudioCapabilityList* out_capablility_list);
  static HRESULT GetDeviceFilter(GUID category,
                                 const std::string& device_id,
                                 IBaseFilter** filter);
  static HRESULT GetCrossbarFilter(ICaptureGraphBuilder2* graph_builder,
                                   IBaseFilter* capture_filter,
                                   IBaseFilter** filter);
  static base::win::ScopedComPtr<IPin> GetPin(IBaseFilter* filter,
                                              PIN_DIRECTION pin_dir,
                                              REFGUID category,
                                              REFGUID major_type);
  static base::win::ScopedComPtr<IPin> GetPinByName(IBaseFilter* filter,
                                                    PIN_DIRECTION pin_dir,
                                                    const std::string& pin_name);
  static void PrintPinCapabilities(const std::string& name,
                                   IAMStreamConfig* stream_config);

  void StartThread();
  void StopThread();
  void ShouldGraphBeRunning();
  void ShouldGraphBeStopping();

  HRESULT SetupGraph();
  HRESULT SetupCrossbar();
  HRESULT SetupVideoCapture();
  HRESULT SetupVideoCaptureFormat();
  HRESULT SetupAudioCapture();
  HRESULT SetupAudioCaptureFormat();

  // DelegateSimpleThread::Delegate implementation.
  void Run() override;

  //  AudioSinkFilterObserver implementation
  void AudioFrameReceived(const uint8_t* buffer,
                          int length,
                          base::TimeDelta timestamp) override;

  //  VideoSinkFilterObserver implementation
  void VideoFrameReceived(const uint8_t* buffer,
                          int length,
                          const DirectShowVideoCaptureFormat& format,
                          base::TimeDelta timestamp) override;


  // Capturing is driven by this thread (which has no message loop).
  // All OnData() callbacks will be called from this thread.
  std::unique_ptr<base::DelegateSimpleThread> capture_thread_;

  // locks to protect observers
  base::Lock audio_lock_;
  base::Lock video_lock_;

  AudioSinkFilterObserver* audio_observer_;
  VideoSinkFilterObserver* video_observer_;

  DirectShowVideoCapabilityList video_capabilities_;
  DirectShowAudioCapabilityList audio_capabilities_;

  // video and audio are separate, when an observer
  // is registered, running count would increase by 1
  base::AtomicRefCount running_count_;

  WAVEFORMATEXTENSIBLE requested_audio_format_;
  DirectShowVideoCaptureFormat requested_video_format_;

  bool has_audio_;
  bool has_video_;

  base::win::ScopedComPtr<IBaseFilter> capture_filter_;
  base::win::ScopedComPtr<IBaseFilter> crossbar_filter_;

  base::win::ScopedComPtr<IGraphBuilder> graph_builder_;
  base::win::ScopedComPtr<ICaptureGraphBuilder2> capture_graph_builder_;

  base::win::ScopedComPtr<IMediaControl> media_control_;
  base::win::ScopedComPtr<IPin> input_audio_sink_pin_;
  base::win::ScopedComPtr<IPin> input_video_sink_pin_;
  base::win::ScopedComPtr<IPin> output_video_capture_pin_;
  base::win::ScopedComPtr<IPin> output_audio_capture_pin_;

  scoped_refptr<AudioSinkFilter> audio_sink_filter_;
  scoped_refptr<VideoSinkFilter> video_sink_filter_;

  std::string friendly_name_;
  std::string device_id_;
};

}  // namespace media

#endif  // MEDIA_DIRECT_SHOW_DIRECT_SHOW_H_
