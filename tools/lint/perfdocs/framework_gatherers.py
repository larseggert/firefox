# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
import json
import os
import pathlib
import re

from gecko_taskgraph.util.attributes import match_run_on_projects
from manifestparser import TestManifest
from mozperftest.script import ScriptInfo

from perfdocs.doc_helpers import TableBuilder
from perfdocs.logger import PerfDocLogger
from perfdocs.utils import read_yaml

logger = PerfDocLogger()

BRANCHES = [
    "mozilla-central",
    "autoland",
    "mozilla-release",
    "mozilla-beta",
]

"""
This file is for framework specific gatherers since manifests
might be parsed differently in each of them. The gatherers
must implement the FrameworkGatherer class.
"""


class FrameworkGatherer:
    """
    Abstract class for framework gatherers.
    """

    def __init__(self, yaml_path, workspace_dir, taskgraph={}):
        """
        Generic initialization for a framework gatherer.
        """
        self.workspace_dir = workspace_dir
        self._yaml_path = yaml_path
        self._taskgraph = taskgraph
        self._suite_list = {}
        self._test_list = {}
        self._descriptions = {}
        self._manifest_path = ""
        self._manifest = None
        self.script_infos = {}
        self._task_list = {}
        self._task_match_pattern = re.compile(r"([\w\W]*/[pgo|opt]*)-([\w\W]*)")

    def _build_section_with_header(self, title, content, header_type=None):
        """
        Adds a section to the documentation with the title as the type mentioned
        and paragraph as content mentioned.
        :param title: title of the section
        :param content: content of section paragraph
        :param header_type: type of the title heading
        """
        heading_map = {"H2": "*", "H3": "=", "H4": "-", "H5": "^"}
        return [title, heading_map.get(header_type, "^") * len(title), content, ""]

    def _get_metric_heading(self, metric, metrics_info):
        """
        Gets the heading of a specific metric.

        :param str metric: The metric to search for.
        :param dict metrics_info: The information of all the
            metrics that were documented.
        :return str: The heading to use for the given metric.
        """
        for metric_heading, metric_info in metrics_info.items():
            if metric == metric_heading or any(
                metric == alias for alias in metric_info.get("aliases", [])
            ):
                return metric_heading
            if metric_info.get("matcher"):
                match = re.search(metric_info["matcher"], metric)
                if match:
                    return metric_heading

        raise Exception(f"Could not find a metric heading for `{metric}`")

    def get_task_match(self, task_name):
        return re.search(self._task_match_pattern, task_name)

    def get_manifest_path(self):
        """
        Returns the path to the manifest based on the
        manifest entry in the frameworks YAML configuration
        file.

        :return str: Path to the manifest.
        """
        if self._manifest_path:
            return self._manifest_path

        yaml_content = read_yaml(self._yaml_path)
        self._manifest_path = pathlib.Path(self.workspace_dir, yaml_content["manifest"])
        return self._manifest_path

    def get_suite_list(self):
        """
        Each framework gatherer must return a dictionary with
        the following structure. Note that the test names must
        be relative paths so that issues can be correctly issued
        by the reviewbot.

        :return dict: A dictionary with the following structure: {
                "suite_name": [
                    'testing/raptor/test1',
                    'testing/raptor/test2'
                ]
            }
        """
        raise NotImplementedError

    def build_metrics_documentation(self, yaml_content):
        """
        Each framework that provides a page with descriptions about the
        metrics it produces must implement this method. The metrics defined
        for the framework can be found in the `yaml_content` variable.

        The framework gatherer is expected to produce the full documentation
        for all the metrics defined in the yaml_content at once. This is done
        to allow differentiation between how metrics are displayed between
        the different frameworks.

        :param dict yaml_content: A dictionary of the YAML config file for
            the specific framework.
        :return list: A list of all the lines being added to the metrics
            documentation.
        """
        raise NotImplementedError

    def build_command_to_run_locally(self, framework_command, title):
        """
        Each framework has specifics to running it locally. This command
        passes arguments to this function to ensure we can construct those
        commands consistently, and return it so it can be in the mozilla source docs.

        :param str framework_command: A string that has the framework specific
            commands needed to run tests
        :param str title: A string of the test name, added on after the framework
            specific commands (see above framework_command param:)
        :return str: Returns the command to run locally, this output is added to
            the mozilla source docs, and is formatted
        """
        command_to_run_locally = "   * Command to Run Locally\n\n"
        command_to_run_locally += "   .. code-block::\n\n"
        command_to_run_locally += f"      ./mach {framework_command} {title}\n\n"
        return command_to_run_locally


class RaptorGatherer(FrameworkGatherer):
    """
    Gatherer for the Raptor framework.
    """

    def get_suite_list(self):
        """
        Returns a dictionary containing a mapping from suites
        to the tests they contain.

        :return dict: A dictionary with the following structure: {
                "suite_name": [
                    'testing/raptor/test1',
                    'testing/raptor/test2'
                ]
            }
        """
        if self._suite_list:
            return self._suite_list

        manifest_path = self.get_manifest_path()

        # Get the tests from the manifest
        test_manifest = TestManifest([str(manifest_path)], strict=False)
        test_list = test_manifest.active_tests(exists=False, disabled=False)

        # Parse the tests into the expected dictionary
        for test in test_list:
            # Get the top-level suite
            s = os.path.basename(test["here"])
            if s not in self._suite_list:
                self._suite_list[s] = []

            # Get the individual test
            fpath = re.sub(".*testing", "testing", test["manifest"])

            if fpath not in self._suite_list[s]:
                self._suite_list[s].append(fpath)

        return self._suite_list

    def _get_ci_tasks(self):
        for task in self._taskgraph.keys():
            if type(self._taskgraph[task]) is dict:
                command = self._taskgraph[task]["task"]["payload"].get("command", [])
                run_on_projects = self._taskgraph[task]["attributes"]["run_on_projects"]
            else:
                command = self._taskgraph[task].task["payload"].get("command", [])
                run_on_projects = self._taskgraph[task].attributes["run_on_projects"]

            test_match = re.search(r"[\s']--test[\s=](.+?)[\s']", str(command))
            task_match = self.get_task_match(task)
            if test_match and task_match:
                test = test_match.group(1)
                platform = task_match.group(1)
                test_name = task_match.group(2)

                item = {"test_name": test_name, "run_on_projects": run_on_projects}
                self._task_list.setdefault(test, {}).setdefault(platform, []).append(
                    item
                )

    def _get_subtests_from_ini(self, manifest_path, suite_name):
        """
        Returns a list of (sub)tests from an ini file containing the test definitions.

        :param str manifest_path: path to the ini file
        :return list: the list of the tests
        """
        desc_exclusion = ["here", "manifest_relpath", "path", "relpath"]
        test_manifest = TestManifest(
            [str(manifest_path)], strict=False, document=True, add_line_no=True
        )
        test_list = test_manifest.active_tests(exists=False, disabled=False)
        subtests = {}
        for subtest in test_list:
            subtests[subtest["name"]] = subtest["manifest"]

            description = {}
            for key, value in subtest.items():
                if key not in desc_exclusion:
                    description[key] = value

            # Add searchfox link
            key = list(test_manifest.source_documents.keys())[0]

            if (
                test_manifest.source_documents[key]
                and subtest["name"] in test_manifest.source_documents[key].keys()
            ):
                description["link searchfox"] = (
                    "https://searchfox.org/mozilla-central/source/"
                    + pathlib.Path(manifest_path).as_posix()
                    + "#"
                    + test_manifest.source_documents[key][subtest["name"]]["lineno"]
                )

            # Prepare alerting metrics for verification
            description["metrics"] = [
                metric.strip()
                for metric in description.get("alert_on", "").split(",")
                if metric.strip() != ""
            ]
            if (
                description.get("gather_cpuTime", None)
                or "cpuTime" in description.get("measure", [])
                or suite_name in ["desktop", "interactive", "mobile"]
            ):
                description["metrics"].append("cpuTime")

            subtests[subtest["name"]] = description
            self._descriptions.setdefault(suite_name, []).append(description)

        self._descriptions[suite_name].sort(key=lambda item: item["name"])

        return subtests

    def _get_metric_heading(self, metric, metrics_info):
        """
        Finds, and returns the correct heading for a metric to target in a reference link.

        :param str metric: The metric to search for.
        :param dict metrics_info: The information of all the
            metrics that were documented.
        :return str: A formatted string containing the reference link to the
            documented metric.
        """
        metric_heading = super(RaptorGatherer, self)._get_metric_heading(
            metric, metrics_info
        )
        return f"`{metric} <raptor-metrics.html#{metric_heading.lower().replace(' ', '-')}>`__"

    def get_test_list(self):
        """
        Returns a dictionary containing the tests in every suite ini file.

        :return dict: A dictionary with the following structure: {
                "suite_name": {
                    'raptor_test1',
                    'raptor_test2'
                },
            }
        """
        if self._test_list:
            return self._test_list

        suite_list = self.get_suite_list()

        # Iterate over each manifest path from suite_list[suite_name]
        # and place the subtests into self._test_list under the same key
        for suite_name, manifest_paths in suite_list.items():
            if not self._test_list.get(suite_name):
                self._test_list[suite_name] = {}
            for manifest_path in manifest_paths:
                subtest_list = self._get_subtests_from_ini(manifest_path, suite_name)
                self._test_list[suite_name].update(subtest_list)

        self._get_ci_tasks()

        return self._test_list

    def build_test_description(
        self, title, test_description="", suite_name="", metrics_info=None
    ):
        matcher = []
        browsers = [
            "firefox",
            "chrome",
            "refbrow",
            "fennec68",
            "geckoview",
            "fenix",
        ]
        test_name = [f"{title}-{browser}" for browser in browsers]
        test_name.append(title)

        for suite, val in self._descriptions.items():
            for test in val:
                if test["name"] in test_name and suite_name == suite:
                    matcher.append(test)

        if len(matcher) == 0:
            logger.critical(
                "No tests exist for the following name "
                f"(obtained from config.yml): {title}"
            )
            raise Exception(
                "No tests exist for the following name "
                f"(obtained from config.yml): {title}"
            )

        result = f".. dropdown:: {title}\n"
        result += f"   :class-container: anchor-id-{title}-{suite_name[0]}\n\n"
        result += self.build_command_to_run_locally("raptor -t", title)

        for idx, description in enumerate(matcher):
            if description["name"] != title:
                result += f"   {idx+1}. **{description['name']}**\n\n"
            if "owner" in description.keys():
                result += f"   **Owner**: {description['owner']}\n\n"
            if test_description:
                result += f"   **Description**: {test_description}\n\n"

            for key in sorted(description.keys()):
                if key in ["owner", "name", "manifest", "metrics"]:
                    continue
                sub_title = key.replace("_", " ")
                if key == "test_url":
                    if "<" in description[key] or ">" in description[key]:
                        description[key] = description[key].replace("<", r"\<")
                        description[key] = description[key].replace(">", r"\>")
                    result += f"   * **{sub_title}**: `<{description[key]}>`__\n"
                elif key == "secondary_url":
                    result += f"   * **{sub_title}**: `<{description[key]}>`__\n"
                elif key == "link searchfox":
                    result += f"   * **{sub_title}**: `<{description[key]}>`__\n"
                elif key in ["playback_pageset_manifest"]:
                    result += (
                        f"   * **{sub_title}**: "
                        f"{description[key].replace('{subtest}', description['name'])}\n"
                    )
                elif key == "alert_on":
                    result += (
                        f"   * **{sub_title}**: "
                        + ", ".join(
                            self._get_metric_heading(metric.strip(), metrics_info)
                            for metric in description[key]
                            .replace("\n", " ")
                            .replace(",", " ")
                            .split()
                        )
                        + "\n"
                    )
                else:
                    if "\n" in description[key]:
                        description[key] = description[key].replace("\n", " ")
                    result += f"   * **{sub_title}**: {description[key]}\n"

            if self._task_list.get(title, []):
                result += "   * **Test Task**:\n\n"
                for platform in sorted(self._task_list[title]):
                    if (suite_name == "mobile" and "android" not in platform) or (
                        suite_name == "desktop" and "android" in platform
                    ):
                        continue
                    self._task_list[title][platform].sort(key=lambda x: x["test_name"])

                    table = TableBuilder(
                        title=platform,
                        widths=[30] + [15 for x in BRANCHES],
                        header_rows=1,
                        headers=[["Test Name"] + BRANCHES],
                        indent=3,
                    )

                    for task in self._task_list[title][platform]:
                        values = [task["test_name"]]
                        values += [
                            (
                                "\u2705"
                                if match_run_on_projects(x, task["run_on_projects"])
                                else "\u274C"
                            )
                            for x in BRANCHES
                        ]
                        table.add_row(values)
                    result += f"{table.finish_table()}\n"

        return [result]

    def build_suite_section(self, title, content):
        return self._build_section_with_header(
            title.capitalize(), content, header_type="H4"
        )

    def build_metrics_documentation(self, parsed_metrics):
        metrics_documentation = []
        for metric, metric_info in sorted(
            parsed_metrics.items(), key=lambda item: item[0]
        ):
            metric_content = metric_info["description"] + "\n\n"

            metric_content += (
                f"  * **Aliases**: {', '.join(sorted(metric_info['aliases']))}\n"
            )
            if metric_info.get("location", None):
                metric_content += "  * **Tests using it**:\n"

                for suite, tests in sorted(
                    metric_info["location"].items(), key=lambda item: item[0]
                ):
                    metric_content += f"     * **{suite.capitalize()}**: "

                    test_links = []
                    for test in sorted(tests):
                        test_links.append(
                            f"`{test} <raptor.html#{test}-{suite.lower()[0]}>`__"
                        )

                    metric_content += ", ".join(test_links) + "\n"

            metrics_documentation.extend(
                self._build_section_with_header(
                    metric, metric_content, header_type="H3"
                )
            )

        return metrics_documentation


class MozperftestGatherer(FrameworkGatherer):
    """
    Gatherer for the Mozperftest framework.
    """

    def get_test_list(self):
        """
        Returns a dictionary containing the tests that are in perftest.toml manifest.

        :return dict: A dictionary with the following structure: {
                "suite_name": {
                    'perftest_test1',
                    'perftest_test2',
                },
            }
        """
        for path in list(pathlib.Path(self.workspace_dir).rglob("perftest.toml")):
            if "obj-" in str(path) or "objdir-" in str(path):
                continue
            suite_name = str(path.parent).replace(str(self.workspace_dir), "")

            # If the workspace dir doesn't end with a forward-slash,
            # the substitution above won't work completely
            if suite_name.startswith("/") or suite_name.startswith("\\"):
                suite_name = suite_name[1:]

            # We have to add new paths to the logger as we search
            # because mozperftest tests exist in multiple places in-tree
            PerfDocLogger.PATHS.append(suite_name)

            # Get the tests from perftest.toml
            test_manifest = TestManifest([str(path)], strict=False)
            test_list = test_manifest.active_tests(exists=False, disabled=True)
            for test in test_list:
                si = ScriptInfo(test["path"])
                if si["name"].endswith(".js"):
                    cleaned_name = si["name"]
                else:
                    cleaned_name = si["name"].replace(".", "")

                self.script_infos[cleaned_name] = si
                self._test_list.setdefault(suite_name.replace("\\", "/"), {}).update(
                    {cleaned_name: {"path": str(path)}}
                )

        return self._test_list

    def build_test_description(
        self, title, test_description="", suite_name="", metrics_info=None
    ):
        return [str(self.script_infos[title])]

    def build_suite_section(self, title, content):
        return self._build_section_with_header(title, content, header_type="H4")


class TalosGatherer(FrameworkGatherer):
    def _get_ci_tasks(self):
        with open(
            pathlib.Path(self.workspace_dir, "testing", "talos", "talos.json")
        ) as f:
            config_suites = json.load(f)["suites"]

        for task_name in self._taskgraph.keys():
            task = self._taskgraph[task_name]

            if type(task) is dict:
                is_talos = task["task"]["extra"].get("suite", [])
                command = task["task"]["payload"].get("command", [])
                run_on_projects = task["attributes"]["run_on_projects"]
            else:
                is_talos = task.task["extra"].get("suite", [])
                command = task.task["payload"].get("command", [])
                run_on_projects = task.attributes["run_on_projects"]

            suite_match = re.search(r"[\s']--suite[\s=](.+?)[\s']", str(command))
            task_match = self.get_task_match(task_name)
            if "talos" == is_talos and task_match:
                suite = suite_match.group(1)
                platform = task_match.group(1)
                test_name = task_match.group(2)
                item = {"test_name": test_name, "run_on_projects": run_on_projects}

                for test in config_suites[suite]["tests"]:
                    self._task_list.setdefault(test, {}).setdefault(
                        platform, []
                    ).append(item)

    def get_test_list(self):
        from talos import test as talos_test

        test_lists = talos_test.test_dict()
        mod = __import__("talos.test", fromlist=test_lists)

        suite_name = "Talos Tests"

        for test in test_lists:
            self._test_list.setdefault(suite_name, {}).update({test: {}})

            klass = getattr(mod, test)
            self._descriptions.setdefault(test, klass.__dict__)

        self._get_ci_tasks()

        return self._test_list

    def build_test_description(
        self, title, test_description="", suite_name="", metrics_info=None
    ):
        result = f".. dropdown:: {title}\n"
        result += f"   :class-container: anchor-id-{title}\n\n"
        result += self.build_command_to_run_locally("talos-test -a", title)

        yml_descriptions = [s.strip() for s in test_description.split("- ") if s]
        for description in yml_descriptions:
            if "Example Data" in description:
                # Example Data for using code block
                example_list = [s.strip() for s in description.split("* ")]
                result += f"   * {example_list[0]}\n"
                result += "\n   .. code-block::\n\n"
                for example in example_list[1:]:
                    result += f"      {example}\n"
                result += "\n"

            elif "    * " in description:
                # Sub List
                sub_list = [s.strip() for s in description.split(" * ")]
                result += f"   * {sub_list[0]}\n"
                for sub in sub_list[1:]:
                    result += f"      * {sub}\n"

            else:
                # General List
                result += f"   * {description}\n"

        if title in self._descriptions:
            for key in sorted(self._descriptions[title]):
                if key.startswith("__") and key.endswith("__"):
                    continue
                elif key == "filters":
                    continue

                # On windows, we get the paths in the wrong style
                value = self._descriptions[title][key]
                if isinstance(value, dict):
                    for k, v in value.items():
                        if isinstance(v, str) and "\\" in v:
                            value[k] = str(v).replace("\\", r"/")
                result += r"   * " + key + r": " + str(value) + r"\n"

        if self._task_list.get(title, []):
            result += "   * **Test Task**:\n\n"
            for platform in sorted(self._task_list[title]):
                self._task_list[title][platform].sort(key=lambda x: x["test_name"])

                table = TableBuilder(
                    title=platform,
                    widths=[30] + [15 for x in BRANCHES],
                    header_rows=1,
                    headers=[["Test Name"] + BRANCHES],
                    indent=3,
                )

                for task in self._task_list[title][platform]:
                    values = [task["test_name"]]
                    values += [
                        (
                            "\u2705"
                            if match_run_on_projects(x, task["run_on_projects"])
                            else "\u274C"
                        )
                        for x in BRANCHES
                    ]
                    table.add_row(values)
                result += f"{table.finish_table()}\n"

        return [result]

    def build_suite_section(self, title, content):
        return self._build_section_with_header(title, content, header_type="H2")


class AwsyGatherer(FrameworkGatherer):
    """
    Gatherer for the Awsy framework.
    """

    def _generate_ci_tasks(self):
        for task_name in self._taskgraph.keys():
            task = self._taskgraph[task_name]

            if type(task) is dict:
                awsy_test = task["task"]["extra"].get("suite", [])
                run_on_projects = task["attributes"]["run_on_projects"]
            else:
                awsy_test = task.task["extra"].get("suite", [])
                run_on_projects = task.attributes["run_on_projects"]

            task_match = self.get_task_match(task_name)

            if "awsy" in awsy_test and task_match:
                platform = task_match.group(1)
                test_name = task_match.group(2)
                item = {"test_name": test_name, "run_on_projects": run_on_projects}
                self._task_list.setdefault(platform, []).append(item)

    def get_suite_list(self):
        self._suite_list = {"Awsy tests": ["tp6", "base", "dmd", "tp5"]}
        return self._suite_list

    def get_test_list(self):
        self._generate_ci_tasks()
        return {
            "Awsy tests": {
                "tp6": {},
                "base": {},
                "dmd": {},
                "tp5": {},
            }
        }

    def build_suite_section(self, title, content):
        return self._build_section_with_header(
            title.capitalize(), content, header_type="H4"
        )

    def build_test_description(
        self, title, test_description="", suite_name="", metrics_info=None
    ):
        dropdown_suite_name = suite_name.replace(" ", "-")
        result = f".. dropdown:: {title} ({test_description})\n"
        result += f"   :class-container: anchor-id-{title}-{dropdown_suite_name}\n\n"
        result += self.build_command_to_run_locally(
            "awsy-test", "" if title == "tp6" else f"--{title}"
        )

        awsy_data = read_yaml(self._yaml_path)["suites"]["Awsy tests"]
        if "owner" in awsy_data.keys():
            result += f"   **Owner**: {awsy_data['owner']}\n\n"
        result += "   * **Test Task**:\n"

        # tp5 tests are represented by awsy-e10s test names
        # while the others have their title in test names
        search_tag = "awsy-e10s" if title == "tp5" else title
        for platform in sorted(self._task_list.keys()):
            result += f"      * {platform}\n"
            for test_dict in sorted(
                self._task_list[platform], key=lambda d: d["test_name"]
            ):
                if search_tag in test_dict["test_name"]:
                    run_on_project = ": " + (
                        ", ".join(test_dict["run_on_projects"])
                        if test_dict["run_on_projects"]
                        else "None"
                    )
                    result += (
                        f"            * {test_dict['test_name']}{run_on_project}\n"
                    )
            result += "\n"

        return [result]


class StaticGatherer(FrameworkGatherer):
    """
    A noop gatherer for frameworks with static-only documentation.
    """

    pass
