/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WGPU_h
#define WGPU_h

// We have to include nsString.h before wgpu_ffi_generated.h because the
// latter is wrapped in an extern "C" declaration but ends up including
// nsString.h (See bug 1784086)
#include "nsString.h"
#include "mozilla/UniquePtr.h"

// Prelude of types necessary before including wgpu_ffi_generated.h
namespace mozilla {
namespace ipc {
class ByteBuf;
}  // namespace ipc
namespace webgpu {
namespace ffi {

#define WGPU_INLINE
#define WGPU_FUNC

extern "C" {
#include "mozilla/webgpu/ffi/wgpu_ffi_generated.h"
}

#undef WGPU_INLINE
#undef WGPU_FUNC

}  // namespace ffi

inline ffi::WGPUByteBuf* ToFFI(ipc::ByteBuf* x) {
  return reinterpret_cast<ffi::WGPUByteBuf*>(x);
}
inline const ffi::WGPUByteBuf* ToFFI(const ipc::ByteBuf* x) {
  return reinterpret_cast<const ffi::WGPUByteBuf*>(x);
}
inline ipc::ByteBuf* FromFFI(ffi::WGPUByteBuf* x) {
  return reinterpret_cast<ipc::ByteBuf*>(x);
}
inline const ipc::ByteBuf* FromFFI(const ffi::WGPUByteBuf* x) {
  return reinterpret_cast<const ipc::ByteBuf*>(x);
}

}  // namespace webgpu

template <>
class DefaultDelete<webgpu::ffi::WGPUClient> {
 public:
  void operator()(webgpu::ffi::WGPUClient* aPtr) const {
    webgpu::ffi::wgpu_client_delete(aPtr);
  }
};

template <>
class DefaultDelete<webgpu::ffi::WGPUGlobal> {
 public:
  void operator()(webgpu::ffi::WGPUGlobal* aPtr) const {
    webgpu::ffi::wgpu_server_delete(aPtr);
  }
};

template <>
class DefaultDelete<webgpu::ffi::WGPUMetalSharedEventHandle> {
 public:
  void operator()(webgpu::ffi::WGPUMetalSharedEventHandle* aPtr) const {
    webgpu::ffi::wgpu_server_delete_metal_shared_event(aPtr);
  }
};

}  // namespace mozilla

#endif  // WGPU_h
