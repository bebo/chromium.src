// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <mfapi.h>

#include "media/base/win/mf_helpers.h"
#include "base/metrics/histogram_macros.h"
#include "base/logging.h"

#undef IF_EQUAL_RETURN
#ifndef IF_EQUAL_RETURN
#define IF_EQUAL_RETURN(param, val) if(val == param) return #val
#endif


namespace media {

namespace mf {

void LogDXVAError(int line) {
  LOG(ERROR) << "Error in dxva_video_decode_accelerator_win.cc on line "
             << line;
  UMA_HISTOGRAM_SPARSE_SLOWLY("Media.DXVAVDA.ErrorLine", line);
}

Microsoft::WRL::ComPtr<IMFSample> CreateEmptySampleWithBuffer(
    uint32_t buffer_length,
    int align) {
  CHECK_GT(buffer_length, 0U);

  Microsoft::WRL::ComPtr<IMFSample> sample;
  HRESULT hr = MFCreateSample(sample.GetAddressOf());
  RETURN_ON_HR_FAILURE(hr, "MFCreateSample failed",
                       Microsoft::WRL::ComPtr<IMFSample>());

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  if (align == 0) {
    // Note that MFCreateMemoryBuffer is same as MFCreateAlignedMemoryBuffer
    // with the align argument being 0.
    hr = MFCreateMemoryBuffer(buffer_length, buffer.GetAddressOf());
  } else {
    hr = MFCreateAlignedMemoryBuffer(buffer_length, align - 1,
                                     buffer.GetAddressOf());
  }
  RETURN_ON_HR_FAILURE(hr, "Failed to create memory buffer for sample",
                       Microsoft::WRL::ComPtr<IMFSample>());

  hr = sample->AddBuffer(buffer.Get());
  RETURN_ON_HR_FAILURE(hr, "Failed to add buffer to sample",
                       Microsoft::WRL::ComPtr<IMFSample>());

  buffer->SetCurrentLength(0);
  return sample;
}

MediaBufferScopedPointer::MediaBufferScopedPointer(IMFMediaBuffer* media_buffer)
    : media_buffer_(media_buffer),
      buffer_(nullptr),
      max_length_(0),
      current_length_(0) {
  HRESULT hr = media_buffer_->Lock(&buffer_, &max_length_, &current_length_);
  CHECK(SUCCEEDED(hr));
}

MediaBufferScopedPointer::~MediaBufferScopedPointer() {
  HRESULT hr = media_buffer_->Unlock();
  CHECK(SUCCEEDED(hr));
}

// Pretty Print Media Type based on MS example:
// https://github.com/Microsoft/Windows-driver-samples/blob/master/avstream/sampledevicemft/multipinmftutils.cpp
//

#define checkAdjustBufferCap(a,len){\
    char* tStore = NULL; \
if (a && strlen(a) > ((len * 7) / 10)){\
    tStore = a; \
    len *= 2; \
    a = new char[len]; \
if (!a){\
goto done;}\
    a[0] = 0; \
    strcat_s(a, len, tStore); \
    delete(tStore); }\
}

#define DMFTCHECKHR_GOTO(val,label) \
    hr = (val); \
if (FAILED(hr)) {\
    /*DMFTRACE(DMFT_GENERAL, TRACE_LEVEL_INFORMATION, "%!FUNC! exiting %x = %!HRESULT!", hr, hr); */\
    goto label; \
}

LPCSTR GetGUIDNameConst(const GUID& guid)
{
    IF_EQUAL_RETURN(guid, MF_MT_MAJOR_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_MAJOR_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_SUBTYPE);
    IF_EQUAL_RETURN(guid, MF_MT_ALL_SAMPLES_INDEPENDENT);
    IF_EQUAL_RETURN(guid, MF_MT_FIXED_SIZE_SAMPLES);
    IF_EQUAL_RETURN(guid, MF_MT_COMPRESSED);
    IF_EQUAL_RETURN(guid, MF_MT_SAMPLE_SIZE);
    IF_EQUAL_RETURN(guid, MF_MT_WRAPPED_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_NUM_CHANNELS);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_SAMPLES_PER_SECOND);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_FLOAT_SAMPLES_PER_SECOND);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_AVG_BYTES_PER_SECOND);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_BLOCK_ALIGNMENT);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_BITS_PER_SAMPLE);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_VALID_BITS_PER_SAMPLE);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_SAMPLES_PER_BLOCK);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_CHANNEL_MASK);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_FOLDDOWN_MATRIX);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_WMADRC_PEAKREF);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_WMADRC_PEAKTARGET);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_WMADRC_AVGREF);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_WMADRC_AVGTARGET);
    IF_EQUAL_RETURN(guid, MF_MT_AUDIO_PREFER_WAVEFORMATEX);
    IF_EQUAL_RETURN(guid, MF_MT_AAC_PAYLOAD_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION);
    IF_EQUAL_RETURN(guid, MF_MT_FRAME_SIZE);
    IF_EQUAL_RETURN(guid, MF_MT_FRAME_RATE);
    IF_EQUAL_RETURN(guid, MF_MT_FRAME_RATE_RANGE_MAX);
    IF_EQUAL_RETURN(guid, MF_MT_FRAME_RATE_RANGE_MIN);
    IF_EQUAL_RETURN(guid, MF_MT_PIXEL_ASPECT_RATIO);
    IF_EQUAL_RETURN(guid, MF_MT_DRM_FLAGS);
    IF_EQUAL_RETURN(guid, MF_MT_PAD_CONTROL_FLAGS);
    IF_EQUAL_RETURN(guid, MF_MT_SOURCE_CONTENT_HINT);
    IF_EQUAL_RETURN(guid, MF_MT_VIDEO_CHROMA_SITING);
    IF_EQUAL_RETURN(guid, MF_MT_INTERLACE_MODE);
    IF_EQUAL_RETURN(guid, MF_MT_TRANSFER_FUNCTION);
    IF_EQUAL_RETURN(guid, MF_MT_VIDEO_PRIMARIES);
    IF_EQUAL_RETURN(guid, MF_MT_CUSTOM_VIDEO_PRIMARIES);
    IF_EQUAL_RETURN(guid, MF_MT_YUV_MATRIX);
    IF_EQUAL_RETURN(guid, MF_MT_VIDEO_LIGHTING);
    IF_EQUAL_RETURN(guid, MF_MT_VIDEO_NOMINAL_RANGE);
    IF_EQUAL_RETURN(guid, MF_MT_GEOMETRIC_APERTURE);
    IF_EQUAL_RETURN(guid, MF_MT_MINIMUM_DISPLAY_APERTURE);
    IF_EQUAL_RETURN(guid, MF_MT_PAN_SCAN_APERTURE);
    IF_EQUAL_RETURN(guid, MF_MT_PAN_SCAN_ENABLED);
    IF_EQUAL_RETURN(guid, MF_MT_AVG_BITRATE);
    IF_EQUAL_RETURN(guid, MF_MT_AVG_BIT_ERROR_RATE);
    IF_EQUAL_RETURN(guid, MF_MT_MAX_KEYFRAME_SPACING);
    IF_EQUAL_RETURN(guid, MF_MT_DEFAULT_STRIDE);
    IF_EQUAL_RETURN(guid, MF_MT_PALETTE);
    IF_EQUAL_RETURN(guid, MF_MT_USER_DATA);
    IF_EQUAL_RETURN(guid, MF_MT_AM_FORMAT_TYPE);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG_START_TIME_CODE);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG2_PROFILE);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG2_LEVEL);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG2_FLAGS);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG_SEQUENCE_HEADER);
    IF_EQUAL_RETURN(guid, MF_MT_DV_AAUX_SRC_PACK_0);
    IF_EQUAL_RETURN(guid, MF_MT_DV_AAUX_CTRL_PACK_0);
    IF_EQUAL_RETURN(guid, MF_MT_DV_AAUX_SRC_PACK_1);
    IF_EQUAL_RETURN(guid, MF_MT_DV_AAUX_CTRL_PACK_1);
    IF_EQUAL_RETURN(guid, MF_MT_DV_VAUX_SRC_PACK);
    IF_EQUAL_RETURN(guid, MF_MT_DV_VAUX_CTRL_PACK);
    IF_EQUAL_RETURN(guid, MF_MT_ARBITRARY_HEADER);
    IF_EQUAL_RETURN(guid, MF_MT_ARBITRARY_FORMAT);
    IF_EQUAL_RETURN(guid, MF_MT_IMAGE_LOSS_TOLERANT);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG4_SAMPLE_DESCRIPTION);
    IF_EQUAL_RETURN(guid, MF_MT_MPEG4_CURRENT_SAMPLE_ENTRY);
    IF_EQUAL_RETURN(guid, MF_MT_ORIGINAL_4CC);
    IF_EQUAL_RETURN(guid, MF_MT_ORIGINAL_WAVE_FORMAT_TAG);

    // Media types

    IF_EQUAL_RETURN(guid, MFMediaType_Audio);
    IF_EQUAL_RETURN(guid, MFMediaType_Video);
    IF_EQUAL_RETURN(guid, MFMediaType_Protected);
    IF_EQUAL_RETURN(guid, MFMediaType_SAMI);
    IF_EQUAL_RETURN(guid, MFMediaType_Script);
    IF_EQUAL_RETURN(guid, MFMediaType_Image);
    IF_EQUAL_RETURN(guid, MFMediaType_HTML);
    IF_EQUAL_RETURN(guid, MFMediaType_Binary);
    IF_EQUAL_RETURN(guid, MFMediaType_FileTransfer);

    IF_EQUAL_RETURN(guid, MFVideoFormat_AI44); //     FCC('AI44')
    IF_EQUAL_RETURN(guid, MFVideoFormat_ARGB32); //   D3DFMT_A8R8G8B8 
    IF_EQUAL_RETURN(guid, MFVideoFormat_AYUV); //     FCC('AYUV')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DV25); //     FCC('dv25')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DV50); //     FCC('dv50')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DVH1); //     FCC('dvh1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DVSD); //     FCC('dvsd')
    IF_EQUAL_RETURN(guid, MFVideoFormat_DVSL); //     FCC('dvsl')
    IF_EQUAL_RETURN(guid, MFVideoFormat_H264); //     FCC('H264')
    IF_EQUAL_RETURN(guid, MFVideoFormat_I420); //     FCC('I420')
    IF_EQUAL_RETURN(guid, MFVideoFormat_IYUV); //     FCC('IYUV')
    IF_EQUAL_RETURN(guid, MFVideoFormat_M4S2); //     FCC('M4S2')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MJPG);
    IF_EQUAL_RETURN(guid, MFVideoFormat_MP43); //     FCC('MP43')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MP4S); //     FCC('MP4S')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MP4V); //     FCC('MP4V')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MPG1); //     FCC('MPG1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MSS1); //     FCC('MSS1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_MSS2); //     FCC('MSS2')
    IF_EQUAL_RETURN(guid, MFVideoFormat_NV11); //     FCC('NV11')
    IF_EQUAL_RETURN(guid, MFVideoFormat_NV12); //     FCC('NV12')
    IF_EQUAL_RETURN(guid, MFVideoFormat_P010); //     FCC('P010')
    IF_EQUAL_RETURN(guid, MFVideoFormat_P016); //     FCC('P016')
    IF_EQUAL_RETURN(guid, MFVideoFormat_P210); //     FCC('P210')
    IF_EQUAL_RETURN(guid, MFVideoFormat_P216); //     FCC('P216')
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB24); //    D3DFMT_R8G8B8 
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB32); //    D3DFMT_X8R8G8B8 
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB555); //   D3DFMT_X1R5G5B5 
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB565); //   D3DFMT_R5G6B5 
    IF_EQUAL_RETURN(guid, MFVideoFormat_RGB8);
    IF_EQUAL_RETURN(guid, MFVideoFormat_UYVY); //     FCC('UYVY')
    IF_EQUAL_RETURN(guid, MFVideoFormat_v210); //     FCC('v210')
    IF_EQUAL_RETURN(guid, MFVideoFormat_v410); //     FCC('v410')
    IF_EQUAL_RETURN(guid, MFVideoFormat_WMV1); //     FCC('WMV1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_WMV2); //     FCC('WMV2')
    IF_EQUAL_RETURN(guid, MFVideoFormat_WMV3); //     FCC('WMV3')
    IF_EQUAL_RETURN(guid, MFVideoFormat_WVC1); //     FCC('WVC1')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y210); //     FCC('Y210')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y216); //     FCC('Y216')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y410); //     FCC('Y410')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y416); //     FCC('Y416')
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y41P);
    IF_EQUAL_RETURN(guid, MFVideoFormat_Y41T);
    IF_EQUAL_RETURN(guid, MFVideoFormat_YUY2); //     FCC('YUY2')
    IF_EQUAL_RETURN(guid, MFVideoFormat_YV12); //     FCC('YV12')
    IF_EQUAL_RETURN(guid, MFVideoFormat_YVYU);

    IF_EQUAL_RETURN(guid, MFAudioFormat_PCM); //              WAVE_FORMAT_PCM 
    IF_EQUAL_RETURN(guid, MFAudioFormat_Float); //            WAVE_FORMAT_IEEE_FLOAT 
    IF_EQUAL_RETURN(guid, MFAudioFormat_DTS); //              WAVE_FORMAT_DTS 
    IF_EQUAL_RETURN(guid, MFAudioFormat_Dolby_AC3_SPDIF); //  WAVE_FORMAT_DOLBY_AC3_SPDIF 
    IF_EQUAL_RETURN(guid, MFAudioFormat_DRM); //              WAVE_FORMAT_DRM 
    IF_EQUAL_RETURN(guid, MFAudioFormat_WMAudioV8); //        WAVE_FORMAT_WMAUDIO2 
    IF_EQUAL_RETURN(guid, MFAudioFormat_WMAudioV9); //        WAVE_FORMAT_WMAUDIO3 
    IF_EQUAL_RETURN(guid, MFAudioFormat_WMAudio_Lossless); // WAVE_FORMAT_WMAUDIO_LOSSLESS 
    IF_EQUAL_RETURN(guid, MFAudioFormat_WMASPDIF); //         WAVE_FORMAT_WMASPDIF 
    IF_EQUAL_RETURN(guid, MFAudioFormat_MSP1); //             WAVE_FORMAT_WMAVOICE9 
    IF_EQUAL_RETURN(guid, MFAudioFormat_MP3); //              WAVE_FORMAT_MPEGLAYER3 
    IF_EQUAL_RETURN(guid, MFAudioFormat_MPEG); //             WAVE_FORMAT_MPEG 
    IF_EQUAL_RETURN(guid, MFAudioFormat_AAC); //              WAVE_FORMAT_MPEG_HEAAC 
    IF_EQUAL_RETURN(guid, MFAudioFormat_ADTS); //             WAVE_FORMAT_MPEG_ADTS_AAC 

    return NULL;
}

/*++
Description:
    returns an ascii buffer for the GUID passed. The Caller should release the memory allocated
--*/
LPSTR DumpGUIDA(_In_ REFGUID guid)
{
    LPOLESTR lpszGuidString = NULL;
    char *ansiguidStr = NULL;
    if (SUCCEEDED(StringFromCLSID(guid, &lpszGuidString)))
    {
        int mbGuidLen = 0;
        mbGuidLen = WideCharToMultiByte( CP_ACP, WC_NO_BEST_FIT_CHARS, lpszGuidString, -1, NULL, 0, NULL, NULL);
        if (mbGuidLen > 0)
        {
            // FIXME
            /* mf_assert(mbGuidLen == (int)wcslen(lpszGuidString)); */
            ansiguidStr = new char[mbGuidLen];
            WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, lpszGuidString, -1, ansiguidStr, mbGuidLen, NULL, NULL);
            CoTaskMemFree(lpszGuidString);
            ansiguidStr[mbGuidLen - 1] = 0;
        }
    }
    return ansiguidStr;
}

LPSTR DumpAttribute( _In_ const MF_ATTRIBUTE_TYPE& type,
    _In_ REFPROPVARIANT var)
{
    CHAR *tempStr = NULL;
    tempStr = new CHAR[256];
    switch (type)
    {
    case MF_ATTRIBUTE_UINT32:
        if (var.vt == VT_UI4)
        {
            sprintf_s(tempStr, 256, "%u", var.ulVal);
        }
        break;
    case MF_ATTRIBUTE_UINT64:
        if (var.vt == VT_UI8)
        {
            sprintf_s(tempStr, 256, "%I64d  (high: %d low: %d)", var.uhVal.QuadPart, var.uhVal.HighPart, var.uhVal.LowPart);
        }
        break;
    case MF_ATTRIBUTE_DOUBLE:
        if (var.vt == VT_R8)
        {
            sprintf_s(tempStr, 256, "%.4f", var.dblVal);
        }
        break;
    case MF_ATTRIBUTE_GUID:
        if (var.vt == VT_CLSID)
        {
            return DumpGUIDA(*var.puuid);
        }
        break;
    case MF_ATTRIBUTE_STRING:
        if (var.vt == VT_LPWSTR)
        {
            sprintf_s(tempStr, 256, "%S", var.pwszVal);
        }
        break;
    case MF_ATTRIBUTE_IUNKNOWN:
        break;
    default:
        printf("(Unknown Attribute Type = %d) ", type);
        break;
    }
    return tempStr;
}


CMediaTypePrinter::CMediaTypePrinter( 
    _In_ IMFMediaType *_pMediaType  )
    :   pMediaType(_pMediaType),
        m_pBuffer(NULL)
{
}

CMediaTypePrinter::~CMediaTypePrinter()
{
    if (m_pBuffer)
    {
        delete(m_pBuffer);
    }
}

/*++
Description:
Rudimentary function to print the complete Media type
--*/
PCHAR CMediaTypePrinter::ToCompleteString( )
{
    HRESULT             hr          = S_OK;
    UINT32              attrCount   = 0;
    GUID                attrGuid    = { 0 };
    char                *tempStore  = nullptr;
    PROPVARIANT var;
    LPSTR pTempBaseStr;
    MF_ATTRIBUTE_TYPE   pType;
    
    if ( pMediaType && !m_pBuffer )
    {
        DMFTCHECKHR_GOTO(pMediaType->GetCount(&attrCount), done );
        /* LOG(INFO) << "Number of attributes: " << attrCount; */
        buffLen = MEDIAPRINTER_STARTLEN;
        m_pBuffer       = new char[buffLen];
        m_pBuffer[0]    = 0;
        for ( UINT32 ulIndex = 0; ulIndex < attrCount; ulIndex++ )
        {
            PropVariantInit( &var );
            checkAdjustBufferCap( m_pBuffer, buffLen );
            DMFTCHECKHR_GOTO( pMediaType->GetItemByIndex( ulIndex, &attrGuid, &var ), done );
            DMFTCHECKHR_GOTO( pMediaType->GetItemType( attrGuid, &pType ), done );
            if ( ulIndex > 0 )
                strcat_s(m_pBuffer, MEDIAPRINTER_STARTLEN, " : ");
            strcat_s( m_pBuffer, buffLen, GetGUIDNameConst( attrGuid ) );
            strcat_s( m_pBuffer, buffLen, "=" );
            pTempBaseStr = DumpAttribute( pType, var );
            strcat_s( m_pBuffer, buffLen, pTempBaseStr );
            delete( pTempBaseStr );
            PropVariantClear( &var );
        }
    done:
        if ( tempStore )
        {
            delete( tempStore );
        }
    }
    return m_pBuffer;
}

/*++
Description:
Rudimentary function to print the Media type
--*/

PCHAR CMediaTypePrinter::ToString()
{
    //
    //Following are the important ones of Mediatype attributes
    //

    HRESULT     hr = S_OK;
    PROPVARIANT var;
    LPSTR pTempBaseStr;
    MF_ATTRIBUTE_TYPE   pType;
    GUID                attrGuid;
    GUID impGuids[] = {
        MF_MT_SUBTYPE,
        MF_MT_FRAME_SIZE,
        MF_MT_SAMPLE_SIZE,
        MF_MT_FRAME_RATE,
        MF_MT_DEFAULT_STRIDE
        /* MF_XVP_DISABLE_FRC FIXME*/
    };

    if (pMediaType && !m_pBuffer)
    {
        buffLen = MEDIAPRINTER_STARTLEN;
        m_pBuffer = new char[buffLen];
        m_pBuffer[0] = 0;
        for (UINT32 ulIndex = 0; ulIndex < ARRAYSIZE(impGuids); ulIndex++)
        {
            PropVariantInit(&var);
            checkAdjustBufferCap(m_pBuffer, buffLen);
            attrGuid = impGuids[ulIndex];
            DMFTCHECKHR_GOTO(pMediaType->GetItemType(attrGuid, &pType), done);
            DMFTCHECKHR_GOTO(pMediaType->GetItem(attrGuid, &var), done);
            if (ulIndex > 0)
                strcat_s(m_pBuffer, MEDIAPRINTER_STARTLEN, " : ");
            strcat_s(m_pBuffer, buffLen, GetGUIDNameConst(attrGuid));
            strcat_s(m_pBuffer, buffLen, "=");
            pTempBaseStr = DumpAttribute(pType, var);
            strcat_s(m_pBuffer, buffLen, pTempBaseStr);
            delete(pTempBaseStr);
            PropVariantClear(&var);
        }
    }
done:
    return m_pBuffer;
}
}  // namespace mf

}  // namespace media
