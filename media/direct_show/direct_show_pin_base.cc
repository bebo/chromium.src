// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/direct_show/direct_show_pin_base.h"
#include "media/direct_show/audio_sink_filter.h"

#include "base/logging.h"

namespace media {

// Implement IEnumPins.
class TypeEnumerator final : public IEnumMediaTypes,
                             public base::RefCounted<TypeEnumerator> {
 public:
  explicit TypeEnumerator(DirectShowPinBase* pin) : pin_(pin), index_(0) {}

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

  scoped_refptr<DirectShowPinBase> pin_;
  int index_;
};

DirectShowPinBase::DirectShowPinBase(IBaseFilter* owner) : owner_(owner) {
  memset(&current_media_type_, 0, sizeof(current_media_type_));
}

void DirectShowPinBase::SetOwner(IBaseFilter* owner) {
  owner_ = owner;
}

// Called on an output pin to and establish a
//   connection.
STDMETHODIMP DirectShowPinBase::Connect(IPin* receive_pin,
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
STDMETHODIMP DirectShowPinBase::ReceiveConnection(IPin* connector,
                                        const AM_MEDIA_TYPE* media_type) {
  if (!IsMediaTypeValid(media_type))
    return VFW_E_TYPE_NOT_ACCEPTED;

  current_media_type_ = *media_type;
  connector->AddRef();
  connected_pin_.Attach(connector);
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::Disconnect() {
  if (!connected_pin_.Get())
    return S_FALSE;

  connected_pin_.Reset();
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::ConnectedTo(IPin** pin) {
  *pin = connected_pin_.Get();
  if (!connected_pin_.Get())
    return VFW_E_NOT_CONNECTED;

  connected_pin_.Get()->AddRef();
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::ConnectionMediaType(AM_MEDIA_TYPE* media_type) {
  if (!connected_pin_.Get())
    return VFW_E_NOT_CONNECTED;
  *media_type = current_media_type_;
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::QueryPinInfo(PIN_INFO* info) {
  info->dir = PINDIR_INPUT;
  info->pFilter = owner_;
  if (owner_)
    owner_->AddRef();
  info->achName[0] = L'\0';

  return S_OK;
}

STDMETHODIMP DirectShowPinBase::QueryDirection(PIN_DIRECTION* pin_dir) {
  *pin_dir = PINDIR_INPUT;
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::QueryId(LPWSTR* id) {
  NOTREACHED();
  return E_OUTOFMEMORY;
}

STDMETHODIMP DirectShowPinBase::QueryAccept(const AM_MEDIA_TYPE* media_type) {
  return S_FALSE;
}

STDMETHODIMP DirectShowPinBase::EnumMediaTypes(IEnumMediaTypes** types) {
  *types = new TypeEnumerator(this);
  (*types)->AddRef();
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::QueryInternalConnections(IPin** pins, ULONG* no_pins) {
  return E_NOTIMPL;
}

STDMETHODIMP DirectShowPinBase::EndOfStream() {
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::BeginFlush() {
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::EndFlush() {
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::NewSegment(REFERENCE_TIME start,
                                 REFERENCE_TIME stop,
                                 double rate) {
  NOTREACHED();
  return E_NOTIMPL;
}

// Inherited from IMemInputPin.
STDMETHODIMP DirectShowPinBase::GetAllocator(IMemAllocator** allocator) {
  return VFW_E_NO_ALLOCATOR;
}

STDMETHODIMP DirectShowPinBase::NotifyAllocator(IMemAllocator* allocator,
                                      BOOL read_only) {
  return S_OK;
}

STDMETHODIMP DirectShowPinBase::GetAllocatorRequirements(
    ALLOCATOR_PROPERTIES* properties) {
  LOG(INFO) << "GetAllocatorRequirements in align: " << properties->cbAlign
    << ", size: " << properties->cbBuffer
    << ", prefix: "<< properties->cbPrefix
    << ", count: " << properties->cBuffers;
  // properties->cbBuffer = 2;
  // properties->cbBuffers = 10 * 480; // * avgBytes

  return E_NOTIMPL;
}

STDMETHODIMP DirectShowPinBase::ReceiveMultiple(IMediaSample** samples,
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

STDMETHODIMP DirectShowPinBase::ReceiveCanBlock() {
  return S_FALSE;
}

// Inherited from IUnknown.
STDMETHODIMP DirectShowPinBase::QueryInterface(REFIID id, void** object_ptr) {
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

STDMETHODIMP_(ULONG) DirectShowPinBase::AddRef() {
  base::RefCounted<DirectShowPinBase>::AddRef();
  return 1;
}

STDMETHODIMP_(ULONG) DirectShowPinBase::Release() {
  base::RefCounted<DirectShowPinBase>::Release();
  return 1;
}

DirectShowPinBase::~DirectShowPinBase() {
}

}  // namespace media
