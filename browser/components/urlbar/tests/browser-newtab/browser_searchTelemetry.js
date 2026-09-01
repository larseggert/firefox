/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

// A search from the newtab address bar is a search issued from the newtab visit
// the bar sits in, so it counts against that visit's id.

"use strict";

ChromeUtils.defineESModuleGetters(this, {
  AboutNewTab: "resource:///modules/AboutNewTab.sys.mjs",
});

add_setup(async function () {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.urlbar.suggest.searches", false],
      ["browser.newtabpage.activity-stream.telemetry", true],
    ],
  });
  await SearchTestUtils.installSearchExtension({}, { setAsDefault: true });
  Services.fog.testResetFOG();
  registerCleanupFunction(() => Services.fog.testResetFOG());
});

add_task(async function searchIssued() {
  let tab = await NewtabSearchbarTestUtils.openNewTabPage();
  let browser = tab.linkedBrowser;
  let visitId = await TestUtils.waitForCondition(
    () => AboutNewTab.getVisitId(browser),
    "Waiting for the page's newtab visit id"
  );

  let pingSubmitted = false;
  GleanPings.newtab.testBeforeNextSubmit(() => {
    pingSubmitted = true;
    let records = Glean.newtabSearch.issued.testGetValue("newtab");
    Assert.equal(records?.length, 1, "One search was issued");
    Assert.deepEqual(
      records[0].extra,
      {
        newtab_visit_id: visitId,
        search_access_point: "newtab_searchbar",
        telemetry_id: "other-Example",
      },
      "The search was recorded against the newtab visit"
    );
  });

  let loaded = BrowserTestUtils.browserLoaded(browser);
  await NewtabSearchbarTestUtils.promiseAutocompleteResultPopup({
    browser,
    value: "hello",
  });
  await BrowserTestUtils.synthesizeKey("KEY_Enter", {}, browser);
  await loaded;

  await TestUtils.waitForCondition(
    () => pingSubmitted,
    "Waiting for the newtab ping carrying the search"
  );

  BrowserTestUtils.removeTab(tab);
});
