AntiTracking._createTask({
  name: "Test that after a storage access grant we have full first-party access",
  cookieBehavior: BEHAVIOR_REJECT_TRACKER,
  blockingByContentBlockingRTUI: true,
  allowList: false,
  callback: async _ => {
    /* import-globals-from storageAccessAPIHelpers.js */
    await noStorageAccessInitially();

    await callRequestStorageAccess();

    const TRACKING_PAGE =
      "https://another-tracking.example.net/browser/toolkit/components/antitracking/test/browser/trackingPage.html";
    async function runChecks(name) {
      let iframe = document.createElement("iframe");
      iframe.src = TRACKING_PAGE;
      let loadPromise = new Promise(resolve => {
        iframe.onload = resolve;
      });
      document.body.appendChild(iframe);
      await loadPromise;

      await SpecialPowers.spawn(iframe, [name], name => {
        content.postMessage(name, "*");
      });

      await new Promise(resolve => {
        onmessage = e => {
          if (e.data == "done") {
            resolve();
          }
        };
      });
    }

    await runChecks("image");
  },
  expectedBlockingNotifications:
    Ci.nsIWebProgressListener.STATE_COOKIES_BLOCKED_TRACKER,
  runInPrivateWindow: false,
  iframeSandbox: null,
  accessRemoval: null,
  callbackAfterRemoval: null,
  thirdPartyPage: TEST_3RD_PARTY_PAGE,
  errorMessageDomains: [
    "https://tracking.example.org",
    "https://tracking.example.org",
  ],
  extraPrefs: [
    // Enable SA heuristics for trackers because the test depends on it.
    [
      "privacy.restrict3rdpartystorage.heuristic.exclude_third_party_trackers",
      false,
    ],
  ],
});

add_task(async _ => {
  await new Promise(resolve => {
    Services.clearData.deleteData(Ci.nsIClearDataService.CLEAR_ALL, () =>
      resolve()
    );
  });
});

AntiTracking._createTask({
  name: "Test that we never grant access to cookieBehavior=2",
  cookieBehavior: BEHAVIOR_REJECT,
  allowList: false,
  callback: async _ => {
    /* import-globals-from storageAccessAPIHelpers.js */
    await noStorageAccessInitially();

    await callRequestStorageAccess(null, true);
  },
  expectedBlockingNotifications: 0,
  runInPrivateWindow: false,
  iframeSandbox: null,
  accessRemoval: null,
  callbackAfterRemoval: null,
  thirdPartyPage: TEST_3RD_PARTY_PAGE,
  errorMessageDomains: [
    "https://tracking.example.org",
    "https://tracking.example.org",
  ],
});

add_task(async _ => {
  await new Promise(resolve => {
    Services.clearData.deleteData(Ci.nsIClearDataService.CLEAR_ALL, () =>
      resolve()
    );
  });
});

AntiTracking._createTask({
  name: "Test that we never grant access to cookieBehavior=3",
  cookieBehavior: BEHAVIOR_LIMIT_FOREIGN,
  allowList: false,
  callback: async _ => {
    /* import-globals-from storageAccessAPIHelpers.js */
    await noStorageAccessInitially();

    await callRequestStorageAccess(null, true);
  },
  expectedBlockingNotifications: 0,
  runInPrivateWindow: false,
  iframeSandbox: null,
  accessRemoval: null,
  callbackAfterRemoval: null,
  thirdPartyPage: TEST_3RD_PARTY_PAGE,
  errorMessageDomains: ["https://tracking.example.org"],
});

add_task(async _ => {
  await new Promise(resolve => {
    Services.clearData.deleteData(Ci.nsIClearDataService.CLEAR_ALL, () =>
      resolve()
    );
  });
});
