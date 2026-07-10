/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Original author: ekr@rtfm.com
#include <iostream>

#include "nsASocketHandler.h"
#include "nsCOMPtr.h"
#include "nsISocketTransportService.h"
#include "nsNetCID.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "prio.h"

#define GTEST_HAS_RTTI 0
#include "gtest/gtest.h"
#include "gtest_utils.h"

using namespace mozilla;

namespace {
class SocketHandler;

class SocketTransportServiceTest : public MtransportTest {
 public:
  SocketTransportServiceTest()
      : received_(0),
        readpipe_(nullptr),
        writepipe_(nullptr),
        registered_(false),
        detached_(false) {}

  void SetUp() override;
  void TearDown() override;
  void RegisterHandler();
  void UnregisterHandler();
  void SendEvent();
  void SendPacket();

  void ReceivePacket() { ++received_; }

  void ReceiveEvent() { ++received_; }

  // Called from SocketHandler::OnSocketDetached(), on the STS thread.
  void NotifyDetached() { detached_ = true; }

  size_t Received() { return received_; }

 private:
  nsCOMPtr<nsISocketTransportService> stservice_;
  nsCOMPtr<nsIEventTarget> target_;
  // Non-owning: AttachSocket() (nsASocketHandlerPtr, a raw-pointer IDL
  // param) doesn't take ownership from the caller, and the STS's own
  // internal reference is the only one that should exist -- SocketHandler
  // uses plain (non-atomic, single-owning-thread-checked) NS_DECL_ISUPPORTS,
  // so a second, test-fixture-owned RefPtr here would trip its owning-
  // thread assertion when this fixture (and thus that RefPtr) is destroyed
  // on the gtest runner thread rather than the STS thread. Only ever
  // dereferenced from UnregisterHandler(), always before the STS's own
  // reference is released.
  SocketHandler* handler_ = nullptr;
  std::atomic<size_t> received_;
  PRFileDesc* readpipe_;
  PRFileDesc* writepipe_;
  std::atomic<bool> registered_;
  std::atomic<bool> detached_;
};

// Received an event.
class EventReceived : public Runnable {
 public:
  explicit EventReceived(SocketTransportServiceTest* test)
      : Runnable("EventReceived"), test_(test) {}

  NS_IMETHOD Run() override {
    test_->ReceiveEvent();
    return NS_OK;
  }

  SocketTransportServiceTest* test_;
};

// Register our listener on the socket
class RegisterEvent : public Runnable {
 public:
  explicit RegisterEvent(SocketTransportServiceTest* test)
      : Runnable("RegisterEvent"), test_(test) {}

  NS_IMETHOD Run() override {
    test_->RegisterHandler();
    return NS_OK;
  }

  SocketTransportServiceTest* test_;
};

// Unregister our listener from the socket.
class UnregisterEvent : public Runnable {
 public:
  explicit UnregisterEvent(SocketTransportServiceTest* test)
      : Runnable("UnregisterEvent"), test_(test) {}

  NS_IMETHOD Run() override {
    test_->UnregisterHandler();
    return NS_OK;
  }

  SocketTransportServiceTest* test_;
};

class SocketHandler : public nsASocketHandler {
 public:
  explicit SocketHandler(SocketTransportServiceTest* test) : test_(test) {}

  void OnSocketReady(PRFileDesc* fd, int16_t outflags) override {
    unsigned char buf[1600];

    int32_t rv;
    rv = PR_Recv(fd, buf, sizeof(buf), 0, PR_INTERVAL_NO_WAIT);
    if (rv > 0) {
      std::cerr << "Read " << rv << " bytes\n";
      test_->ReceivePacket();
    }
  }

  // Setting mCondition to a failure code causes the socket transport
  // service's next poll iteration to detach us -- there is no direct public
  // "detach" call. Used from UnregisterHandler() so the test's pipes are no
  // longer registered by the time they're closed in TearDown(); leaving them
  // registered would leak the registration, and the pipe fd numbers get
  // reused by later tests' PR_CreatePipe() calls.
  void RequestClose() { mCondition = NS_ERROR_ABORT; }

  void OnSocketDetached(PRFileDesc* fd) override { test_->NotifyDetached(); }

  void IsLocal(bool* aIsLocal) override {
    // TODO(jesup): better check? Does it matter? (likely no)
    *aIsLocal = false;
  }

  virtual uint64_t ByteCountSent() override { return 0; }
  virtual uint64_t ByteCountReceived() override { return 0; }

  NS_DECL_ISUPPORTS

 protected:
  virtual ~SocketHandler() = default;

 private:
  SocketTransportServiceTest* test_;
};

NS_IMPL_ISUPPORTS0(SocketHandler)

void SocketTransportServiceTest::SetUp() {
  MtransportTest::SetUp();

  // Get the transport service as a dispatch target
  nsresult rv;
  target_ = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &rv);
  ASSERT_TRUE(NS_SUCCEEDED(rv));

  // Get the transport service as a transport service
  stservice_ = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &rv);
  ASSERT_TRUE(NS_SUCCEEDED(rv));

  // Create a loopback pipe
  PRStatus status = PR_CreatePipe(&readpipe_, &writepipe_);
  ASSERT_EQ(status, PR_SUCCESS);

  // Register ourselves as a listener for the read side of the
  // socket. The registration has to happen on the STS thread,
  // hence this event stuff.
  rv = target_->Dispatch(new RegisterEvent(this), NS_DISPATCH_NORMAL);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE_WAIT(registered_, 10000);
}

void SocketTransportServiceTest::TearDown() {
  // Unregister before closing the pipes below: leaving the read side
  // registered with the socket transport service and then closing it out
  // from under that registration would leak it there, and the fd number
  // gets reused by later tests' PR_CreatePipe() calls, which will crash
  // when the poller backend finds it still registered.
  nsresult rv = target_->Dispatch(new UnregisterEvent(this), NS_DISPATCH_NORMAL);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE_WAIT(detached_, 10000);

  if (readpipe_) PR_Close(readpipe_);
  if (writepipe_) PR_Close(writepipe_);

  MtransportTest::TearDown();
}

void SocketTransportServiceTest::RegisterHandler() {
  nsresult rv;

  RefPtr<SocketHandler> handler = new SocketHandler(this);
  rv = stservice_->AttachSocket(readpipe_, handler);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  handler_ = handler.get();

  registered_ = true;
}

void SocketTransportServiceTest::UnregisterHandler() {
  if (handler_) {
    handler_->RequestClose();
  } else {
    // Never registered (SetUp() failed before getting here); nothing to
    // detach.
    detached_ = true;
  }
}

void SocketTransportServiceTest::SendEvent() {
  nsresult rv;

  rv = target_->Dispatch(new EventReceived(this), NS_DISPATCH_NORMAL);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE_WAIT(Received() == 1, 10000);
}

void SocketTransportServiceTest::SendPacket() {
  unsigned char buffer[1024];
  memset(buffer, 0, sizeof(buffer));

  int32_t status = PR_Write(writepipe_, buffer, sizeof(buffer));
  uint32_t size = status & 0xffff;
  ASSERT_EQ(sizeof(buffer), size);
}

// The unit tests themselves
TEST_F(SocketTransportServiceTest, SendEvent) { SendEvent(); }

TEST_F(SocketTransportServiceTest, SendPacket) { SendPacket(); }

}  // end namespace
