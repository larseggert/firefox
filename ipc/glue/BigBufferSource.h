/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_BigBufferSource_h
#define mozilla_ipc_BigBufferSource_h

#include "mozilla/ipc/BigBuffer.h"
#include "mozilla/StreamBufferSource.h"

namespace mozilla::ipc {

// A StreamBufferSource that owns a BigBuffer, allowing the stream to outlive
// the IPC receive handler that created it.
class BigBufferSource final : public StreamBufferSource {
 public:
  explicit BigBufferSource(BigBuffer&& aBuffer) : mBuffer(std::move(aBuffer)) {}

  Span<const char> Data() override {
    return AsChars(Span(mBuffer.Data(), mBuffer.Size()));
  }

  bool Owning() override { return true; }

  size_t SizeOfExcludingThisEvenIfShared(MallocSizeOf aMallocSizeOf) override {
    if (mBuffer.GetSharedMemory()) {
      return 0;
    }
    return aMallocSizeOf(mBuffer.Data());
  }

 private:
  BigBuffer mBuffer;
};

}  // namespace mozilla::ipc

#endif  // mozilla_ipc_BigBufferSource_h
