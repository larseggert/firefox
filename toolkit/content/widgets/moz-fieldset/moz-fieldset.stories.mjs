/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { html } from "../vendor/lit.all.mjs";
// eslint-disable-next-line import/no-unassigned-import
import "./moz-fieldset.mjs";
// eslint-disable-next-line import/no-unassigned-import
import "../moz-toggle/moz-toggle.mjs";

export default {
  title: "UI Widgets/Fieldset",
  component: "moz-fieldset",
  argTypes: {
    headingLevel: {
      options: ["", "1", "2", "3", "4", "5", "6"],
      control: { type: "select" },
    },
  },
  parameters: {
    status: "in-development",
    fluent: `
moz-fieldset-label =
  .label = Related Settings
moz-fieldset-description =
  .label = Some Settings
  .description = Perhaps you want to have a longer description of what these settings do. Width is set explicitly for emphasis.
  `,
  },
};

const Template = ({
  label,
  description,
  l10nId,
  supportPage,
  hasSlottedSupportLinks,
  headingLevel,
}) => html`
  <moz-fieldset
    data-l10n-id=${l10nId}
    .label=${label}
    .description=${description}
    .headingLevel=${headingLevel}
    support-page=${supportPage}
    style="width: 400px;"
  >
    <moz-toggle
      pressed
      label="First setting"
      description="These could have descriptions too."
    ></moz-toggle>
    <moz-checkbox label="Second setting"></moz-checkbox>
    <moz-checkbox
      label="Third setting"
      description="Checkbox with a description."
      support-page="foo"
    ></moz-checkbox>
    <moz-select label="Make a choice">
      <moz-option label="Option One" value="1"></moz-option>
      <moz-option label="Option A" value="a"></moz-option>
    </moz-select>
    ${hasSlottedSupportLinks
      ? html`<a slot="support-link" href="www.example.com"> Click me! </a>`
      : ""}
  </moz-fieldset>
`;

export const Default = Template.bind({});
Default.args = {
  label: "",
  description: "",
  supportPage: "",
  l10nId: "moz-fieldset-label",
  hasSlottedSupportLinks: false,
};

export const WithDescription = Template.bind({});
WithDescription.args = {
  ...Default.args,
  l10nId: "moz-fieldset-description",
};

export const WithSupportLink = Template.bind({});
WithSupportLink.args = {
  ...Default.args,
  supportPage: "test",
};

export const WithDescriptionAndSupportLink = Template.bind({});
WithDescriptionAndSupportLink.args = {
  ...WithSupportLink.args,
  l10nId: "moz-fieldset-description",
};

export const WithSlottedSupportLink = Template.bind({});
WithSlottedSupportLink.args = {
  ...Default.args,
  hasSlottedSupportLinks: true,
};

export const WithDescriptionAndSlottedSupportLink = Template.bind({});
WithDescriptionAndSlottedSupportLink.args = {
  ...WithDescription.args,
  hasSlottedSupportLinks: true,
};

export const WithHeadingLegend = Template.bind({});
WithHeadingLegend.args = {
  ...WithDescription.args,
  headingLevel: "2",
};
