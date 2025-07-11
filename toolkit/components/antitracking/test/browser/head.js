/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const TEST_DOMAIN = "http://example.net/";
const TEST_DOMAIN_HTTPS = "https://example.net/";
const TEST_DOMAIN_2 = "http://xn--exmple-cua.test/";
const TEST_DOMAIN_3 = "https://xn--hxajbheg2az3al.xn--jxalpdlp/";
const TEST_DOMAIN_4 = "http://prefixexample.com/";
const TEST_DOMAIN_5 = "http://test/";
const TEST_DOMAIN_6 = "http://mochi.test:8888/";
const TEST_DOMAIN_7 = "http://example.com/";
const TEST_DOMAIN_8 = "http://www.example.com/";
const TEST_DOMAIN_9 = "https://example.org:443/";
const TEST_3RD_PARTY_DOMAIN = "https://tracking.example.org/";
const TEST_3RD_PARTY_DOMAIN_HTTP = "http://tracking.example.org/";
const TEST_3RD_PARTY_DOMAIN_TP = "https://tracking.example.com/";
const TEST_3RD_PARTY_DOMAIN_STP = "https://social-tracking.example.org/";
const TEST_4TH_PARTY_DOMAIN = "http://not-tracking.example.com/";
const TEST_4TH_PARTY_DOMAIN_HTTPS = "https://not-tracking.example.com/";
const TEST_ANOTHER_3RD_PARTY_DOMAIN_HTTP =
  "http://another-tracking.example.net/";
const TEST_ANOTHER_3RD_PARTY_DOMAIN_HTTPS =
  "https://another-tracking.example.net/";
const TEST_ANOTHER_3RD_PARTY_DOMAIN = SpecialPowers.useRemoteSubframes
  ? TEST_ANOTHER_3RD_PARTY_DOMAIN_HTTP
  : TEST_ANOTHER_3RD_PARTY_DOMAIN_HTTPS;
const TEST_EMAIL_TRACKER_DOMAIN = "http://email-tracking.example.org/";

const TEST_PATH = "browser/toolkit/components/antitracking/test/browser/";

const TEST_TOP_PAGE = TEST_DOMAIN + TEST_PATH + "page.html";
const TEST_TOP_PAGE_HTTPS = TEST_DOMAIN_HTTPS + TEST_PATH + "page.html";
const TEST_TOP_PAGE_2 = TEST_DOMAIN_2 + TEST_PATH + "page.html";
const TEST_TOP_PAGE_3 = TEST_DOMAIN_3 + TEST_PATH + "page.html";
const TEST_TOP_PAGE_4 = TEST_DOMAIN_4 + TEST_PATH + "page.html";
const TEST_TOP_PAGE_5 = TEST_DOMAIN_5 + TEST_PATH + "page.html";
const TEST_TOP_PAGE_6 = TEST_DOMAIN_6 + TEST_PATH + "page.html";
const TEST_TOP_PAGE_7 = TEST_DOMAIN_7 + TEST_PATH + "page.html";
const TEST_TOP_PAGE_8 = TEST_DOMAIN_8 + TEST_PATH + "page.html";
const TEST_TOP_PAGE_9 = TEST_DOMAIN_9 + TEST_PATH + "page.html";
const TEST_EMBEDDER_PAGE = TEST_DOMAIN + TEST_PATH + "embedder.html";
const TEST_POPUP_PAGE = TEST_DOMAIN + TEST_PATH + "popup.html";
const TEST_IFRAME_PAGE = TEST_DOMAIN + TEST_PATH + "iframe.html";
const TEST_3RD_PARTY_PAGE = TEST_3RD_PARTY_DOMAIN + TEST_PATH + "3rdParty.html";
const TEST_3RD_PARTY_PAGE_HTTP =
  TEST_3RD_PARTY_DOMAIN_HTTP + TEST_PATH + "3rdParty.html";
const TEST_3RD_PARTY_PAGE_WO =
  TEST_3RD_PARTY_DOMAIN + TEST_PATH + "3rdPartyWO.html";
const TEST_3RD_PARTY_PAGE_UI =
  TEST_3RD_PARTY_DOMAIN + TEST_PATH + "3rdPartyUI.html";
const TEST_3RD_PARTY_PAGE_WITH_SVG =
  TEST_3RD_PARTY_DOMAIN + TEST_PATH + "3rdPartySVG.html";
const TEST_3RD_PARTY_PAGE_RELAY =
  TEST_4TH_PARTY_DOMAIN + TEST_PATH + "3rdPartyRelay.html";
const TEST_4TH_PARTY_PAGE = TEST_4TH_PARTY_DOMAIN + TEST_PATH + "3rdParty.html";
const TEST_4TH_PARTY_PAGE_HTTPS =
  TEST_4TH_PARTY_DOMAIN_HTTPS + TEST_PATH + "3rdParty.html";
const TEST_ANOTHER_3RD_PARTY_PAGE =
  TEST_ANOTHER_3RD_PARTY_DOMAIN + TEST_PATH + "3rdParty.html";
const TEST_ANOTHER_3RD_PARTY_PAGE_HTTPS =
  TEST_ANOTHER_3RD_PARTY_DOMAIN_HTTPS + TEST_PATH + "3rdParty.html";
const TEST_3RD_PARTY_STORAGE_PAGE =
  TEST_3RD_PARTY_DOMAIN_HTTP + TEST_PATH + "3rdPartyStorage.html";
const TEST_3RD_PARTY_PAGE_WORKER =
  TEST_3RD_PARTY_DOMAIN + TEST_PATH + "3rdPartyWorker.html";
const TEST_3RD_PARTY_PARTITIONED_PAGE =
  TEST_3RD_PARTY_DOMAIN + TEST_PATH + "3rdPartyPartitioned.html";
const TEST_4TH_PARTY_STORAGE_PAGE =
  TEST_4TH_PARTY_DOMAIN + TEST_PATH + "3rdPartyStorage.html";
const TEST_4TH_PARTY_STORAGE_PAGE_HTTPS =
  TEST_4TH_PARTY_DOMAIN_HTTPS + TEST_PATH + "3rdPartyStorage.html";
const TEST_4TH_PARTY_PARTITIONED_PAGE =
  TEST_4TH_PARTY_DOMAIN + TEST_PATH + "3rdPartyPartitioned.html";
const TEST_4TH_PARTY_PARTITIONED_PAGE_HTTPS =
  TEST_4TH_PARTY_DOMAIN_HTTPS + TEST_PATH + "3rdPartyPartitioned.html";
const BEHAVIOR_ACCEPT = Ci.nsICookieService.BEHAVIOR_ACCEPT;
const BEHAVIOR_REJECT = Ci.nsICookieService.BEHAVIOR_REJECT;
const BEHAVIOR_LIMIT_FOREIGN = Ci.nsICookieService.BEHAVIOR_LIMIT_FOREIGN;
const BEHAVIOR_REJECT_FOREIGN = Ci.nsICookieService.BEHAVIOR_REJECT_FOREIGN;
const BEHAVIOR_REJECT_TRACKER = Ci.nsICookieService.BEHAVIOR_REJECT_TRACKER;
const BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN =
  Ci.nsICookieService.BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN;

let originalRequestLongerTimeout = requestLongerTimeout;
// eslint-disable-next-line no-global-assign
requestLongerTimeout = function AntiTrackingRequestLongerTimeout(factor) {
  let ccovMultiplier = AppConstants.MOZ_CODE_COVERAGE ? 2 : 1;
  let fissionMultiplier = SpecialPowers.useRemoteSubframes ? 2 : 1;
  originalRequestLongerTimeout(ccovMultiplier * fissionMultiplier * factor);
};

requestLongerTimeout(3);

const { UrlClassifierTestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/UrlClassifierTestUtils.sys.mjs"
);

const { PermissionTestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/PermissionTestUtils.sys.mjs"
);

const { RemoteSettings } = ChromeUtils.importESModule(
  "resource://services-settings/remote-settings.sys.mjs"
);

Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/toolkit/components/antitracking/test/browser/antitracking_head.js",
  this
);

Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/toolkit/components/antitracking/test/browser/partitionedstorage_head.js",
  this
);

function setCookieBehaviorPref(cookieBehavior, runInPrivateWindow) {
  let cbRegular;
  let cbPrivate;

  // Set different cookieBehaviors to regular mode and private mode so that we
  // can make sure these two prefs don't interfere with each other for all
  // tests.
  if (runInPrivateWindow) {
    cbPrivate = cookieBehavior;

    let defaultPrefBranch = Services.prefs.getDefaultBranch("");
    // In order to test the default private cookieBehavior pref, we need to set
    // the regular pref to the default value because we don't want the private
    // pref to mirror the regular pref in this case.
    //
    // Note that the private pref will mirror the regular pref if the private
    // pref is in default value and the regular pref is not in default value.
    if (
      cookieBehavior ==
      defaultPrefBranch.getIntPref("network.cookie.cookieBehavior.pbmode")
    ) {
      cbRegular = defaultPrefBranch.getIntPref("network.cookie.cookieBehavior");
    } else {
      cbRegular =
        cookieBehavior == BEHAVIOR_ACCEPT ? BEHAVIOR_REJECT : BEHAVIOR_ACCEPT;
    }
  } else {
    cbRegular = cookieBehavior;
    cbPrivate =
      cookieBehavior == BEHAVIOR_ACCEPT ? BEHAVIOR_REJECT : BEHAVIOR_ACCEPT;
  }

  return SpecialPowers.pushPrefEnv({
    set: [
      ["network.cookie.cookieBehavior", cbRegular],
      ["network.cookie.cookieBehavior.pbmode", cbPrivate],
    ],
  });
}

/**
 * Wait for the exception list service to initialize.
 * @param {string} [urlPattern] - The URL pattern to wait for to be present.
 * Pass null to wait for all entries to be removed.
 */
async function waitForExceptionListServiceSynced(urlPattern) {
  info(
    `Waiting for the exception list service to initialize for ${urlPattern}`
  );
  let classifier = Cc["@mozilla.org/url-classifier/dbservice;1"].getService(
    Ci.nsIURIClassifier
  );
  let feature = classifier.getFeatureByName("tracking-protection");
  await TestUtils.waitForCondition(() => {
    if (urlPattern == null) {
      return feature.exceptionList.testGetEntries().length === 0;
    }
    return feature.exceptionList
      .testGetEntries()
      .some(entry => entry.urlPattern === urlPattern);
  }, "Exception list service initialized");
}

/**
 * Wait for a content blocking event to occur.
 * @param {Window} win - The window to listen for the event on.
 * @returns {Promise} A promise that resolves when the event occurs.
 */
async function waitForContentBlockingEvent(win) {
  return new Promise(resolve => {
    let listener = {
      onContentBlockingEvent(webProgress, request, event) {
        if (event & Ci.nsIWebProgressListener.STATE_BLOCKED_TRACKING_CONTENT) {
          win.gBrowser.removeProgressListener(listener);
          resolve();
        }
      },
    };
    win.gBrowser.addProgressListener(listener);
  });
}

/**
 * Dispatch a RemoteSettings "sync" event.
 * @param {string} collectionName - The remote setting collection name
 * @param {Object} data - The event's data payload.
 * @param {Object} [data.created] - Records that were created.
 * @param {Object} [data.updated] - Records that were updated.
 * @param {Object} [data.deleted] - Records that were removed.
 * @param {Object} [data.current] - The current list of records.
 */
async function remoteSettingsSync(
  collectionName,
  { created, updated, deleted, current }
) {
  await RemoteSettings(collectionName).emit("sync", {
    data: {
      created,
      updated,
      deleted,
      // The list service seems to require this field to be set.
      current,
    },
  });
}

/**
 * Set exceptions via RemoteSettings.
 * @param {Object[]} entries - The entries to set. If empty, the exceptions will be cleared.
 * @param {Object} db - The Remote Settings collections database.
 * @param {Object} collectionName The remote setting collection name
 */
async function setExceptions(entries, db, collectionName) {
  info("Set exceptions via RemoteSettings");
  if (!entries.length) {
    await db.clear();
    await db.importChanges({}, Date.now());
    await remoteSettingsSync(collectionName, { current: [] });
    await waitForExceptionListServiceSynced();
    return;
  }

  let entriesPromises = entries.map(e =>
    db.create({
      category: e.category,
      urlPattern: e.urlPattern,
      classifierFeatures: e.classifierFeatures,
      // Only apply to private browsing in ETP "standard" mode.
      isPrivateBrowsingOnly: e.isPrivateBrowsingOnly,
      filterContentBlockingCategories: e.filterContentBlockingCategories,
    })
  );

  let rsEntries = await Promise.all(entriesPromises);

  await db.importChanges({}, Date.now());
  await remoteSettingsSync(collectionName, { current: rsEntries });
  await waitForExceptionListServiceSynced(rsEntries[0].urlPattern);
}
