/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.components.menu.middleware

import mozilla.components.lib.state.Middleware
import mozilla.components.lib.state.MiddlewareContext
import mozilla.telemetry.glean.private.NoExtras
import org.mozilla.fenix.GleanMetrics.AppMenu
import org.mozilla.fenix.GleanMetrics.Events
import org.mozilla.fenix.GleanMetrics.HomeMenu
import org.mozilla.fenix.GleanMetrics.Menu
import org.mozilla.fenix.GleanMetrics.ReaderMode
import org.mozilla.fenix.GleanMetrics.Translations
import org.mozilla.fenix.components.menu.MenuAccessPoint
import org.mozilla.fenix.components.menu.store.MenuAction
import org.mozilla.fenix.components.menu.store.MenuState
import org.mozilla.fenix.components.menu.store.MenuStore

/**
 * A [Middleware] for recording telemetry based on [MenuAction]s that are dispatch to the
 * [MenuStore].
 *
 * @param accessPoint The [MenuAccessPoint] that was used to navigate to the menu dialog.
 */
class MenuTelemetryMiddleware(
    private val accessPoint: MenuAccessPoint,
) : Middleware<MenuState, MenuAction> {

    @Suppress("CyclomaticComplexMethod", "LongMethod")
    override fun invoke(
        context: MiddlewareContext<MenuState, MenuAction>,
        next: (MenuAction) -> Unit,
        action: MenuAction,
    ) {
        val currentState = context.state

        next(action)

        when (action) {
            MenuAction.AddBookmark -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "add_bookmark",
                ),
            )

            MenuAction.Navigate.EditBookmark -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "edit_bookmark",
                ),
            )

            MenuAction.AddShortcut -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "add_to_top_sites",
                ),
            )

            MenuAction.RemoveShortcut -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "remove_from_top_sites",
                ),
            )

            MenuAction.Navigate.AddToHomeScreen -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "add_to_homescreen",
                ),
            )

            MenuAction.Navigate.Bookmarks -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "bookmarks",
                ),
            )

            MenuAction.Navigate.CustomizeHomepage -> AppMenu.customizeHomepage.record(NoExtras())

            MenuAction.Navigate.Downloads -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "downloads",
                ),
            )

            MenuAction.Navigate.History -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "history",
                ),
            )

            MenuAction.Navigate.ManageExtensions -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "addons_manager",
                ),
            )

            is MenuAction.Navigate.MozillaAccount -> {
                Events.browserMenuAction.record(Events.BrowserMenuActionExtra(item = "sync_account"))
                AppMenu.signIntoSync.add()
            }

            MenuAction.OpenInApp -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "open_in_app",
                ),
            )

            MenuAction.Navigate.Passwords -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "passwords",
                ),
            )

            MenuAction.Navigate.ReleaseNotes -> Events.whatsNewTapped.record(
                Events.WhatsNewTappedExtra(
                    source = "MENU",
                ),
            )

            MenuAction.Navigate.Settings -> {
                when (accessPoint) {
                    MenuAccessPoint.Browser -> Events.browserMenuAction.record(
                        Events.BrowserMenuActionExtra(
                            item = "settings",
                        ),
                    )

                    MenuAccessPoint.Home -> HomeMenu.settingsItemClicked.record(NoExtras())

                    MenuAccessPoint.External -> Unit
                }
            }

            is MenuAction.Navigate.SaveToCollection -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "save_to_collection",
                ),
            )

            MenuAction.Navigate.Share -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = when (accessPoint) {
                        MenuAccessPoint.External -> "share_custom_tab"
                        else -> "share"
                    },
                ),
            )

            MenuAction.Navigate.Translate -> {
                Translations.action.record(Translations.ActionExtra(item = "main_flow_browser"))

                Events.browserMenuAction.record(
                    Events.BrowserMenuActionExtra(
                        item = "translate",
                    ),
                )
            }

            MenuAction.DeleteBrowsingDataAndQuit -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "quit",
                ),
            )

            MenuAction.FindInPage -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = when (accessPoint) {
                        MenuAccessPoint.External -> "find_in_page_custom_tab"
                        else -> "find_in_page"
                    },
                ),
            )

            is MenuAction.MenuBanner -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "menu_banner",
                ),
            )

            MenuAction.DismissMenuBanner -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "dismiss_menu_banner",
                ),
            )

            MenuAction.CustomizeReaderView -> ReaderMode.appearance.record(NoExtras())

            MenuAction.ToggleReaderView -> {
                val readerState = currentState.browserMenuState?.selectedTab?.readerState ?: return

                if (readerState.active) {
                    ReaderMode.closed.record(NoExtras())
                } else {
                    ReaderMode.opened.record(NoExtras())
                }
            }

            is MenuAction.RequestDesktopSite -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = when (accessPoint) {
                        MenuAccessPoint.External -> "desktop_view_on_custom_tab"
                        else -> "desktop_view_on"
                    },
                ),
            )

            is MenuAction.RequestMobileSite -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = when (accessPoint) {
                        MenuAccessPoint.External -> "desktop_view_off_custom_tab"
                        else -> "desktop_view_off"
                    },
                ),
            )

            MenuAction.OpenInFirefox -> Events.browserMenuAction.record(
                Events.BrowserMenuActionExtra(
                    item = "open_in_fenix",
                ),
            )

            MenuAction.Navigate.DiscoverMoreExtensions -> {
                Events.browserMenuAction.record(
                    Events.BrowserMenuActionExtra(
                        item = "discover_more_extensions",
                    ),
                )
            }

            MenuAction.Navigate.ExtensionsLearnMore -> {
                Events.browserMenuAction.record(
                    Events.BrowserMenuActionExtra(
                        item = "extensions_learn_more",
                    ),
                )
            }

            is MenuAction.Navigate.AddonDetails -> {
                Events.browserMenuAction.record(
                    Events.BrowserMenuActionExtra(
                        item = "addon_details",
                    ),
                )
            }

            is MenuAction.InstallAddon -> {
                Events.browserMenuAction.record(
                    Events.BrowserMenuActionExtra(
                        item = "install_addon",
                    ),
                )
            }

            is MenuAction.Navigate.WebCompatReporter -> {
                Events.browserMenuAction.record(
                    Events.BrowserMenuActionExtra(
                        item = "report_broken_site",
                    ),
                )
            }

            MenuAction.OpenInRegularTab -> {
                Events.browserMenuAction.record(
                    Events.BrowserMenuActionExtra(
                        item = "open_in_regular_tab",
                    ),
                )
            }

            MenuAction.OnCFRShown -> Menu.showCfr.record(NoExtras())

            MenuAction.OnCFRDismiss -> Menu.dismissCfr.record(NoExtras())

            MenuAction.InitAction,
            is MenuAction.CustomMenuItemAction,
            is MenuAction.UpdateBookmarkState,
            is MenuAction.UpdateExtensionState,
            is MenuAction.UpdatePinnedState,
            is MenuAction.UpdateWebExtensionBrowserMenuItems,
            is MenuAction.InstallAddonFailed,
            is MenuAction.InstallAddonSuccess,
            is MenuAction.UpdateInstallAddonInProgress,
            is MenuAction.UpdateShowExtensionsOnboarding,
            is MenuAction.UpdateShowDisabledExtensionsOnboarding,
            is MenuAction.UpdateManageExtensionsMenuItemVisibility,
            is MenuAction.UpdateAvailableAddons,
            is MenuAction.Navigate.Back,
            is MenuAction.Navigate.Forward,
            is MenuAction.Navigate.Reload,
            is MenuAction.Navigate.Stop,
            is MenuAction.Navigate.InstalledAddonDetails,
            -> Unit
        }
    }
}
