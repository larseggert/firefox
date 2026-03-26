/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsServerSocket_h_
#define nsServerSocket_h_

#include "prio.h"
#include "nsASocketHandler.h"
#include "nsCOMPtr.h"
#include "nsIServerSocket.h"
#include "mozilla/Mutex.h"

//-----------------------------------------------------------------------------

class nsIEventTarget;
namespace mozilla {
namespace net {
union NetAddr;

class nsServerSocket : public nsASocketHandler, public nsIServerSocket {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSISERVERSOCKET

  // nsASocketHandler methods:
  virtual void OnSocketReady(PRFileDesc* fd, int16_t outFlags) override;
  virtual void OnSocketDetached(PRFileDesc* fd) override;
  virtual void IsLocal(bool* aIsLocal) override;
  virtual void KeepWhenOffline(bool* aKeepWhenOffline) override;

  virtual uint64_t ByteCountSent() override { return 0; }
  virtual uint64_t ByteCountReceived() override { return 0; }
  nsServerSocket();

  virtual void CreateClientTransport(PRFileDesc* clientFD,
                                     const mozilla::net::NetAddr& clientAddr);
  virtual nsresult SetSocketDefaults() { return NS_OK; }
  virtual nsresult OnSocketListen() { return NS_OK; }

 protected:
  virtual ~nsServerSocket();
  PRFileDesc* mFD{nullptr};
  nsCOMPtr<nsIServerSocketListener> mListener MOZ_GUARDED_BY(mLock);

 private:
  void OnMsgClose();
  void OnMsgAttach();

  // try attaching our socket (mFD) to the STS's poll list.
  nsresult TryAttach();

  nsresult InitWithAddressInternal(const PRNetAddr* aAddr, int32_t aBackLog,
                                   bool aDualStack = false);

 protected:
  // Returns NS_ERROR_IN_PROGRESS if AsyncListen() has already been called.
  // Subclasses should call this before modifying options that must be set
  // before listening begins.
  nsresult FailIfListening() {
    MutexAutoLock lock(mLock);
    return mListener ? NS_ERROR_IN_PROGRESS : NS_OK;
  }

  // Atomically copies and returns the current listener.
  already_AddRefed<nsIServerSocketListener> CopyListener() {
    MutexAutoLock lock(mLock);
    return do_AddRef(mListener.get());
  }

  PRNetAddr mAddr = {.raw = {0, {0}}};
  nsCOMPtr<nsIEventTarget> mListenerTarget;
  bool mAttached{false};
  bool mKeepWhenOffline{false};

 private:
  // lock protects access to mListener; so it is not cleared while being used.
  mozilla::Mutex mLock{"nsServerSocket.mLock"};
};

}  // namespace net
}  // namespace mozilla

//-----------------------------------------------------------------------------

#endif  // nsServerSocket_h_
