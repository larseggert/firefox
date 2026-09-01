/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <cstdlib>

#include "OriginInfo.h"
#include "QuotaManagerDependencyFixture.h"
#include "QuotaManagerImpl.h"
#include "mozilla/Atomics.h"
#include "mozilla/Preferences.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "mozilla/dom/quota/Client.h"
#include "mozilla/dom/quota/CommonMetadata.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/dom/quota/QuotaObject.h"
#include "mozilla/gtest/MozAssertions.h"
#include "mozilla/gtest/MozHelpers.h"
#include "nsThreadUtils.h"

namespace mozilla::dom::quota::test {

static OriginMetadata GetDirtyTrackingTestOriginMetadata() {
  return OriginMetadata{""_ns,
                        "example.com"_ns,
                        "http://example.com"_ns,
                        "http://example.com"_ns,
                        /* aIsPrivate */ false,
                        PERSISTENCE_TYPE_DEFAULT};
}

static ClientMetadata GetDirtyTrackingTestClientMetadata() {
  return {GetDirtyTrackingTestOriginMetadata(), Client::SDB};
}

class TestDirtyTrackingRace : public QuotaManagerDependencyFixture {
 public:
  static void SetUpTestCase() {
    testing::GTEST_FLAG(death_test_style) = "threadsafe";
    ASSERT_NO_FATAL_FAILURE(InitializeFixture());
  }

  static void TearDownTestCase() {
    EXPECT_NO_FATAL_FAILURE(
        ClearStoragesForOrigin(GetDirtyTrackingTestOriginMetadata()));
    ASSERT_NO_FATAL_FAILURE(ShutdownFixture());
  }
};

// Standalone helper for the ResetUsageForClient race death test.
// Manages its own QuotaManager lifecycle so the subprocess can exit cleanly.
static void DoTruncateRaceWithResetUsage() {
  auto reporter = mozilla::gtest::ScopedTestResultReporter::Create(
      mozilla::gtest::ExitMode::NoExit);

  auto testOriginMetadata = GetDirtyTrackingTestOriginMetadata();
  auto testClientMetadata = GetDirtyTrackingTestClientMetadata();

  QuotaManagerDependencyFixture::InitializeFixture();

  QuotaManagerDependencyFixture::ShutdownStorage();
  QuotaManagerDependencyFixture::InitializeStorage();
  QuotaManagerDependencyFixture::InitializeTemporaryStorage();
  QuotaManagerDependencyFixture::InitializeTemporaryOrigin(
      testOriginMetadata, /* aCreateIfNonExistent */ true);
  QuotaManagerDependencyFixture::InitializeTemporaryClient(
      testClientMetadata, /* aCreateIfNonExistent */ true);

  // Setup: create a file and set initial usage.
  QuotaManagerDependencyFixture::PerformClientDirectoryTest(
      testClientMetadata, [testOriginMetadata](int64_t /* aDirectoryLockId */) {
        QuotaManager* quotaManager = QuotaManager::Get();
        ASSERT_TRUE(quotaManager);

        auto testPathRes = quotaManager->GetOrCreateTemporaryOriginDirectory(
            testOriginMetadata);
        ASSERT_TRUE(testPathRes.isOk());

        nsCOMPtr<nsIFile> testPath = testPathRes.unwrap();
        ASSERT_NS_SUCCEEDED(testPath->AppendRelativePath(u"sdb"_ns));
        ASSERT_NS_SUCCEEDED(
            testPath->AppendRelativePath(u"tTestDirtyTrackingRace.txt"_ns));

        bool exists = false;
        ASSERT_NS_SUCCEEDED(testPath->Exists(&exists));
        if (exists) {
          ASSERT_NS_SUCCEEDED(testPath->Remove(false));
        }

        ASSERT_NS_SUCCEEDED(testPath->Create(nsIFile::NORMAL_FILE_TYPE, 0666));

        RefPtr<QuotaObject> quotaObject = quotaManager->GetQuotaObject(
            PERSISTENCE_TYPE_DEFAULT, testOriginMetadata, Client::SDB, testPath,
            /* aFileSize */ 0);
        ASSERT_TRUE(quotaObject);

        ASSERT_TRUE(quotaObject->MaybeUpdateSize(1000, false));
      });

  QuotaManagerDependencyFixture::ShutdownStorage();

  // Race block: trigger MaybeUpdateSize with concurrent ResetUsageForClient.
  QuotaManagerDependencyFixture::InitializeStorage();
  QuotaManagerDependencyFixture::InitializeTemporaryStorage();
  QuotaManagerDependencyFixture::InitializeTemporaryOrigin(testOriginMetadata);
  QuotaManagerDependencyFixture::InitializeTemporaryClient(testClientMetadata);

  Preferences::SetInt("dom.quotaManager.dirtyTracking.pauseOnCallerThreadMs",
                      500);
  mozilla::gtest::DisableCrashReporter();

  QuotaManagerDependencyFixture::PerformClientDirectoryTest(
      testClientMetadata,
      [testOriginMetadata, testClientMetadata](int64_t /* aDirectoryLockId */) {
        QuotaManager* quotaManager = QuotaManager::Get();
        ASSERT_TRUE(quotaManager);

        auto testPathRes = quotaManager->GetOrCreateTemporaryOriginDirectory(
            testOriginMetadata);
        ASSERT_TRUE(testPathRes.isOk());

        nsCOMPtr<nsIFile> testPath = testPathRes.unwrap();
        ASSERT_NS_SUCCEEDED(testPath->AppendRelativePath(u"sdb"_ns));
        ASSERT_NS_SUCCEEDED(
            testPath->AppendRelativePath(u"tTestDirtyTrackingRace.txt"_ns));

        RefPtr<QuotaObject> quotaObject = quotaManager->GetQuotaObject(
            PERSISTENCE_TYPE_DEFAULT, testOriginMetadata, Client::SDB, testPath,
            /* aFileSize */ 1000);
        ASSERT_TRUE(quotaObject);

        quotaManager->WithOriginInfo(testOriginMetadata,
                                     [](const RefPtr<OriginInfo>& aOriginInfo) {
                                       aOriginInfo->LockedSetClean();
                                     });

        nsCOMPtr<nsIThread> ioThread = NS_GetCurrentThread();
        Atomic<bool> resetDone{false};

        nsCOMPtr<nsIRunnable> resetRunnable = NS_NewRunnableFunction(
            "TestResetUsage",
            [quotaManager, testClientMetadata, &resetDone, ioThread]() {
              quotaManager->ResetUsageForClient(testClientMetadata);
              resetDone = true;
              MOZ_ALWAYS_SUCCEEDS(
                  ioThread->Dispatch(NS_NewRunnableFunction("WakeIO", []() {}),
                                     NS_DISPATCH_NORMAL));
            });
        MOZ_ALWAYS_SUCCEEDS(quotaManager->OwningThread()->Dispatch(
            resetRunnable.forget(), NS_DISPATCH_NORMAL));

        bool result = quotaObject->MaybeUpdateSize(500, /* aTruncate */ true);
        EXPECT_TRUE(result);

        SpinEventLoopUntil(
            "Waiting for ResetUsageForClient"_ns,
            [&resetDone]() { return static_cast<bool>(resetDone); });
      });

  Preferences::SetInt("dom.quotaManager.dirtyTracking.pauseOnCallerThreadMs",
                      0);

  QuotaManagerDependencyFixture::ShutdownStorage();
  QuotaManagerDependencyFixture::ClearStoragesForOrigin(testOriginMetadata);
  QuotaManagerDependencyFixture::ShutdownFixture();

  // Use _Exit to skip static destructors that can crash during process
  // teardown (e.g. LinkedList<nsStandardURL> asserting on non-empty list).
  _Exit(mozilla::gtest::ExitCode(reporter->Status()));
}

// Test that ResetUsageForClient during the EagerMarkAsDirty race window
// causes MaybeUpdateSize to crash. On main, PauseLock releases mQuotaMutex
// during EagerMarkAsDirty, allowing ResetUsageForClient on another thread
// to clear mClientUsages to Nothing. When MaybeUpdateSize proceeds with
// LockedTruncateUsages, it hits MOZ_ASSERT(mClientUsages[SDB].isSome()).
// With the fix (mutex held throughout), the operations serialize safely.
// See Bug 2056841.
TEST_F(TestDirtyTrackingRace, MaybeUpdateSize_TruncateRaceWithResetUsage) {
  EXPECT_EXIT(DoTruncateRaceWithResetUsage(), testing::ExitedWithCode(0), "");
}

// Standalone helper for the RemoveQuotaForOrigin race death test.
static void DoRaceWithOriginRemoval() {
  auto reporter = mozilla::gtest::ScopedTestResultReporter::Create(
      mozilla::gtest::ExitMode::NoExit);

  auto testOriginMetadata = GetDirtyTrackingTestOriginMetadata();
  auto testClientMetadata = GetDirtyTrackingTestClientMetadata();

  QuotaManagerDependencyFixture::InitializeFixture();

  QuotaManagerDependencyFixture::ShutdownStorage();
  QuotaManagerDependencyFixture::InitializeStorage();
  QuotaManagerDependencyFixture::InitializeTemporaryStorage();
  QuotaManagerDependencyFixture::InitializeTemporaryOrigin(
      testOriginMetadata, /* aCreateIfNonExistent */ true);
  QuotaManagerDependencyFixture::InitializeTemporaryClient(
      testClientMetadata, /* aCreateIfNonExistent */ true);

  QuotaManagerDependencyFixture::PerformClientDirectoryTest(
      testClientMetadata, [testOriginMetadata](int64_t /* aDirectoryLockId */) {
        QuotaManager* quotaManager = QuotaManager::Get();
        ASSERT_TRUE(quotaManager);

        auto testPathRes = quotaManager->GetOrCreateTemporaryOriginDirectory(
            testOriginMetadata);
        ASSERT_TRUE(testPathRes.isOk());

        nsCOMPtr<nsIFile> testPath = testPathRes.unwrap();
        ASSERT_NS_SUCCEEDED(testPath->AppendRelativePath(u"sdb"_ns));
        ASSERT_NS_SUCCEEDED(
            testPath->AppendRelativePath(u"tTestDirtyTrackingRace.txt"_ns));

        bool exists = false;
        ASSERT_NS_SUCCEEDED(testPath->Exists(&exists));
        if (exists) {
          ASSERT_NS_SUCCEEDED(testPath->Remove(false));
        }

        ASSERT_NS_SUCCEEDED(testPath->Create(nsIFile::NORMAL_FILE_TYPE, 0666));

        RefPtr<QuotaObject> quotaObject = quotaManager->GetQuotaObject(
            PERSISTENCE_TYPE_DEFAULT, testOriginMetadata, Client::SDB, testPath,
            /* aFileSize */ 0);
        ASSERT_TRUE(quotaObject);

        ASSERT_TRUE(quotaObject->MaybeUpdateSize(1000, false));
      });

  QuotaManagerDependencyFixture::ShutdownStorage();

  QuotaManagerDependencyFixture::InitializeStorage();
  QuotaManagerDependencyFixture::InitializeTemporaryStorage();
  QuotaManagerDependencyFixture::InitializeTemporaryOrigin(testOriginMetadata);
  QuotaManagerDependencyFixture::InitializeTemporaryClient(testClientMetadata);

  Preferences::SetInt("dom.quotaManager.dirtyTracking.pauseOnCallerThreadMs",
                      500);
  mozilla::gtest::DisableCrashReporter();

  QuotaManagerDependencyFixture::PerformClientDirectoryTest(
      testClientMetadata, [testOriginMetadata](int64_t /* aDirectoryLockId */) {
        QuotaManager* quotaManager = QuotaManager::Get();
        ASSERT_TRUE(quotaManager);

        auto testPathRes = quotaManager->GetOrCreateTemporaryOriginDirectory(
            testOriginMetadata);
        ASSERT_TRUE(testPathRes.isOk());

        nsCOMPtr<nsIFile> testPath = testPathRes.unwrap();
        ASSERT_NS_SUCCEEDED(testPath->AppendRelativePath(u"sdb"_ns));
        ASSERT_NS_SUCCEEDED(
            testPath->AppendRelativePath(u"tTestDirtyTrackingRace.txt"_ns));

        RefPtr<QuotaObject> quotaObject = quotaManager->GetQuotaObject(
            PERSISTENCE_TYPE_DEFAULT, testOriginMetadata, Client::SDB, testPath,
            /* aFileSize */ 1000);
        ASSERT_TRUE(quotaObject);

        quotaManager->WithOriginInfo(testOriginMetadata,
                                     [](const RefPtr<OriginInfo>& aOriginInfo) {
                                       aOriginInfo->LockedSetClean();
                                     });

        nsCOMPtr<nsIThread> ioThread = NS_GetCurrentThread();
        Atomic<bool> removeDone{false};

        nsCOMPtr<nsIRunnable> removeRunnable = NS_NewRunnableFunction(
            "TestRemoveOrigin",
            [quotaManager, testOriginMetadata, &removeDone, ioThread]() {
              quotaManager->RemoveQuotaForOrigin(PERSISTENCE_TYPE_DEFAULT,
                                                 testOriginMetadata);
              removeDone = true;
              MOZ_ALWAYS_SUCCEEDS(
                  ioThread->Dispatch(NS_NewRunnableFunction("WakeIO", []() {}),
                                     NS_DISPATCH_NORMAL));
            });
        MOZ_ALWAYS_SUCCEEDS(quotaManager->OwningThread()->Dispatch(
            removeRunnable.forget(), NS_DISPATCH_NORMAL));

        bool result = quotaObject->MaybeUpdateSize(500, /* aTruncate */ true);
        EXPECT_TRUE(result);

        SpinEventLoopUntil(
            "Waiting for RemoveQuotaForOrigin"_ns,
            [&removeDone]() { return static_cast<bool>(removeDone); });
      });

  Preferences::SetInt("dom.quotaManager.dirtyTracking.pauseOnCallerThreadMs",
                      0);

  QuotaManagerDependencyFixture::ShutdownStorage();
  QuotaManagerDependencyFixture::ClearStoragesForOrigin(testOriginMetadata);
  QuotaManagerDependencyFixture::ShutdownFixture();

  _Exit(mozilla::gtest::ExitCode(reporter->Status()));
}

// Test that RemoveQuotaForOrigin during the EagerMarkAsDirty race window
// causes MaybeUpdateSize to crash. On main, PauseLock releases mQuotaMutex
// during EagerMarkAsDirty, allowing RemoveQuotaForOrigin on another thread
// to null the OriginInfo's mGroupInfo. When MaybeUpdateSize proceeds,
// LockedMaybeUpdateSize hits MOZ_ASSERT(groupInfo) on the stale OriginInfo.
// With the fix (mutex held throughout), the operations serialize safely.
// See Bug 2056841.
TEST_F(TestDirtyTrackingRace, MaybeUpdateSize_RaceWithOriginRemoval) {
  EXPECT_EXIT(DoRaceWithOriginRemoval(), testing::ExitedWithCode(0), "");
}

}  // namespace mozilla::dom::quota::test
