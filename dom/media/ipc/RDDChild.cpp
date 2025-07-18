/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "RDDChild.h"

#include "TelemetryProbesReporter.h"
#include "VideoUtils.h"
#include "mozilla/FOGIPC.h"
#include "mozilla/RDDProcessManager.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/MemoryReportRequest.h"
#include "mozilla/gfx/GPUProcessManager.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/ipc/CrashReporterHost.h"
#include "mozilla/ipc/Endpoint.h"

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
#  include "mozilla/SandboxBroker.h"
#  include "mozilla/SandboxBrokerPolicyFactory.h"
#endif

#include "mozilla/Telemetry.h"
#include "mozilla/TelemetryIPC.h"

#if defined(XP_WIN)
#  include "mozilla/WinDllServices.h"
#endif

#include "ProfilerParent.h"
#include "RDDProcessHost.h"

namespace mozilla {

using namespace layers;
using namespace gfx;

RDDChild::RDDChild(RDDProcessHost* aHost) : mHost(aHost) {}

RDDChild::~RDDChild() = default;

bool RDDChild::Init() {
  Maybe<FileDescriptor> brokerFd;

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
  auto policy = SandboxBrokerPolicyFactory::GetRDDPolicy(OtherPid());
  if (policy != nullptr) {
    brokerFd = Some(FileDescriptor());
    mSandboxBroker =
        SandboxBroker::Create(std::move(policy), OtherPid(), brokerFd.ref());
    // This is unlikely to fail and probably indicates OS resource
    // exhaustion, but we can at least try to recover.
    if (NS_WARN_IF(mSandboxBroker == nullptr)) {
      return false;
    }
    MOZ_ASSERT(brokerFd.ref().IsValid());
  }
#endif  // XP_LINUX && MOZ_SANDBOX

  nsTArray<GfxVarUpdate> updates = gfxVars::FetchNonDefaultVars();

  bool isReadyForBackgroundProcessing = false;
#if defined(XP_WIN)
  RefPtr<DllServices> dllSvc(DllServices::Get());
  isReadyForBackgroundProcessing = dllSvc->IsReadyForBackgroundProcessing();
#endif

  SendInit(updates, brokerFd, Telemetry::CanRecordReleaseData(),
           isReadyForBackgroundProcessing);

  Unused << SendInitProfiler(ProfilerParent::CreateForProcess(OtherPid()));

  gfxVars::AddReceiver(this);
  auto* gpm = gfx::GPUProcessManager::Get();
  if (gpm) {
    gpm->AddListener(this);
  }

  return true;
}

bool RDDChild::SendRequestMemoryReport(const uint32_t& aGeneration,
                                       const bool& aAnonymize,
                                       const bool& aMinimizeMemoryUsage,
                                       const Maybe<FileDescriptor>& aDMDFile) {
  mMemoryReportRequest = MakeUnique<MemoryReportRequestHost>(aGeneration);

  PRDDChild::SendRequestMemoryReport(
      aGeneration, aAnonymize, aMinimizeMemoryUsage, aDMDFile,
      [&](const uint32_t& aGeneration2) {
        if (RDDProcessManager* rddpm = RDDProcessManager::Get()) {
          if (RDDChild* child = rddpm->GetRDDChild()) {
            if (child->mMemoryReportRequest) {
              child->mMemoryReportRequest->Finish(aGeneration2);
              child->mMemoryReportRequest = nullptr;
            }
          }
        }
      },
      [&](mozilla::ipc::ResponseRejectReason) {
        if (RDDProcessManager* rddpm = RDDProcessManager::Get()) {
          if (RDDChild* child = rddpm->GetRDDChild()) {
            child->mMemoryReportRequest = nullptr;
          }
        }
      });

  return true;
}

void RDDChild::OnCompositorUnexpectedShutdown() {
  auto* rddm = RDDProcessManager::Get();
  if (rddm) {
    rddm->CreateVideoBridge();
  }
}

void RDDChild::OnVarChanged(const GfxVarUpdate& aVar) { SendUpdateVar(aVar); }

mozilla::ipc::IPCResult RDDChild::RecvAddMemoryReport(
    const MemoryReport& aReport) {
  if (mMemoryReportRequest) {
    mMemoryReportRequest->RecvReport(aReport);
  }
  return IPC_OK();
}

#if defined(XP_WIN)
mozilla::ipc::IPCResult RDDChild::RecvGetModulesTrust(
    ModulePaths&& aModPaths, bool aRunAtNormalPriority,
    GetModulesTrustResolver&& aResolver) {
  RefPtr<DllServices> dllSvc(DllServices::Get());
  dllSvc->GetModulesTrust(std::move(aModPaths), aRunAtNormalPriority)
      ->Then(
          GetMainThreadSerialEventTarget(), __func__,
          [aResolver](ModulesMapResult&& aResult) {
            aResolver(Some(ModulesMapResult(std::move(aResult))));
          },
          [aResolver](nsresult aRv) { aResolver(Nothing()); });
  return IPC_OK();
}
#endif  // defined(XP_WIN)

mozilla::ipc::IPCResult RDDChild::RecvUpdateMediaCodecsSupported(
    const media::MediaCodecsSupported& aSupported) {
#if defined(XP_MACOSX) || defined(XP_LINUX)
  // We report this on GPUChild on Windows and Android
  if (ContainHardwareCodecsSupported(aSupported)) {
    mozilla::TelemetryProbesReporter::ReportDeviceMediaCodecSupported(
        aSupported);
  }
#endif
  dom::ContentParent::BroadcastMediaCodecsSupportedUpdate(
      RemoteMediaIn::RddProcess, aSupported);
  return IPC_OK();
}

mozilla::ipc::IPCResult RDDChild::RecvAccumulateChildHistograms(
    nsTArray<HistogramAccumulation>&& aAccumulations) {
  TelemetryIPC::AccumulateChildHistograms(Telemetry::ProcessID::Rdd,
                                          aAccumulations);
  return IPC_OK();
}

mozilla::ipc::IPCResult RDDChild::RecvAccumulateChildKeyedHistograms(
    nsTArray<KeyedHistogramAccumulation>&& aAccumulations) {
  TelemetryIPC::AccumulateChildKeyedHistograms(Telemetry::ProcessID::Rdd,
                                               aAccumulations);
  return IPC_OK();
}

mozilla::ipc::IPCResult RDDChild::RecvUpdateChildScalars(
    nsTArray<ScalarAction>&& aScalarActions) {
  TelemetryIPC::UpdateChildScalars(Telemetry::ProcessID::Rdd, aScalarActions);
  return IPC_OK();
}

mozilla::ipc::IPCResult RDDChild::RecvUpdateChildKeyedScalars(
    nsTArray<KeyedScalarAction>&& aScalarActions) {
  TelemetryIPC::UpdateChildKeyedScalars(Telemetry::ProcessID::Rdd,
                                        aScalarActions);
  return IPC_OK();
}

mozilla::ipc::IPCResult RDDChild::RecvRecordChildEvents(
    nsTArray<mozilla::Telemetry::ChildEventData>&& aEvents) {
  TelemetryIPC::RecordChildEvents(Telemetry::ProcessID::Rdd, aEvents);
  return IPC_OK();
}

mozilla::ipc::IPCResult RDDChild::RecvRecordDiscardedData(
    const mozilla::Telemetry::DiscardedData& aDiscardedData) {
  TelemetryIPC::RecordDiscardedData(Telemetry::ProcessID::Rdd, aDiscardedData);
  return IPC_OK();
}

mozilla::ipc::IPCResult RDDChild::RecvFOGData(ByteBuf&& aBuf) {
  glean::FOGData(std::move(aBuf));
  return IPC_OK();
}

void RDDChild::ActorDestroy(ActorDestroyReason aWhy) {
  if (aWhy == AbnormalShutdown) {
    GenerateCrashReport();
  }

  auto* gpm = gfx::GPUProcessManager::Get();
  if (gpm) {
    // Note: the manager could have shutdown already.
    gpm->RemoveListener(this);
  }

  gfxVars::RemoveReceiver(this);
  mHost->OnChannelClosed();
}

class DeferredDeleteRDDChild : public Runnable {
 public:
  explicit DeferredDeleteRDDChild(RefPtr<RDDChild>&& aChild)
      : Runnable("gfx::DeferredDeleteRDDChild"), mChild(std::move(aChild)) {}

  NS_IMETHODIMP Run() override { return NS_OK; }

 private:
  RefPtr<RDDChild> mChild;
};

/* static */
void RDDChild::Destroy(RefPtr<RDDChild>&& aChild) {
  NS_DispatchToMainThread(new DeferredDeleteRDDChild(std::move(aChild)));
}

}  // namespace mozilla
