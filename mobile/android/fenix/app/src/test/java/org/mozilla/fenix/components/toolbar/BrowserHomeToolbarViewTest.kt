/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.components.toolbar

import android.view.View
import androidx.coordinatorlayout.widget.CoordinatorLayout
import io.mockk.confirmVerified
import io.mockk.every
import io.mockk.mockk
import io.mockk.spyk
import io.mockk.verify
import mozilla.components.browser.toolbar.BrowserToolbar
import mozilla.components.lib.publicsuffixlist.PublicSuffixList
import mozilla.components.support.test.robolectric.testContext
import mozilla.components.ui.widgets.behavior.EngineViewScrollingBehavior
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.mozilla.fenix.R
import org.mozilla.fenix.ext.components
import org.mozilla.fenix.ext.settings
import org.mozilla.fenix.utils.Settings
import org.robolectric.RobolectricTestRunner
import mozilla.components.ui.widgets.behavior.ViewPosition as MozacToolbarPosition

@RunWith(RobolectricTestRunner::class)
class BrowserHomeToolbarViewTest {
    private lateinit var toolbarView: BrowserToolbarView
    private lateinit var toolbar: BrowserToolbar
    private lateinit var behavior: EngineViewScrollingBehavior
    private lateinit var settings: Settings

    @Before
    fun setup() {
        toolbar = BrowserToolbar(testContext).apply {
            id = R.id.toolbar
        }

        settings = mockk(relaxed = true)
        every { testContext.components.useCases } returns mockk(relaxed = true)
        every { testContext.components.core } returns mockk(relaxed = true)
        every { testContext.components.publicSuffixList } returns PublicSuffixList(testContext)
        every { testContext.settings() } returns settings
        toolbarView = BrowserToolbarView(
            context = testContext,
            settings = settings,
            container = CoordinatorLayout(testContext).apply {
                layoutDirection = View.LAYOUT_DIRECTION_RTL
                addView(toolbar, CoordinatorLayout.LayoutParams(100, 100))
            },
            snackbarParent = mockk(),
            interactor = mockk(),
            customTabSession = mockk(relaxed = true),
            lifecycleOwner = mockk(),
            tabStripContent = {},
        )

        toolbarView.toolbar = toolbar
        behavior = spyk(EngineViewScrollingBehavior(testContext, null, MozacToolbarPosition.BOTTOM))
        (toolbarView.layout.layoutParams as CoordinatorLayout.LayoutParams).behavior = behavior
    }

    @Test
    fun `setToolbarBehavior(false) should setDynamicToolbarBehavior if no a11y and bottom toolbar is dynamic`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.BOTTOM
        every { settings.isDynamicToolbarEnabled } returns true
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns false
        every { settings.shouldUseFixedTopToolbar } returns false

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, false)

        verify { toolbarViewSpy.setDynamicToolbarBehavior(MozacToolbarPosition.BOTTOM) }
    }

    @Test
    fun `setToolbarBehavior(false) should expandToolbarAndMakeItFixed if bottom toolbar is not set as dynamic`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.BOTTOM
        every { settings.isDynamicToolbarEnabled } returns false
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns false
        every { settings.shouldUseFixedTopToolbar } returns false

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, false)

        verify { toolbarViewSpy.expandToolbarAndMakeItFixed() }
    }

    @Test
    fun `setToolbarBehavior(false) should setDynamicToolbarBehavior if bottom toolbar is dynamic and the tab is for a PWA or TWA`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.BOTTOM
        every { settings.isDynamicToolbarEnabled } returns true
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns true
        every { settings.shouldUseFixedTopToolbar } returns false

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, false)

        verify { toolbarViewSpy.setDynamicToolbarBehavior(MozacToolbarPosition.BOTTOM) }
    }

    @Test
    fun `setToolbarBehavior(false) should expandToolbarAndMakeItFixed if bottom toolbar is dynamic and a11y is enabled`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.BOTTOM
        every { settings.isDynamicToolbarEnabled } returns true
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns false
        every { settings.shouldUseFixedTopToolbar } returns true

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, false)

        verify { toolbarViewSpy.expandToolbarAndMakeItFixed() }
    }

    @Test
    fun `setToolbarBehavior(true) should expandToolbarAndMakeItFixed bottom toolbar is dynamic and a11y is disabled`() {
        // All intrinsic checks are met but the method was called with `shouldDisableScroll` = true

        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.BOTTOM
        every { settings.isDynamicToolbarEnabled } returns true
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns false
        every { settings.shouldUseFixedTopToolbar } returns false

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, false)

        verify { toolbarViewSpy.setDynamicToolbarBehavior(MozacToolbarPosition.BOTTOM) }
    }

    @Test
    fun `setToolbarBehavior(true) should expandToolbarAndMakeItFixed if bottom toolbar is not set as dynamic`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.BOTTOM
        every { settings.isDynamicToolbarEnabled } returns false
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns false
        every { settings.shouldUseFixedTopToolbar } returns false

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, false)

        verify { toolbarViewSpy.expandToolbarAndMakeItFixed() }
    }

    @Test
    fun `setToolbarBehavior(true) should setDynamicToolbarBehavior if bottom toolbar is dynamic and the tab is for a PWA or TWA`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.BOTTOM
        every { settings.isDynamicToolbarEnabled } returns true
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns true
        every { settings.shouldUseFixedTopToolbar } returns false

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, false)

        verify { toolbarViewSpy.setDynamicToolbarBehavior(MozacToolbarPosition.BOTTOM) }
    }

    @Test
    fun `setToolbarBehavior(true) should expandToolbarAndMakeItFixed if bottom toolbar is dynamic and and a11 is enabled`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.BOTTOM
        every { settings.isDynamicToolbarEnabled } returns true
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns false
        every { settings.shouldUseFixedTopToolbar } returns true

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, false)

        verify { toolbarViewSpy.expandToolbarAndMakeItFixed() }
    }

    @Test
    fun `setToolbarBehavior(true) should expandToolbarAndMakeItFixed for top toolbar if shouldUseFixedTopToolbar`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.TOP
        every { settings.shouldUseFixedTopToolbar } returns true

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, true)

        verify { toolbarViewSpy.expandToolbarAndMakeItFixed() }
    }

    @Test
    fun `setToolbarBehavior(true) should expandToolbarAndMakeItFixed for top toolbar if it is not dynamic`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.TOP
        every { settings.isDynamicToolbarEnabled } returns false

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, true)

        verify { toolbarViewSpy.expandToolbarAndMakeItFixed() }
    }

    @Test
    fun `setToolbarBehavior(true) should expandToolbarAndMakeItFixed for top toolbar if shouldDisableScroll`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.TOP

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, true)

        verify { toolbarViewSpy.expandToolbarAndMakeItFixed() }
    }

    @Test
    fun `setToolbarBehavior(false) should setDynamicToolbarBehavior for top toolbar`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { settings.toolbarPosition } returns ToolbarPosition.TOP
        every { settings.shouldUseFixedTopToolbar } returns true
        every { settings.isDynamicToolbarEnabled } returns true

        toolbarViewSpy.setToolbarBehavior(settings.toolbarPosition, true)

        verify { toolbarViewSpy.expandToolbarAndMakeItFixed() }
    }

    @Test
    fun `expandToolbarAndMakeItFixed should expand the toolbar and and disable the dynamic behavior`() {
        val toolbarViewSpy = spyk(toolbarView)

        assertNotNull((toolbarView.layout.layoutParams as CoordinatorLayout.LayoutParams).behavior)

        toolbarViewSpy.expandToolbarAndMakeItFixed()

        verify { toolbarViewSpy.expand() }
        assertNull((toolbarView.layout.layoutParams as CoordinatorLayout.LayoutParams).behavior)
    }

    @Test
    fun `setDynamicToolbarBehavior should set a ViewHideOnScrollBehavior for the bottom toolbar`() {
        val toolbarViewSpy = spyk(toolbarView)
        (toolbar.layoutParams as CoordinatorLayout.LayoutParams).behavior = null

        toolbarViewSpy.setDynamicToolbarBehavior(MozacToolbarPosition.BOTTOM)

        assertNotNull((toolbarView.layout.layoutParams as CoordinatorLayout.LayoutParams).behavior)
    }

    @Test
    fun `setDynamicToolbarBehavior should set a ViewHideOnScrollBehavior for the top toolbar`() {
        val toolbarViewSpy = spyk(toolbarView)
        (toolbar.layoutParams as CoordinatorLayout.LayoutParams).behavior = null

        toolbarViewSpy.setDynamicToolbarBehavior(MozacToolbarPosition.TOP)

        assertNotNull((toolbarView.layout.layoutParams as CoordinatorLayout.LayoutParams).behavior)
    }

    @Test
    fun `expand should not do anything if isPwaTabOrTwaTab`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns true

        toolbarViewSpy.expand()

        verify { toolbarViewSpy.expand() }
        verify { toolbarViewSpy.isPwaTabOrTwaTab }
        // verify that no other interactions than the expected ones took place
        confirmVerified(toolbarViewSpy)
    }

    @Test
    fun `expand should call forceExpand if not isPwaTabOrTwaTab`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns false

        toolbarViewSpy.expand()

        verify { behavior.forceExpand(toolbarView.layout) }
    }

    @Test
    fun `collapse should not do anything if isPwaTabOrTwaTab`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns true

        toolbarViewSpy.collapse()

        verify { toolbarViewSpy.collapse() }
        verify { toolbarViewSpy.isPwaTabOrTwaTab }
        // verify that no other interactions than the expected ones took place
        confirmVerified(toolbarViewSpy)
    }

    @Test
    fun `collapse should call forceExpand if not isPwaTabOrTwaTab`() {
        val toolbarViewSpy = spyk(toolbarView)
        every { toolbarViewSpy.isPwaTabOrTwaTab } returns false

        toolbarViewSpy.collapse()

        verify { behavior.forceCollapse(toolbarView.layout) }
    }

    @Test
    fun `enable scrolling is forwarded to the toolbar behavior`() {
        toolbarView.enableScrolling()

        verify { behavior.enableScrolling() }
    }

    @Test
    fun `disable scrolling is forwarded to the toolbar behavior`() {
        toolbarView.disableScrolling()

        verify { behavior.disableScrolling() }
    }
}
