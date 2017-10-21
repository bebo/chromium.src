// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/media_foundation_video_encode_accelerator_win.h"

#pragma warning(push)
#pragma warning(disable : 4800)  // Disable warning for added padding.

#include <codecapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <objbase.h>

#include <iterator>
#include <utility>
#include <vector>

#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "base/win/registry.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_variant.h"
#include "base/win/windows_version.h"
#include "media/base/win/mf_helpers.h"
#include "media/base/win/mf_initializer.h"
#include "media/base/win/mf_initializer.h"
#include "base/strings/utf_string_conversions.h"
#include "third_party/libyuv/include/libyuv.h"

using base::win::ScopedComPtr;
using media::mf::MediaBufferScopedPointer;
using base::win::RegKey;

#define BVLOG VLOG

namespace media {

namespace {

const int32_t kDefaultTargetBitrate = 6000000;
const size_t kMaxFrameRateNumerator = 60;
const size_t kMaxFrameRateDenominator = 1;
const size_t kMaxResolutionWidth = 1920;
const size_t kMaxResolutionHeight = 1088;
const size_t kNumInputBuffers = 3;
// Media Foundation uses 100 nanosecond units for time, see
// https://msdn.microsoft.com/en-us/library/windows/desktop/ms697282(v=vs.85).aspx
const size_t kOneMicrosecondInMFSampleTimeUnits = 10;
const size_t kOutputSampleBufferSizeRatio = 4;

constexpr const wchar_t* const kMediaFoundationVideoEncoderDLLs[] = {
    L"mf.dll", L"mfplat.dll",
};

// Resolutions that some platforms support, should be listed in ascending order.
constexpr const gfx::Size kOptionalMaxResolutions[] = {gfx::Size(1280, 720), gfx::Size(1920, 1080), gfx::Size(3840, 2176)};

}  // namespace


class MediaFoundationVideoEncodeAccelerator::EncodeOutput {
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

struct MediaFoundationVideoEncodeAccelerator::BitstreamBufferRef {
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

MediaFoundationVideoEncodeAccelerator::MediaFoundationVideoEncodeAccelerator()
    : main_client_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      encoder_thread_("MFEncoderThread"),
      implementation_name_("unknown MFT"),
      encoder_task_weak_factory_(this) {

  drained_ = CreateEvent(NULL, FALSE, FALSE, NULL);
}

MediaFoundationVideoEncodeAccelerator::
    ~MediaFoundationVideoEncodeAccelerator() {
  DVLOG(3) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());

  DCHECK(!encoder_thread_.IsRunning());
  DCHECK(!encoder_task_weak_factory_.HasWeakPtrs());
}

VideoEncodeAccelerator::SupportedProfiles
MediaFoundationVideoEncodeAccelerator::GetSupportedProfiles() {
  TRACE_EVENT0("gpu,startup",
               "MediaFoundationVideoEncodeAccelerator::GetSupportedProfiles");
  DVLOG(3) << __func__;
  LOG(INFO) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());

  SupportedProfiles profiles;
  target_bitrate_ = kDefaultTargetBitrate;
  frame_rate_ = kMaxFrameRateNumerator / kMaxFrameRateDenominator;
  input_visible_size_ = gfx::Size(kMaxResolutionWidth, kMaxResolutionHeight);
  if (!CreateHardwareEncoderMFT() || !SetEncoderModes() ||
      !InitializeInputOutputSamples()) {
    ReleaseEncoderResources();
    LOG(ERROR)
        << "Hardware encode acceleration is not available on this platform.";
    return profiles;
  }

  // Intel MFT does not support this:
  gfx::Size highest_supported_resolution = input_visible_size_;
  for (const auto& resolution : kOptionalMaxResolutions) {
    DCHECK_GT(resolution.GetArea(), highest_supported_resolution.GetArea());
    if (!IsResolutionSupported(resolution))
      break;
    highest_supported_resolution = resolution;
  }
  ReleaseEncoderResources();

  SupportedProfile profile;
  // More profiles can be supported here, but they should be available in SW
  // fallback as well.
  profile.profile = H264PROFILE_BASELINE;
  profile.max_framerate_numerator = kMaxFrameRateNumerator;
  profile.max_framerate_denominator = kMaxFrameRateDenominator;
  profile.max_resolution = highest_supported_resolution;
  profiles.push_back(profile);
  return profiles;
}

bool MediaFoundationVideoEncodeAccelerator::Initialize(
    VideoPixelFormat format,
    const gfx::Size& input_visible_size,
    VideoCodecProfile output_profile,
    uint32_t initial_bitrate,
    Client* client) {

  // FIXME
  initial_bitrate = kDefaultTargetBitrate;
  DVLOG(3) << __func__ << ": input_format=" << VideoPixelFormatToString(format)
           << ", input_visible_size=" << input_visible_size.ToString()
           << ", output_profile=" << output_profile
           << ", initial_bitrate=" << initial_bitrate;
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

  if (H264PROFILE_BASELINE != output_profile) {
    DLOG(ERROR) << "Output profile not supported= " << output_profile;
    return false;
  }

  encoder_thread_.init_com_with_mta(false);
  if (!encoder_thread_.Start()) {
    DLOG(ERROR) << "Failed spawning encoder thread.";
    return false;
  }
  encoder_thread_task_runner_ = encoder_thread_.task_runner();

  if (!CreateHardwareEncoderMFT()) {
    DLOG(ERROR) << "Failed creating a hardware encoder MFT.";
    return false;
  }

  main_client_weak_factory_.reset(new base::WeakPtrFactory<Client>(client));
  main_client_ = main_client_weak_factory_->GetWeakPtr();
  input_visible_size_ = input_visible_size;
  frame_rate_ = kMaxFrameRateNumerator / kMaxFrameRateDenominator;
  target_bitrate_ = initial_bitrate;
  bitstream_buffer_size_ = input_visible_size.GetArea();
  /* u_plane_offset_ = */
  /*     VideoFrame::PlaneSize(PIXEL_FORMAT_I420, VideoFrame::kYPlane, */
  /*                           input_visible_size_) */
  /*         .GetArea(); */
  /* v_plane_offset_ = u_plane_offset_ + VideoFrame::PlaneSize(PIXEL_FORMAT_I420, */
  /*                                                           VideoFrame::kUPlane, */
  /*                                                           input_visible_size_) */
  /*                                         .GetArea(); */
  /* y_stride_ = VideoFrame::RowBytes(VideoFrame::kYPlane, PIXEL_FORMAT_I420, */
  /*                                  input_visible_size_.width()); */
  /* u_stride_ = VideoFrame::RowBytes(VideoFrame::kUPlane, PIXEL_FORMAT_I420, */
  /*                                  input_visible_size_.width()); */
  /* v_stride_ = VideoFrame::RowBytes(VideoFrame::kVPlane, PIXEL_FORMAT_I420, */
  /*                                  input_visible_size_.width()); */

  u_plane_offset_ = VideoFrame::PlaneSize(PIXEL_FORMAT_NV12,
                                          VideoFrame::kYPlane,
                                          input_visible_size_).GetArea();
  y_stride_ = VideoFrame::RowBytes(VideoFrame::kYPlane, PIXEL_FORMAT_NV12,
                                   input_visible_size_.width());
  u_stride_ = VideoFrame::RowBytes(VideoFrame::kUPlane, PIXEL_FORMAT_NV12,
                                   input_visible_size_.width());

  LOG(INFO) << "u_plane_offset: " << u_plane_offset_
    << " y_stride: " << y_stride_
    << " u_stride: " <<  u_stride_;

  if (!SetEncoderModes()) {
    DLOG(ERROR) << "Failed setting encoder parameters.";
    return false;
  }

  if (!InitializeInputOutputSamples()) {
    LOG(ERROR) << "Failed initializing input-output samples.";
    return false;
  }

  if (!InitializeEventGenerator()) {
    LOG(ERROR) << "Failed initializing event generator";
    return false;
  }

  // TODO: ProcessMessage and BeginGetEvent moves to processInput - lazy init
  // to workaround quicksync can't change bitrate issues.

  HRESULT hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set ProcessMessage", false);

  hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set ProcessMessage MFT_MESSAGE_NOTIFY_START_OF_STREAM", false);

  // Pin all client callbacks to the main task runner initially. It can be
  // reassigned by TryToSetupEncodeOnSeparateThread().
  if (!encode_client_task_runner_) {
    encode_client_task_runner_ = main_client_task_runner_;
    encode_client_ = main_client_;
  }

  hr = imf_media_event_generator_->BeginGetEvent(this, NULL);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set BeginGetEvent", false);

  main_client_task_runner_->PostTask(
      FROM_HERE, base::Bind(&Client::SetImplementationName, main_client_,
      implementation_name_));
  VLOG(3) << "Posting SetImplementationName: " << implementation_name_;

  main_client_task_runner_->PostTask(
      FROM_HERE, base::Bind(&Client::RequireBitstreamBuffers, main_client_,
                            kNumInputBuffers, input_visible_size_,
                            bitstream_buffer_size_));
  return SUCCEEDED(hr);
}

void MediaFoundationVideoEncodeAccelerator::Encode(
    const scoped_refptr<VideoFrame>& frame,
    bool force_keyframe) {
  DVLOG(3) << __func__;
  DCHECK(encode_client_task_runner_->BelongsToCurrentThread());

  encoder_thread_task_runner_->PostTask(
      FROM_HERE, base::Bind(&MediaFoundationVideoEncodeAccelerator::EncodeTask,
                            encoder_task_weak_factory_.GetWeakPtr(), frame,
                            force_keyframe));
}

void MediaFoundationVideoEncodeAccelerator::UseOutputBitstreamBuffer(
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
      base::Bind(
          &MediaFoundationVideoEncodeAccelerator::UseOutputBitstreamBufferTask,
          encoder_task_weak_factory_.GetWeakPtr(), base::Passed(&buffer_ref)));
}

void MediaFoundationVideoEncodeAccelerator::RequestEncodingParametersChange(
    uint32_t bitrate,
    uint32_t framerate) {
  DVLOG(3) << __func__ << ": bitrate=" << bitrate
           << ": framerate=" << framerate;
  DCHECK(encode_client_task_runner_->BelongsToCurrentThread());

  encoder_thread_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&MediaFoundationVideoEncodeAccelerator::
                     RequestEncodingParametersChangeTask,
                 encoder_task_weak_factory_.GetWeakPtr(), bitrate, framerate));
}

void MediaFoundationVideoEncodeAccelerator::Destroy() {
  DVLOG(3) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());

  // Cancel all callbacks.
  main_client_weak_factory_.reset();

  if (encoder_thread_.IsRunning()) {
    encoder_thread_task_runner_->PostTask(
        FROM_HERE,
        base::Bind(&MediaFoundationVideoEncodeAccelerator::DestroyTask,
                   encoder_task_weak_factory_.GetWeakPtr()));
    encoder_thread_.Stop();
  }

  delete this;
}

bool MediaFoundationVideoEncodeAccelerator::TryToSetupEncodeOnSeparateThread(
    const base::WeakPtr<Client>& encode_client,
    const scoped_refptr<base::SingleThreadTaskRunner>& encode_task_runner) {
  DVLOG(3) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());
  encode_client_ = encode_client;
  encode_client_task_runner_ = encode_task_runner;
  return true;
}

// static
void MediaFoundationVideoEncodeAccelerator::PreSandboxInitialization() {
  for (const wchar_t* mfdll : kMediaFoundationVideoEncoderDLLs)
    ::LoadLibrary(mfdll);

}

std::string GuidToString(GUID *guid) {
    char guid_string[37]; // 32 hex chars + 4 hyphens + null terminator
    snprintf(
          guid_string, sizeof(guid_string) / sizeof(guid_string[0]),
          "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
          guid->Data1, guid->Data2, guid->Data3,
          guid->Data4[0], guid->Data4[1], guid->Data4[2],
          guid->Data4[3], guid->Data4[4], guid->Data4[5],
          guid->Data4[6], guid->Data4[7]);
    return guid_string;
}

bool MediaFoundationVideoEncodeAccelerator::CreateHardwareEncoderMFT() {
  DVLOG(3) << __func__;
  LOG(INFO) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());

  if (base::win::GetVersion() < base::win::VERSION_WIN8) {
    DVLOG(ERROR) << "Windows versions earlier than 8 are not supported.";
    return false;
  }

  for (const wchar_t* mfdll : kMediaFoundationVideoEncoderDLLs) {
    if (!::GetModuleHandle(mfdll)) {
      DVLOG(ERROR) << mfdll << " is required for encoding";
      return false;
    }
  }

  if (!InitializeMediaFoundation())
    return false;

  uint32_t flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
  MFT_REGISTER_TYPE_INFO input_info;
  input_info.guidMajorType = MFMediaType_Video;
  input_info.guidSubtype = MFVideoFormat_NV12;
  MFT_REGISTER_TYPE_INFO output_info;
  output_info.guidMajorType = MFMediaType_Video;
  output_info.guidSubtype = MFVideoFormat_H264;

  uint32_t count = 0;

  CLSID av_mft_transform_id = {0};
//  CLSID av_mft_transform_id;

  RegKey beboKey(HKEY_CURRENT_USER, L"SOFTWARE\\Bebo\\App", KEY_READ);
  if (beboKey.Valid()) {
    if (beboKey.HasValue(L"AV_MFT_TRANSFORM_ID")) {
      std::wstring clsid_string;

      beboKey.ReadValue(L"AV_MFT_TRANSFORM_ID", &clsid_string);
      CLSIDFromString(clsid_string.c_str(), &av_mft_transform_id);
      LOG(INFO) << "AV_MFT_TRANSFORM_ID: " << clsid_string;
    }
  }

  IMFActivate **activate = NULL;
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input_info,
          &output_info, &activate, &count);
  RETURN_ON_HR_FAILURE(hr, "Couldn't enumerate hardware encoder", false);
  RETURN_ON_FAILURE((count > 0), "No HW encoder found", false);
  LOG(INFO) << "HW encoder(s) found: " << count;
  DVLOG(3) << "HW encoder(s) found: " << count;

  uint32_t index = 0;

  std::wstring encoder_name;

  PROPVARIANT pvalue = {0};
  for (uint32_t j = 0; j < count; j++) {

    hr = activate[j]->GetItem(MFT_FRIENDLY_NAME_Attribute, &pvalue);
    if (hr != S_OK) {
      LOG(ERROR) << "Could not get friendly name";
    }
    LOG(INFO) << "Available HW encoder[" << j << "]: " << pvalue.pwszVal;
    if (j == 0) {
      encoder_name = pvalue.pwszVal;
    }

    GUID cls_id;
    hr = activate[j]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &cls_id);
    
    if (hr != S_OK) {
      LOG(ERROR) << "can't get MFT_TRANSFORM_CLSID: 0x" << std::hex << hr << std::dec;
      continue;
    }

    LOG(INFO) << "comparing " << GuidToString(&av_mft_transform_id) << " to " << GuidToString(&cls_id);
    if (IsEqualCLSID(av_mft_transform_id, cls_id)) {
        encoder_name = pvalue.pwszVal;
        index = j;
        break;
    }
  }
    
  LOG(INFO) << "Selected encoder: " << encoder_name;
  implementation_name_ = base::WideToUTF8(encoder_name);

  hr = activate[index]->ActivateObject(IID_PPV_ARGS(&encoder_));

  RETURN_ON_HR_FAILURE(hr, "Couldn't activate hardware encoder", false);
  RETURN_ON_FAILURE((encoder_.Get() != nullptr),
                    "No HW encoder instance created", false);
  
  IMFAttributes *attributes;
  encoder_.Get()->GetAttributes(&attributes);
  hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, true);
  RETURN_ON_HR_FAILURE(hr, "Could not set sync", false);
  //FIXME - free all activate instances
  //
  return true;
}

bool MediaFoundationVideoEncodeAccelerator::InitializeInputOutputSamples() {
  LOG(INFO) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());

  DWORD input_count = 0;
  DWORD output_count = 0;
  HRESULT hr = encoder_->GetStreamCount(&input_count, &output_count);
  RETURN_ON_HR_FAILURE(hr, "Couldn't get stream count", false);
  if (input_count < 1 || output_count < 1) {
    LOG(ERROR) << "Stream count too few: input " << input_count << ", output "
               << output_count;
    return false;
  }

  std::vector<DWORD> input_ids(input_count, 0);
  std::vector<DWORD> output_ids(output_count, 0);
  hr = encoder_->GetStreamIDs(input_count, input_ids.data(), output_count,
                              output_ids.data());
  if (hr == S_OK) {
    input_stream_id_ = input_ids[0];
    output_stream_id_ = output_ids[0];
  } else if (hr == E_NOTIMPL) {
    input_stream_id_ = 0;
    output_stream_id_ = 0;
  } else {
    // LOG(ERROR) << "Couldn't find stream ids.";
    LOG(ERROR) << "Couldn't find stream ids." << std::hex << hr << std::dec ;
    RETURN_ON_HR_FAILURE(hr, "Couldn't find stream ids", false);

    return false;
  }

  // Initialize output parameters.
  hr = MFCreateMediaType(imf_output_media_type_.GetAddressOf());
  RETURN_ON_HR_FAILURE(hr, "Couldn't create media type", false);
  hr = imf_output_media_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set media type", false);
  hr = imf_output_media_type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set video format", false);
  hr = imf_output_media_type_->SetUINT32(MF_MT_AVG_BITRATE, target_bitrate_);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set bitrate", false);
  LOG(INFO) << "MF_MT_AVG_BITRATE: " << target_bitrate_;
  hr = MFSetAttributeRatio(imf_output_media_type_.Get(), MF_MT_FRAME_RATE,
                           frame_rate_, 1);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set frame rate", false);
  hr = MFSetAttributeSize(imf_output_media_type_.Get(), MF_MT_FRAME_SIZE,
                          input_visible_size_.width(),
                          input_visible_size_.height());
  RETURN_ON_HR_FAILURE(hr, "Couldn't set frame size", false);
  hr = imf_output_media_type_->SetUINT32(MF_MT_INTERLACE_MODE,
                                         MFVideoInterlace_Progressive);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set interlace mode", false);

  DWORD AVEncH264VProfile = eAVEncH264VProfile_UCConstrainedHigh;

  RegKey beboKey(HKEY_CURRENT_USER, L"SOFTWARE\\Bebo\\App", KEY_READ);
  if (beboKey.Valid()) {
    if (beboKey.HasValue(L"AVEncH264VProfile")) {
       beboKey.ReadValueDW(L"AVEncH264VProfile", &AVEncH264VProfile);
    }
  }
  hr = imf_output_media_type_->SetUINT32(MF_MT_MPEG2_PROFILE,
                                         AVEncH264VProfile);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set codec profile", false);

  LOG(INFO) << "AVEncH264VProfile: " << AVEncH264VProfile;

  // Initialize input parameters.
  hr = MFCreateMediaType(imf_input_media_type_.GetAddressOf());
  RETURN_ON_HR_FAILURE(hr, "Couldn't create media type", false);
  hr = imf_input_media_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set media type", false);
  hr = imf_input_media_type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set video format", false);
  hr = MFSetAttributeRatio(imf_input_media_type_.Get(), MF_MT_FRAME_RATE,
                           frame_rate_, 1);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set frame rate", false);
  hr = MFSetAttributeSize(imf_input_media_type_.Get(), MF_MT_FRAME_SIZE,
                          input_visible_size_.width(),
                          input_visible_size_.height());
  RETURN_ON_HR_FAILURE(hr, "Couldn't set frame size", false);
  hr = imf_input_media_type_->SetUINT32(MF_MT_INTERLACE_MODE,
                                        MFVideoInterlace_Progressive);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set interlace mode", false);

  hr = encoder_->SetOutputType(output_stream_id_, imf_output_media_type_.Get(),
                               0);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set output media type", false);

  hr = encoder_->SetInputType(input_stream_id_, imf_input_media_type_.Get(), 0);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set input media type", false);

  return SUCCEEDED(hr);
}

bool MediaFoundationVideoEncodeAccelerator::InitializeEventGenerator() {
  LOG(INFO) << __func__;
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());
  HRESULT hr = encoder_.Get()->QueryInterface(IID_IMFMediaEventGenerator, &imf_media_event_generator_);
  return SUCCEEDED(hr);
}

bool MediaFoundationVideoEncodeAccelerator::SetEncoderModes() {
  LOG(INFO) << __func__;

  DCHECK(main_client_task_runner_->BelongsToCurrentThread());
  RETURN_ON_FAILURE((encoder_.Get() != nullptr),
                    "No HW encoder instance created", false);

  HRESULT hr = encoder_.CopyTo(codec_api_.GetAddressOf());
  RETURN_ON_HR_FAILURE(hr, "Couldn't get ICodecAPI", false);
  VARIANT var;

  RegKey beboKey(HKEY_CURRENT_USER, L"SOFTWARE\\Bebo\\App", KEY_READ);
  DWORD AVEncCommonQualityVsSpeed = 75;
  DWORD AVEncNumWorkerThreads = 0;
  DWORD AVEncMPVDefaultBPictureCount = 2;
  DWORD AVEncCommonRateControlMode = eAVEncCommonRateControlMode_CBR;
  DWORD AVEncCommonQuality = 0;
  DWORD AVEncH264CABACEnable = 0xDEADBEEF;
  DWORD AVEncAdaptiveMode = eAVEncAdaptiveMode_Resolution;
  DWORD AVEncVideoMinQP = 0;
  DWORD AVLowLatencyMode = true;
  int64_t AVEncVideoEncodeQP = 0x0;

  if (beboKey.Valid()) {
    if (beboKey.HasValue(L"AVEncCommonRateControlMode")) {
       beboKey.ReadValueDW(L"AVEncCommonRateControlMode", &AVEncCommonRateControlMode);
    }
    if (beboKey.HasValue(L"AVEncAdaptiveMode")) {
       beboKey.ReadValueDW(L"AVEncAdaptiveMode", &AVEncAdaptiveMode);
    }
    if (beboKey.HasValue(L"AVEncVideoMinQP ")) {
       beboKey.ReadValueDW(L"AVEncVideoMinQP ", &AVEncVideoMinQP);
    }
    if (beboKey.HasValue(L"AVLowLatencyMode")) {
       beboKey.ReadValueDW(L"AVLowLatencyMode", &AVLowLatencyMode);
    }
    if (beboKey.HasValue(L"AVEncCommonMaxBitRate")) {
       beboKey.ReadValueDW(L"AVEncCommonMaxBitRate", &AVEncCommonMaxBitRate_);
    } else {
       AVEncCommonMaxBitRate_ = 0;
    }
    if (beboKey.HasValue(L"AVEncCommonQuality")) {
       beboKey.ReadValueDW(L"AVEncCommonQuality", &AVEncCommonQuality);
    }
    if (beboKey.HasValue(L"AVEncCommonQualityVsSpeed")) {
       beboKey.ReadValueDW(L"AVEncCommonQualityVsSpeed", &AVEncCommonQualityVsSpeed);
    }
    if (beboKey.HasValue(L"AVEncNumWorkerThreads")) {
       beboKey.ReadValueDW(L"AVEncNumWorkerThreads", &AVEncNumWorkerThreads);
    }
    if (beboKey.HasValue(L"AVEncMPVDefaultBPictureCount")) {
       beboKey.ReadValueDW(L"AVEncMPVDefaultBPictureCount", &AVEncMPVDefaultBPictureCount);
    }
    if (beboKey.HasValue(L"AVEncVideoEncodeQP")) {
       beboKey.ReadInt64(L"AVEncVideoEncodeQP", &AVEncVideoEncodeQP);
    }
    if (beboKey.HasValue(L"AVEncH264CABACEnable")) {
       beboKey.ReadValueDW(L"AVEncH264CABACEnable", &AVEncH264CABACEnable);
    }
  }

  var.vt = VT_UI4;
  var.ulVal = AVEncCommonRateControlMode;
  hr = codec_api_->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set CODECAPI_AVEncCommonRateControlMode", false);
  LOG(INFO) << "CODECAPI_AVEncCommonRateControlMode: " << AVEncCommonRateControlMode;

  var.vt = VT_UI4;
  var.ulVal = target_bitrate_;
  hr = codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set bitrate", false);
  LOG(INFO) << "CODECAPI_AVEncCommonMeanBitRate: " << target_bitrate_;

  if (AVEncCommonQuality) {
    var.vt = VT_UI4;
    var.ulVal = AVEncCommonQuality ;
    hr = codec_api_->SetValue(&CODECAPI_AVEncCommonQuality, &var);
    RETURN_ON_HR_FAILURE(hr, "Couldn't set CODECAPI_AVEncCommonQuality", false);
    LOG(INFO) << "CODECAPI_AVEncCommonQuality: " << AVEncCommonQuality;
  }

  if (AVEncCommonMaxBitRate_) {
    var.vt = VT_UI4;
    var.ulVal = target_bitrate_ * AVEncCommonMaxBitRate_ * 100;
    hr = codec_api_->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &var);
    RETURN_ON_HR_FAILURE(hr, "Couldn't set max bitrate", false);
    LOG(INFO) << "CODECAPI_AVEncCommonMaxBitRate: " << target_bitrate_ * AVEncCommonMaxBitRate_ * 100;
  }

  if (AVEncVideoEncodeQP) {
    var.vt = VT_UI8;
    var.ullVal = AVEncVideoEncodeQP;
    hr = codec_api_->SetValue(&CODECAPI_AVEncVideoEncodeQP, &var);
    RETURN_ON_HR_FAILURE(hr, "Couldn't set encode QP", false);
  }
  LOG(INFO) << "CODECAPI_AVEncVideoEncodeQP: 0x" << std::hex << AVEncVideoEncodeQP << std::dec;

  /* var.vt = VT_UI4; */
  /* var.ulVal = AVEncAdaptiveMode; */
  /* hr = codec_api_->SetValue(&CODECAPI_AVEncAdaptiveMode, &var); */
  /* RETURN_ON_HR_FAILURE(hr, "Couldn't set AVEncAdaptiveMode", false); */
  /* LOG(INFO) << std::hex << "CODECAPI_AVEncAdaptiveMode: 0x" << AVEncAdaptiveMode << std::dec ; */

  var.vt = VT_BOOL;
  var.boolVal = AVLowLatencyMode > 0 ? VARIANT_TRUE : VARIANT_FALSE;
  hr = codec_api_->SetValue(&CODECAPI_AVLowLatencyMode, &var);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set LowLatencyMode", false);
  LOG(INFO) << std::hex << "CODECAPI_AVLowLatencyMode: 0x" << AVLowLatencyMode << std::dec ;

  var.vt = VT_UI4;
  var.ulVal = AVEncCommonQualityVsSpeed;
  hr = codec_api_->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &var);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set CODECAPI_AVEncCommonQualityVsSpeed", false);
  LOG(INFO) << "CODECAPI_AVEncCommonQualityVsSpeed: " << AVEncCommonQualityVsSpeed;

  if (AVEncNumWorkerThreads) {
    var.vt = VT_UI4;
    var.ulVal = AVEncNumWorkerThreads;
    hr = codec_api_->SetValue(&CODECAPI_AVEncNumWorkerThreads, &var);
    RETURN_ON_HR_FAILURE(hr, "Couldn't set CODECAPI_AVEncNumWorkerThreads", false);
  }
  LOG(INFO) << "CODECAPI_AVEncNumWorkerThreads: " << AVEncNumWorkerThreads;

  if (AVEncH264CABACEnable != 0xDEADBEEF) {
    var.vt = VT_BOOL;
    var.boolVal = AVEncH264CABACEnable > 0 ? VARIANT_TRUE : VARIANT_FALSE;
    hr = codec_api_->SetValue(&CODECAPI_AVEncH264CABACEnable, &var);
    RETURN_ON_HR_FAILURE(hr, "Couldn't set AVEncH264CABACEnable", false);
  }
  LOG(INFO) << "CODECAPI_AVEncH264CABACEnable: 0x" << std::hex << AVEncH264CABACEnable << std::dec;

  /* var.vt = VT_UI4; */
  /* var.ulVal = AVEncMPVDefaultBPictureCount; */
  /* hr = codec_api_->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var); */
  /* RETURN_ON_HR_FAILURE(hr, "Couldn't set CODECAPI_AVEncMPVDefaultBPictureCount", false); */
  /* LOG(INFO) << "CODECAPI_AVEncMPVDefaultBPictureCount: " << AVEncMPVDefaultBPictureCount; */

  var.vt = VT_UI4;
  var.ulVal = AVEncVideoMinQP;
  hr = codec_api_->SetValue(&CODECAPI_AVEncVideoMinQP, &var);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set CODECAPI_AVEncVideoMinQP", false);
  LOG(INFO) << "CODECAPI_AVEncVideoMinQP: " << AVEncVideoMinQP;

  return SUCCEEDED(hr);

}

bool MediaFoundationVideoEncodeAccelerator::IsResolutionSupported(
    const gfx::Size& resolution) {
  DCHECK(main_client_task_runner_->BelongsToCurrentThread());
  DCHECK(encoder_);

  LOG(INFO) << "trying resolution: " << resolution.width()
            << "x" << resolution.height();

  HRESULT hr =
      MFSetAttributeSize(imf_output_media_type_.Get(), MF_MT_FRAME_SIZE,
                         resolution.width(), resolution.height());
  RETURN_ON_HR_FAILURE(hr, "Couldn't set frame size", false);
  hr = encoder_->SetOutputType(output_stream_id_, imf_output_media_type_.Get(),
                               0);
  if (hr != S_OK) {
    LOG(INFO) << "IsResolutionSupported Couldn't set output media type";
    return false;
  }

  hr = MFSetAttributeSize(imf_input_media_type_.Get(), MF_MT_FRAME_SIZE,
                          resolution.width(), resolution.height());
  RETURN_ON_HR_FAILURE(hr, "Couldn't set frame size", false);
  hr = encoder_->SetInputType(input_stream_id_, imf_input_media_type_.Get(), 0);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set input media type", false);

  return true;
}

void MediaFoundationVideoEncodeAccelerator::NotifyError(
    VideoEncodeAccelerator::Error error) {
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());
  main_client_task_runner_->PostTask(
      FROM_HERE, base::Bind(&Client::NotifyError, main_client_, error));
}

void MediaFoundationVideoEncodeAccelerator::EncodeTask(
    scoped_refptr<VideoFrame> frame,
    bool force_keyframe) {
  BVLOG(3) << __func__;
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  if (!alive_) {
    LOG(WARNING) << "not alive - got a an encode task - dropping frame";
    return;
  }

  QueueFrame(frame, force_keyframe);
  ProcessOutput();
  ProcessInput();
}

void MediaFoundationVideoEncodeAccelerator::ProcessInputOutput() {
  ProcessOutput();
  ProcessInput();
}

bool MediaFoundationVideoEncodeAccelerator::ProcessEvent(ScopedComPtr<IMFMediaEvent> event) {

  if (event.Get() == nullptr) {
    LOG(ERROR) << "invalid event (null)";
    return false;
  }

  MediaEventType media_event_type = MEUnknown;
  HRESULT event_status = S_OK;
	event->GetType(&media_event_type);
	event->GetStatus(&event_status);

	if (SUCCEEDED(event_status)) {
		if (media_event_type == METransformNeedInput) {
      BVLOG(3) << "input event";
			input_events_++;
		} else if (media_event_type == METransformHaveOutput) {
      BVLOG(3) << "output event";
			output_events_++;
		} else if (media_event_type == METransformDrainComplete) {
      BVLOG(3) << "drain complete";
      HRESULT hr = encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      LOG_IF(ERROR, hr != S_OK) <<  "Error in ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH) 0x" << std::hex << hr << std::dec;
      SetEvent(drained_);
      return false; // don't post new task
		} else {
      LOG(ERROR) << "Unknown Media Event: " << media_event_type;
    }
	} else {
    LOG(ERROR) << "Bad Media Event Status 0x" << std::hex << event_status << std::dec;
  }
  return true;
}

base::win::ScopedComPtr<IMFSample> MediaFoundationVideoEncodeAccelerator::GetInputSample() {
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  int i = 0;
  for (const base::win::ScopedComPtr<IMFSample>& sample: input_sample_pool_) {
    // If the buffer is in use, the ref count will be 2 or higher.
    // If the ref count is 1 then the list we are looping over holds the only reference
    // and it's safe to reuse.
    i++;
    sample.Get()->AddRef();
    LONG c = sample.Get()->Release();
    if (c == 1) {
      return sample;
    }
    if (i > kNumInputBuffers) {
      return nullptr;
    }
  }

  // Allocate new buffer.
  base::win::ScopedComPtr<IMFSample> sample ;

  MFT_INPUT_STREAM_INFO input_stream_info;
  HRESULT hr = encoder_->GetInputStreamInfo(input_stream_id_, &input_stream_info);
  RETURN_ON_HR_FAILURE(hr, "Couldn't get input stream info", nullptr);
  uint32_t buffer_length = input_stream_info.cbSize ? input_stream_info.cbSize
    : VideoFrame::AllocationSize(PIXEL_FORMAT_NV12, input_visible_size_);

  LOG(INFO) << "new input sample with buffer length: " << buffer_length
            << " aligned: " << input_stream_info.cbAlignment;

  sample = mf::CreateEmptySampleWithBuffer(
      buffer_length,
      input_stream_info.cbAlignment);

  input_sample_pool_.push_back(sample);
  return sample;
}

base::win::ScopedComPtr<IMFSample> MediaFoundationVideoEncodeAccelerator::GetOutputSample() {
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  for (const base::win::ScopedComPtr<IMFSample>& sample: output_sample_pool_) {
    // If the buffer is in use, the ref count will be 2 or higher.
    // If the ref count is 1 then the list we are looping over holds the only reference
    // and it's safe to reuse.
    sample.Get()->AddRef();
    LONG c = sample.Get()->Release();
    if (c == 1) {
      return sample;
    }
  }

  MFT_OUTPUT_STREAM_INFO output_stream_info;
  HRESULT hr = encoder_->GetOutputStreamInfo(output_stream_id_, &output_stream_info);
  RETURN_ON_HR_FAILURE(hr, "Couldn't get output stream info", nullptr);
  LOG(INFO) << "new output sample with size: " << output_stream_info.cbSize;

  encoder_provides_samples_ = output_stream_info.dwFlags & 
    (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
  LOG(INFO) << "encoder_provides_samples_: " << encoder_provides_samples_ ;

  uint32_t buffer_length = output_stream_info.cbSize
        ? output_stream_info.cbSize
        : bitstream_buffer_size_ * kOutputSampleBufferSizeRatio;

  LOG(INFO) << "new output sample with buffer length: " << buffer_length
            << " aligned: " << output_stream_info.cbAlignment;

  base::win::ScopedComPtr<IMFSample> sample ;
  sample = mf::CreateEmptySampleWithBuffer(
        buffer_length,
        output_stream_info.cbAlignment);

  input_sample_pool_.push_back(sample);
  return sample;
}

void MediaFoundationVideoEncodeAccelerator::QueueFrame(scoped_refptr<VideoFrame> frame, bool force_keyframe) {

  BVLOG(3) << __func__;

  if (encoder_output_queue_.size() > 3) {
    dropped_bitstream_queue_cnt_++;
    if ((dropped_bitstream_queue_cnt_ % frame_rate_) == 1) {
      LOG(WARNING) << "Dropping Input Buffer - output bitream queue > 3 - cnt: " << dropped_bitstream_queue_cnt_;
    }
    frame = nullptr;
    return;
  }

  const gfx::Rect& frame_size = frame->visible_rect();
  if (frame_size.height() > input_visible_size_.height() ||
      frame_size.width() > input_visible_size_.width()) {
    LOG(WARNING) << "frame is larger than expected visible input size " << frame_size.width() << "x" << frame_size.height();
  }

  base::win::ScopedComPtr<IMFMediaBuffer> input_buffer;
  base::win::ScopedComPtr<IMFSample> input_sample = std::move(GetInputSample());
  if (input_sample == nullptr) {
    dropped_input_cnt_++;
    BVLOG(3) << "Dropping Input Buffer - Queue full";
    if ((dropped_input_cnt_ % frame_rate_) == 1) {
      LOG(WARNING) << "Dropping Input Buffer - Queue full - cnt: " << dropped_input_cnt_;
    }
    frame = nullptr;
    return;
  }

  input_sample->GetBufferByIndex(0, input_buffer.GetAddressOf());

  {
    // TODO: MediaBufferScopedPointer doesn't handle errors when lock can not be aquired...
    MediaBufferScopedPointer scoped_buffer(input_buffer.Get());
    DCHECK(scoped_buffer.get());

    libyuv::I420ToNV12(frame->visible_data(VideoFrame::kYPlane),
                     frame->stride(VideoFrame::kYPlane),
                     frame->visible_data(VideoFrame::kUPlane),
                     frame->stride(VideoFrame::kUPlane),
                     frame->visible_data(VideoFrame::kVPlane),
                     frame->stride(VideoFrame::kVPlane),
                     scoped_buffer.get(),
                     y_stride_,
                     scoped_buffer.get() + u_plane_offset_,
                     u_stride_,
                     input_visible_size_.width(),
                     input_visible_size_.height());
  }
  uint32_t current_length = VideoFrame::AllocationSize(PIXEL_FORMAT_NV12, input_visible_size_);
  HRESULT hr = input_buffer.Get()->SetCurrentLength(current_length);
  RETURN_ON_HR_FAILURE(hr, "Couldn't set current length", );

  input_sample->SetSampleTime(frame->timestamp().InMicroseconds() *
                               kOneMicrosecondInMFSampleTimeUnits);
  UINT64 sample_duration = 1;
  hr = MFFrameRateToAverageTimePerFrame(frame_rate_, 1, &sample_duration);
  RETURN_ON_HR_FAILURE(hr, "Couldn't calculate sample duration", );
  input_sample->SetSampleDuration(sample_duration);

  LONGLONG sample_time;
  input_sample->GetSampleTime(&sample_time);
  BVLOG(3) << "QueueFrame - keyframe: " << force_keyframe << " timestamp: " << sample_time;

  input_sample_queue_.push_back(std::move(input_sample));

  // Release frame after input is copied.
  frame = nullptr;
}

void MediaFoundationVideoEncodeAccelerator::ProcessInput() {

  BVLOG(3) << __func__ << " events: " << input_events_ << " queue empty: " << input_sample_queue_.empty() ;

  while(! input_sample_queue_.empty() && input_events_ > 0) {
    ScopedComPtr<IMFSample> sample = std::move(input_sample_queue_.front());
    input_sample_queue_.pop_front();
    input_events_--;

    LONGLONG sample_time = 0;
    sample->GetSampleTime(&sample_time);

    HRESULT hr = encoder_->ProcessInput(input_stream_id_, sample.Get(), 0);

    // According to MSDN, if encoder returns MF_E_NOTACCEPTING, we need to try
    // processing the output. This error indicates that encoder does not accept
    // any more input data.
    if (hr == MF_E_NOTACCEPTING) {
      DVLOG(3) << "MF_E_NOTACCEPTING";
      LOG(ERROR) << "MF_E_NOTACCEPTING"; // don't expect this on async
      ProcessOutput();
      hr = encoder_->ProcessInput(input_stream_id_, sample.Get(), 0);
      if (!SUCCEEDED(hr)) {
        LOG(ERROR) << "Coudn't encode (after not accepting) 0x" << std::hex << hr << std:: dec;
        alive_ = false;
        NotifyError(kPlatformFailureError);
        RETURN_ON_HR_FAILURE(hr, "Couldn't encode", );
      }
    } else if (!SUCCEEDED(hr)) {
      LOG(ERROR) << "Coudn't encode 0x" << std::hex << hr << std:: dec;
      alive_ = false;
      NotifyError(kPlatformFailureError);
      RETURN_ON_HR_FAILURE(hr, "Couldn't encode", );
    }

    BVLOG(3) << "ProcessInput  - Sent for encode - timestamp " << sample_time;
  }
};

void MediaFoundationVideoEncodeAccelerator::ProcessOutput() {
  BVLOG(3) << __func__ << " events: " << output_events_;
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  while(output_events_ > 0) {
    output_events_--;

    MFT_OUTPUT_DATA_BUFFER output_data_buffer = {0};
    output_data_buffer.dwStreamID = 0;
    output_data_buffer.dwStatus = 0;
    output_data_buffer.pEvents = NULL;
    if (encoder_provides_samples_) {
      output_data_buffer.pSample = NULL;
    } else {
      output_data_buffer.pSample = GetOutputSample().Get();
    }
    DWORD status = 0;
    HRESULT hr;
    hr = encoder_->ProcessOutput(output_stream_id_, 1, &output_data_buffer,
                                 &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      LOG(ERROR) << "MF_E_TRANSFORM_NEED_MORE_INPUT" << status; // not expected for async
      DVLOG(3) << "MF_E_TRANSFORM_NEED_MORE_INPUT" << status;
      return;
    }
    // quicksync...
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      LOG(INFO) << "encoder signaled MF_E_TRANSFORM_STREAM_CHANGE";
      hr = encoder_->GetOutputAvailableType(0, 0, imf_output_media_type_.GetAddressOf());
      if (hr != S_OK) {
        LOG(ERROR) << "MF_E_TRANSFORM_STREAM_CHANGE - Could not Get available output type 0x" << std::hex << hr << std::dec;
      }

      mf::CMediaTypePrinter out_type(imf_output_media_type_.Get());
      LOG(INFO) << "New Output Media Type: " << out_type.ToCompleteString(); 
      hr = encoder_->SetOutputType(0, imf_output_media_type_.Get(), 0);
      if (hr != S_OK) {
        LOG(ERROR) << "MF_E_TRANSFORM_STREAM_CHANGE - Could not set available output type 0x" << std::hex << hr << std::dec;
      }
      /* output_events_++; */
      continue; // try to get output again...

    } else if (hr == E_UNEXPECTED) {
      LOG(ERROR) << "Couldn't get encoded data - shutting down encoder 0x" << std::hex << hr << std::dec;
      NotifyError(kPlatformFailureError);
      RETURN_ON_HR_FAILURE(hr, "Couldn't get encoded data", );
    } else if (! SUCCEEDED(hr)) {
      LOG(ERROR) << "Couldn't get encoded data 0x" << std::hex << hr << std::dec;
      RETURN_ON_HR_FAILURE(hr, "Couldn't get encoded data", );
    }
    BVLOG(3) << "Got encoded data " << hr;
    
    // base::win::ScopedComPtr<IMFSample> sample;
    // sample = output_data_buffer.pSample;

    DWORD buffer_cnt = 0;
    output_data_buffer.pSample->GetBufferCount(&buffer_cnt);
    BVLOG(3) << "Sample Has Buffers: " << buffer_cnt;

    base::win::ScopedComPtr<IMFMediaBuffer> output_buffer;
    if (buffer_cnt == 1) {
      hr = output_data_buffer.pSample->GetBufferByIndex(0, output_buffer.GetAddressOf());
      RETURN_ON_HR_FAILURE(hr, "Couldn't get buffer by index", );
    } else {
      LOG(WARNING) << "Unexpected large buffer converting: " << buffer_cnt;
      hr = output_data_buffer.pSample->ConvertToContiguousBuffer(output_buffer.GetAddressOf());
      RETURN_ON_HR_FAILURE(hr, "Couldn't get contiguous buffer", );
    }

    DWORD size = 0;
    hr = output_buffer->GetCurrentLength(&size);
    RETURN_ON_HR_FAILURE(hr, "Couldn't get buffer length", );

    base::TimeDelta timestamp;
    LONGLONG sample_time;
    hr = output_data_buffer.pSample->GetSampleTime(&sample_time);
    if (SUCCEEDED(hr)) {
      timestamp = base::TimeDelta::FromMicroseconds(
          sample_time / kOneMicrosecondInMFSampleTimeUnits);
    }

    const bool keyframe = MFGetAttributeUINT32(
        output_data_buffer.pSample, MFSampleExtension_CleanPoint, false);
    BVLOG(3) << "We HAVE encoded data with size:" << size << " keyframe "
             << keyframe
             << " timestamp: "
             << sample_time;

    DVLOG(3) << "We HAVE encoded data with size:" << size << " keyframe "
             << keyframe;

    if (bitstream_buffer_queue_.empty()) {
      VLOG(3) << "No bitstream buffers.";
      // We need to copy the output so that encoding can continue.
      std::unique_ptr<EncodeOutput> encode_output(
          new EncodeOutput(size, keyframe, timestamp));
      {
        MediaBufferScopedPointer scoped_buffer(output_buffer.Get());
        memcpy(encode_output->memory(), scoped_buffer.get(), size);
      }
      encoder_output_queue_.push_back(std::move(encode_output));

      if (output_data_buffer.pEvents) {
        LONG ce = output_data_buffer.pEvents->Release();
        BVLOG(3) << "Release() the events: " << ce;
      }

      if (output_data_buffer.pSample) {
        output_data_buffer.pSample->RemoveAllBuffers();

        LONG c = output_data_buffer.pSample->Release();
        BVLOG(3) << "Release() the buffer: " << c;
      }

      return;
    }

    std::unique_ptr<MediaFoundationVideoEncodeAccelerator::BitstreamBufferRef>
        buffer_ref = std::move(bitstream_buffer_queue_.front());
    bitstream_buffer_queue_.pop_front();

    {
      MediaBufferScopedPointer scoped_buffer(output_buffer.Get());
      memcpy(buffer_ref->shm->memory(), scoped_buffer.get(), size);
    }

    if (output_data_buffer.pEvents) {
      LONG ce = output_data_buffer.pEvents->Release();
      BVLOG(3) << "Release() the events: " << ce;
    }

    if (output_data_buffer.pSample) {
      output_data_buffer.pSample->RemoveAllBuffers();

      LONG c = output_data_buffer.pSample->Release();
      BVLOG(3) << "Release() the buffer: " << c;
    }

    BVLOG(3) << "Posted Frame";
    encode_client_task_runner_->PostTask(
        FROM_HERE, base::Bind(&Client::BitstreamBufferReady, encode_client_,
                              buffer_ref->id, size, keyframe, timestamp));
  }
}

void MediaFoundationVideoEncodeAccelerator::UseOutputBitstreamBufferTask(
    std::unique_ptr<BitstreamBufferRef> buffer_ref) {
  BVLOG(3) << __func__;
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  // If there is already EncodeOutput waiting, copy its output first.
  if (!encoder_output_queue_.empty()) {
    std::unique_ptr<MediaFoundationVideoEncodeAccelerator::EncodeOutput>
        encode_output = std::move(encoder_output_queue_.front());
    encoder_output_queue_.pop_front();
    ReturnBitstreamBuffer(std::move(encode_output), std::move(buffer_ref));
    return;
  }

  bitstream_buffer_queue_.push_back(std::move(buffer_ref));
}

void MediaFoundationVideoEncodeAccelerator::ReturnBitstreamBuffer(
    std::unique_ptr<EncodeOutput> encode_output,
    std::unique_ptr<MediaFoundationVideoEncodeAccelerator::BitstreamBufferRef>
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

void MediaFoundationVideoEncodeAccelerator::RequestEncodingParametersChangeTask(
    uint32_t bitrate,
    uint32_t framerate) {
  BVLOG(3) << __func__;
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  frame_rate_ =
      framerate
          ? std::min(framerate, static_cast<uint32_t>(kMaxFrameRateNumerator))
          : 1;

  if (bitrate == 300000) {
    // TODO figure out where this comes from
    LOG(INFO) << "ignoring initial wrong bitrate - CODECAPI_AVEncCommonMeanBitRate: " << bitrate;
    return;
  }

  if (target_bitrate_ != bitrate) {
    target_bitrate_ = bitrate ? bitrate : 1;
    VARIANT var;
    var.vt = VT_UI4;
    var.ulVal = target_bitrate_;

    HRESULT hr = codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    RETURN_ON_HR_FAILURE(hr, "Couldn't set bitrate", );
    LOG(INFO) << "CODECAPI_AVEncCommonMeanBitRate: " << target_bitrate_;

    /* hr = imf_output_media_type_->SetUINT32(MF_MT_AVG_BITRATE, target_bitrate_); */
    /* RETURN_ON_HR_FAILURE(hr, "Couldn't set bitrate", ); */
    /* LOG(INFO) << "MF_MT_AVG_BITRATE: " << target_bitrate_; */
    /* hr = encoder_->SetOutputType(0, imf_output_media_type_.Get(), 0); */
    /* RETURN_ON_HR_FAILURE(hr, "Couldn't set output bitrate", ); */
    /* mf::CMediaTypePrinter out_type(imf_output_media_type_.Get()); */
    /* LOG(INFO) << "New Output Media Type: " << out_type.ToCompleteString(); */ 

    if (AVEncCommonMaxBitRate_) {
      var.vt = VT_UI4;
      var.ulVal = target_bitrate_ * AVEncCommonMaxBitRate_ * 100;
      hr = codec_api_->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &var);
      RETURN_ON_HR_FAILURE(hr, "Couldn't set max bitrate", );
      LOG(INFO) << "CODECAPI_AVEncCommonMaxBitRate: " << target_bitrate_ * AVEncCommonMaxBitRate_ * 100;
    }
  }
}

void MediaFoundationVideoEncodeAccelerator::DestroyTask() {
  BVLOG(3) << __func__;
  DCHECK(encoder_thread_task_runner_->BelongsToCurrentThread());

  // There are reports that some MFT's crash (AMD) if there are
  // unprocessed samples, so we should probably drain all events and process
  // all output, even if we send it to dev/null
  HRESULT hr = S_OK;
 
  for (auto &it: input_sample_queue_) {
    it.Reset();
  }
  input_sample_queue_.clear();

  hr =  encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  LOG_IF(ERROR, hr != S_OK) <<  "DestroyTask - can't ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM) 0x" << std::hex << hr << std::dec;
  hr =  encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  LOG_IF(ERROR, hr != S_OK) <<  "DestroyTask - can't ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN) 0x" << std::hex << hr << std::dec;

  ProcessOutput();

  encoder_task_weak_factory_.InvalidateWeakPtrs();
  WaitForSingleObject(drained_, 1000);

  ProcessOutput();
  alive_ = false;

  // Cancel all encoder thread callbacks.
  
  ReleaseEncoderResources();
}

void MediaFoundationVideoEncodeAccelerator::ReleaseEncoderResources() {
  LOG(INFO) << __func__;

  if (encoder_.Get() != nullptr) {
    MFShutdownObject(encoder_.Get()); // async MFT's must be shut down
  }
  encoder_.Reset();
  LOG_IF(ERROR,!ref_count_.IsZero()) << "MFT still has a reference to us!";
  codec_api_.Reset();
  imf_media_event_generator_.Reset();
  imf_input_media_type_.Reset();
  imf_output_media_type_.Reset();

  for (auto &it: input_sample_pool_) {
    unsigned long c = it.Reset();
    BVLOG(3) << "Releasing input sample - " << c;
  }
  input_sample_pool_.clear();

  for (auto &it: output_sample_pool_) {
    unsigned long c = it.Reset();
    BVLOG(3) << "Releasing output sample - " << c;
  }
  output_sample_pool_.clear();

  CloseHandle(drained_);
}

/* #pragma warning (disable: 4723) */
STDMETHODIMP MediaFoundationVideoEncodeAccelerator::Invoke(IMFAsyncResult *pAsyncResult) {

  DVLOG(3) << __func__;

  HRESULT hr = S_OK;
  base::win::ScopedComPtr<IMFMediaEvent> event;

  // Get the event from the event queue.
  // Assume that m_pEventGenerator is a valid pointer to the
  // event generator's IMFMediaEventGenerator interface.
  hr = imf_media_event_generator_->EndGetEvent(pAsyncResult, event.GetAddressOf());

  /* kill_cnt_++; */
  /* if (kill_cnt_ > (30 * 30)) { */
  /*   kill_cnt_ = kill_cnt_ / 0; // die */
  /* } */

  // Get the event type.
  if (SUCCEEDED(hr))
  {
    if (ProcessEvent(event)) {
      encoder_thread_task_runner_->PostTask(
          FROM_HERE, base::Bind(&MediaFoundationVideoEncodeAccelerator::ProcessInputOutput,
            this));
    }
  } else {
    LOG(ERROR) << "Error from Event Generator 0x" << std::hex << hr << std::dec;
  }

  if (alive_)
  {
    hr = imf_media_event_generator_->BeginGetEvent(this, NULL);
    if (hr != S_OK) {
      LOG(ERROR) << "Error from Event Generator 0x" << std::hex << hr << std::dec;
    }
  }

  return S_OK;
}


STDMETHODIMP MediaFoundationVideoEncodeAccelerator::GetParameters(DWORD *pdwFlags, DWORD *pdwQueue) {
	return E_NOTIMPL;
}

ULONG STDMETHODCALLTYPE MediaFoundationVideoEncodeAccelerator::AddRef() {
  ref_count_.Increment();
  return 1;
}

ULONG STDMETHODCALLTYPE MediaFoundationVideoEncodeAccelerator::Release() {
  if (!ref_count_.Decrement()) {
    // delete this; - can't delete here - Shutting down the async MFT releases us 
    return 0;
  }
  return 1;
}

STDMETHODIMP MediaFoundationVideoEncodeAccelerator::QueryInterface(REFIID riid, void** ppv) {
  if (riid == IID_IUnknown) {
    *ppv = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }

  *ppv = NULL;
  return E_NOINTERFACE;
}

}  // namespace content
