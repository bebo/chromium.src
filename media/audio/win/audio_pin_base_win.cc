// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/win/audio_pin_base_win.h"
#include "media/audio/win/audio_sink_filter_win.h"

#include "base/logging.h"

namespace media {

// Implement IEnumPins.
class TypeEnumerator final : public IEnumMediaTypes,
                             public base::RefCounted<TypeEnumerator> {
 public:
  explicit TypeEnumerator(AudioPinBase* pin) : pin_(pin), index_(0) {}

  // Implement from IUnknown.
  STDMETHOD(QueryInterface)(REFIID iid, void** object_ptr) override {
    if (iid == IID_IEnumMediaTypes || iid == IID_IUnknown) {
      AddRef();
      *object_ptr = static_cast<IEnumMediaTypes*>(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  STDMETHOD_(ULONG, AddRef)() override {
    base::RefCounted<TypeEnumerator>::AddRef();
    return 1;
  }

  STDMETHOD_(ULONG, Release)() override {
    base::RefCounted<TypeEnumerator>::Release();
    return 1;
  }

  // Implement IEnumMediaTypes.
  STDMETHOD(Next)(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override {
    ULONG types_fetched = 0;

    while (types_fetched < count) {
      // Allocate AM_MEDIA_TYPE that we will store the media type in.
      AM_MEDIA_TYPE* type = reinterpret_cast<AM_MEDIA_TYPE*>(
          CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
      if (!type) {
        FreeAllocatedMediaTypes(types_fetched, types);
        return E_OUTOFMEMORY;
      }
      ZeroMemory(type, sizeof(AM_MEDIA_TYPE));

      // Allocate a WAVEFORMATEX and connect it to the AM_MEDIA_TYPE.
      type->cbFormat = sizeof(WAVEFORMATEX);
      BYTE* format = 
        reinterpret_cast<BYTE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
      if (!format) {
        CoTaskMemFree(type);
        FreeAllocatedMediaTypes(types_fetched, types);
        return E_OUTOFMEMORY;
      }

      type->pbFormat = format;

      // Get the media type from the pin.
      if (pin_->GetValidMediaType(index_++, type)) {
        types[types_fetched++] = type;
      } else {
        CoTaskMemFree(format);
        CoTaskMemFree(type);
        break;
      }
    }

    if (fetched)
      *fetched = types_fetched;

    return types_fetched == count ? S_OK : S_FALSE;
  }

  STDMETHOD(Skip)(ULONG count) override {
    index_ += count;
    return S_OK;
  }

  STDMETHOD(Reset)() override {
    index_ = 0;
    return S_OK;
  }

  STDMETHOD(Clone)(IEnumMediaTypes** clone) override {
    TypeEnumerator* type_enum = new TypeEnumerator(pin_.get());
    type_enum->AddRef();
    type_enum->index_ = index_;
    *clone = type_enum;
    return S_OK;
  }

 private:
  friend class base::RefCounted<TypeEnumerator>;
  ~TypeEnumerator() {}

  void FreeAllocatedMediaTypes(ULONG allocated, AM_MEDIA_TYPE** types) {
    for (ULONG i = 0; i < allocated; ++i) {
      CoTaskMemFree(types[i]->pbFormat);
      CoTaskMemFree(types[i]);
    }
  }

  scoped_refptr<AudioPinBase> pin_;
  int index_;
};

AudioPinBase::AudioPinBase(IBaseFilter* owner) : owner_(owner) {
  memset(&current_media_type_, 0, sizeof(current_media_type_));
}

void AudioPinBase::SetOwner(IBaseFilter* owner) {
  owner_ = owner;
}

// Called on an output pin to and establish a
//   connection.
STDMETHODIMP AudioPinBase::Connect(IPin* receive_pin,
                              const AM_MEDIA_TYPE* media_type) {
  if (!receive_pin || !media_type)
    return E_POINTER;

  current_media_type_ = *media_type;
  receive_pin->AddRef();
  connected_pin_.Attach(receive_pin);
  HRESULT hr = receive_pin->ReceiveConnection(this, media_type);

  return hr;
}

// Called from an output pin on an input pin to and establish a
// connection.
STDMETHODIMP AudioPinBase::ReceiveConnection(IPin* connector,
                                        const AM_MEDIA_TYPE* media_type) {
  if (!IsMediaTypeValid(media_type))
    return VFW_E_TYPE_NOT_ACCEPTED;

  current_media_type_ = *media_type;
  connector->AddRef();
  connected_pin_.Attach(connector);
  return S_OK;
}

STDMETHODIMP AudioPinBase::Disconnect() {
  if (!connected_pin_.Get())
    return S_FALSE;

  connected_pin_.Reset();
  return S_OK;
}

STDMETHODIMP AudioPinBase::ConnectedTo(IPin** pin) {
  *pin = connected_pin_.Get();
  if (!connected_pin_.Get())
    return VFW_E_NOT_CONNECTED;

  connected_pin_.Get()->AddRef();
  return S_OK;
}

STDMETHODIMP AudioPinBase::ConnectionMediaType(AM_MEDIA_TYPE* media_type) {
  if (!connected_pin_.Get())
    return VFW_E_NOT_CONNECTED;
  *media_type = current_media_type_;
  return S_OK;
}

STDMETHODIMP AudioPinBase::QueryPinInfo(PIN_INFO* info) {
  info->dir = PINDIR_INPUT;
  info->pFilter = owner_;
  if (owner_)
    owner_->AddRef();
  info->achName[0] = L'\0';

  return S_OK;
}

STDMETHODIMP AudioPinBase::QueryDirection(PIN_DIRECTION* pin_dir) {
  *pin_dir = PINDIR_INPUT;
  return S_OK;
}

STDMETHODIMP AudioPinBase::QueryId(LPWSTR* id) {
  NOTREACHED();
  return E_OUTOFMEMORY;
}

STDMETHODIMP AudioPinBase::QueryAccept(const AM_MEDIA_TYPE* media_type) {
  return S_FALSE;
}

STDMETHODIMP AudioPinBase::EnumMediaTypes(IEnumMediaTypes** types) {
  *types = new TypeEnumerator(this);
  (*types)->AddRef();
  return S_OK;
}

STDMETHODIMP AudioPinBase::QueryInternalConnections(IPin** pins, ULONG* no_pins) {
  return E_NOTIMPL;
}

STDMETHODIMP AudioPinBase::EndOfStream() {
  return S_OK;
}

STDMETHODIMP AudioPinBase::BeginFlush() {
  return S_OK;
}

STDMETHODIMP AudioPinBase::EndFlush() {
  return S_OK;
}

STDMETHODIMP AudioPinBase::NewSegment(REFERENCE_TIME start,
                                 REFERENCE_TIME stop,
                                 double rate) {
  NOTREACHED();
  return E_NOTIMPL;
}

// Inherited from IMemInputPin.
STDMETHODIMP AudioPinBase::GetAllocator(IMemAllocator** allocator) {
  return VFW_E_NO_ALLOCATOR;
}

STDMETHODIMP AudioPinBase::NotifyAllocator(IMemAllocator* allocator,
                                      BOOL read_only) {
  return S_OK;
}

STDMETHODIMP AudioPinBase::GetAllocatorRequirements(
    ALLOCATOR_PROPERTIES* properties) {
  return E_NOTIMPL;
}

STDMETHODIMP AudioPinBase::ReceiveMultiple(IMediaSample** samples,
                                      long sample_count,
                                      long* processed) {
  DCHECK(samples);

  HRESULT hr = S_OK;
  *processed = 0;
  while (sample_count--) {
    hr = Receive(samples[*processed]);
    // S_FALSE means don't send any more.
    if (hr != S_OK)
      break;
    ++(*processed);
  }
  return hr;
}

STDMETHODIMP AudioPinBase::ReceiveCanBlock() {
  return S_FALSE;
}

// Inherited from IUnknown.
STDMETHODIMP AudioPinBase::QueryInterface(REFIID id, void** object_ptr) {
  if (id == IID_IPin || id == IID_IUnknown) {
    *object_ptr = static_cast<IPin*>(this);
  } else if (id == IID_IMemInputPin) {
    *object_ptr = static_cast<IMemInputPin*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) AudioPinBase::AddRef() {
  base::RefCounted<AudioPinBase>::AddRef();
  return 1;
}

STDMETHODIMP_(ULONG) AudioPinBase::Release() {
  base::RefCounted<AudioPinBase>::Release();
  return 1;
}

AudioPinBase::~AudioPinBase() {
}

}  // namespace media
