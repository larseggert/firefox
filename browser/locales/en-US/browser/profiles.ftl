# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

profile-window-title-2 = { -brand-short-name } - Choose a profile
profile-window-logo =
    .alt = { -brand-short-name } logo
profile-window-heading = Choose a { -brand-short-name } profile
profile-window-body = Keep your work and personal browsing, including things like passwords and bookmarks, totally separate. Or create profiles for everyone who uses this device.
# This checkbox appears in the Choose profile window that appears when the browser is opened. "Show this" refers to this window, which is displayed when the checkbox is enabled.
profile-window-checkbox-label-2 =
    .label = Choose a profile when { -brand-short-name } opens
# This subcopy appears below the checkbox when it is unchecked
profile-window-checkbox-subcopy = { -brand-short-name } will open to your most recently used profile.
profile-window-create-profile = Create a profile
profile-card-edit-button =
    .title = Edit profile
    .aria-label = Edit profile
profile-card-delete-button =
    .title = Delete profile
    .aria-label = Delete profile

# Variables
#   $profileName (string) - The name of the profile
profile-card =
    .title = Open { $profileName }
    .aria-label = Open { $profileName }

# Variables
#   $number (number) - The number of the profile
default-profile-name = Profile { $number }

# The word 'original' is used in the sense that it is the initial or starting profile when you install Firefox.
original-profile-name = Original profile

edit-profile-page-title = Edit profile
edit-profile-page-header = Edit your profile
edit-profile-page-profile-name-label = Profile name
edit-profile-page-theme-header-2 =
    .label = Theme
edit-profile-page-explore-themes = Explore more themes
edit-profile-page-avatar-header-2 =
    .label = Avatar
edit-profile-page-delete-button =
    .label = Delete

edit-profile-page-avatar-selector-opener-link = Edit
avatar-selector-icon-tab = Icon
avatar-selector-custom-tab = Custom
avatar-selector-cancel-button =
  .label = Cancel
avatar-selector-save-button =
  .label = Save
avatar-selector-upload-file = Upload a file
avatar-selector-drag-file = Or drag a file here
avatar-selector-add-image = Add an image
avatar-selector-crop = Crop

edit-profile-page-no-name = Name this profile to help you find it later. Rename it any time.
edit-profile-page-duplicate-name = Profile name already in use. Try a new name.

edit-profile-page-profile-saved = Saved

new-profile-page-title = New profile
new-profile-page-header = Customize your new profile
new-profile-page-header-description = Each profile keeps its unique browsing history and settings separate from your other profiles. Plus, { -brand-short-name }’s strong privacy protections are on by default.
new-profile-page-learn-more = Learn more
new-profile-page-input-placeholder =
    .placeholder = Pick a name like “Work” or “Personal”
new-profile-page-done-button =
    .label = Done editing

## Delete profile dialogue that allows users to review what they will lose if they choose to delete their profile. Each item (open windows, etc.) is displayed in a table, followed by a column with the number of items.

# Variables
#   $profilename (String) - The name of the profile.
delete-profile-page-title = Delete { $profilename } profile

# Variables
#   $profilename (String) - The name of the profile.
delete-profile-header = Delete { $profilename } profile?
delete-profile-description = { -brand-short-name } will permanently delete the following data from this device:
# Open is an adjective, as in "browser windows currently open".
delete-profile-windows = Open windows
# Open is an adjective, as in "browser tabs currently open".
delete-profile-tabs = Open tabs
delete-profile-bookmarks = Bookmarks
delete-profile-history = History (visited pages, cookies, site data)
delete-profile-autofill = Autofill data (addresses, payment methods)
delete-profile-logins = Passwords

##

# Button label
delete-profile-cancel = Cancel
# Button label
delete-profile-confirm = Delete

## These strings are color themes available to select from the profile selection screen. Theme names should be localized.

# This light theme features sunny colors such as goldenrod and pale yellow. Its name evokes the color of a marigold flower. This name can be translated directly if it's easily understood in your language, or adapted to a more natural sounding name that fits the color scheme.
profiles-marigold-theme-2 = Marigold yellow

# This light theme features various shades of soft, muted purples. Its name evokes the color of a lavender flower. This name can be translated directly if it's easily understood in your language, or adapted to a more natural sounding name that fits the color scheme.
profiles-lavender-theme-2 = Pale lavender

# This light theme features very pale green tones. Its name evokes the color of pale green mint ice cream. This name can be translated directly if it's easily understood in your language, or adapted to a more natural sounding name that fits the color scheme.
profiles-lichen-theme-2 = Minty green

# This light theme features various shades of pink ranging from pale to bold. Its name evokes the color of a pink magnolia flower. This name can be translated directly if it's easily understood in your language, or adapted to a more natural sounding name that fits the color scheme.
profiles-magnolia-theme-2 = Magnolia pink

# Ocean blue is a dark theme that features very dark blues and black. Its name evokes the color of the deep ocean water. This name can be translated directly if it's easily understood in your language, or adapted to a more natural sounding name that fits the color scheme.
profiles-ocean-theme-2 = Ocean blue

# This dark theme features warm oranges, dark mahogany browns, and earthy red/brown colors. The name evokes the earthy colors of brick masonry. This name can be translated directly if it's easily understood in your language, or adapted to a more natural sounding name that fits the color scheme.
profiles-terracotta-theme-2 = Brick red

# This dark theme features forest green, dusky green with a gray undertone, and a muted sage green. Its name evokes the rich color of green moss in the forest. This name can be translated directly if its easily understood in your language, or adapted to a more natural sounding name that fits the color scheme.
profiles-moss-theme-2 = Moss green

# The default light theme
profiles-light-theme = Light

# The default dark theme
profiles-dark-theme = Dark

# The default system theme
profiles-system-theme = System

## Alternative text for default profile icons

book-avatar-alt =
    .alt = Book
briefcase-avatar-alt =
    .alt = Briefcase
flower-avatar-alt =
    .alt = Flower
heart-avatar-alt =
    .alt = Heart
shopping-avatar-alt =
    .alt = Shopping cart
star-avatar-alt =
    .alt = Star
custom-avatar-alt =
    .alt = Custom avatar

## Labels for default avatar icons

book-avatar = Book
briefcase-avatar = Briefcase
flower-avatar = Flower
heart-avatar = Heart
shopping-avatar = Shopping cart
star-avatar = Star
