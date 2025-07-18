/* import-globals-from storageAccessAPIHelpers.js */
requestLongerTimeout(2);

AntiTracking.runTest(
  "DOM Cache Always Partition Storage and Storage Access API",
  async _ => {
    await noStorageAccessInitially();

    let effectiveCookieBehavior = SpecialPowers.isContentWindowPrivate(window)
      ? SpecialPowers.Services.prefs.getIntPref(
          "network.cookie.cookieBehavior.pbmode"
        )
      : SpecialPowers.Services.prefs.getIntPref(
          "network.cookie.cookieBehavior"
        );

    let shouldThrow = [
      SpecialPowers.Ci.nsICookieService.BEHAVIOR_REJECT,
      SpecialPowers.Ci.nsICookieService.BEHAVIOR_REJECT_FOREIGN,
    ].includes(effectiveCookieBehavior);

    await caches.open("wow").then(
      _ => {
        ok(!shouldThrow, "DOM Cache can be used!");
      },
      _ => {
        ok(shouldThrow, "DOM Cache can be used!");
      }
    );

    await callRequestStorageAccess();

    await caches.open("wow").then(
      _ => {
        ok(!shouldThrow, "DOM Cache can be used!");
      },
      _ => {
        ok(shouldThrow, "DOM Cache can be used!");
      }
    );
  },
  async _ => {
    await hasStorageAccessInitially();

    await caches.open("wow").then(
      _ => {
        ok(true, "DOM Cache can be used!");
      },
      _ => {
        ok(false, "DOM Cache can be used!");
      }
    );
    await callRequestStorageAccess();

    // For non-tracking windows, calling the API is a no-op
    await caches.open("wow").then(
      _ => {
        ok(true, "DOM Cache can be used!");
      },
      _ => {
        ok(false, "DOM Cache can be used!");
      }
    );
  },
  async _ => {
    await new Promise(resolve => {
      Services.clearData.deleteData(Ci.nsIClearDataService.CLEAR_ALL, () =>
        resolve()
      );
    });
  },
  [
    ["dom.caches.testing.enabled", true],
    ["network.lna.block_trackers", false],
  ],
  false,
  false
);
