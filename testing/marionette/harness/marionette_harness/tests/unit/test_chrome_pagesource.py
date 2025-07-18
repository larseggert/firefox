# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import os
import sys

from marionette_harness import MarionetteTestCase, WindowManagerMixin

# add this directory to the path
sys.path.append(os.path.dirname(__file__))

from chrome_handler_mixin import ChromeHandlerMixin


class TestPageSourceChrome(ChromeHandlerMixin, WindowManagerMixin, MarionetteTestCase):
    def setUp(self):
        super(TestPageSourceChrome, self).setUp()
        self.marionette.set_context("chrome")

        new_window = self.open_chrome_window(self.chrome_base_url + "test_xul.xhtml")
        self.marionette.switch_to_window(new_window)

    def tearDown(self):
        self.close_all_windows()
        super(TestPageSourceChrome, self).tearDown()

    def testShouldReturnXULDetails(self):
        source = self.marionette.page_source
        self.assertIn('<checkbox id="testBox" label="box"', source)
