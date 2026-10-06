/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.components.menu

import kotlin.test.assertEquals
import kotlin.test.assertSame
import kotlinx.coroutines.flow.MutableStateFlow
import mozilla.components.compose.menu.data.MenuItem
import org.junit.Test
import org.mozilla.fenix.components.menu.FenixMenuItem.CustomizeReaderView
import org.mozilla.fenix.components.menu.FenixMenuItem.FindInPage
import org.mozilla.fenix.components.menu.FenixMenuItem.More
import org.mozilla.fenix.components.menu.FenixMenuItem.Settings
import org.mozilla.fenix.components.menu.MenuPresentationMode.Row

class MenuItemsRegistryTest {
    @Test
    fun `GIVEN menu items configured more than once WHEN building the providers THEN build a single one for each menu item`() {
        val more = More(subMenuItems = listOf(FindInPage, Settings, FindInPage))
        val resolved = mutableListOf<FenixMenuItem>()

        MenuItemsRegistry(
            configuration =
                listOf(
                    sectionOf(CustomizeReaderView, more, FindInPage),
                    sectionOf(Settings, more, CustomizeReaderView),
                ),
            resolver = { item -> FakeMenuItemProvider().also { resolved += item } },
        )

        assertEquals(listOf(CustomizeReaderView, more, FindInPage, Settings), resolved)
    }

    @Test
    fun `WHEN asking for the provider of an item THEN always offer the one configured for it`() {
        val registry =
            MenuItemsRegistry(
                configuration = listOf(sectionOf(FindInPage)),
                resolver = { FakeMenuItemProvider() },
            )

        assertSame(registry.providers.getValue(FindInPage), registry[FindInPage])
        assertSame(registry[FindInPage], registry.get(FindInPage))
    }

    private fun sectionOf(vararg items: FenixMenuItem) =
        MenuSectionConfiguration(id = "section", presentationMode = Row, items = items.toList())

    private class FakeMenuItemProvider : MenuItemProvider {
        override val itemFlow = MutableStateFlow<MenuItem?>(null)
    }
}
