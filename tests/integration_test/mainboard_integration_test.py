#!/usr/bin/env python3

# Copyright 2026 WheelOS. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import subprocess
import unittest


def _runfile(*parts):
    return os.path.join(
        os.environ["TEST_SRCDIR"], os.environ["TEST_WORKSPACE"], *parts
    )


def _run_mainboard(*args):
    return subprocess.run(
        [_runfile("cyber", "mainboard", "mainboard"), *args],
        capture_output=True,
        env={**os.environ, "CYBER_PATH": _runfile("cyber")},
        text=True,
        timeout=10,
    )


class MainboardIntegrationTest(unittest.TestCase):
    def test_missing_dag_option_exits_with_argument_error(self):
        result = _run_mainboard("--process_name", "mainboard-integration-test")

        self.assertEqual(1, result.returncode)
        self.assertIn("-d parameter must be specified", result.stdout + result.stderr)

    def test_missing_dag_file_exits_after_load_failure(self):
        result = _run_mainboard("-d", "mainboard_missing_input.dag")

        self.assertEqual(255, result.returncode)
        self.assertIn("Get proto failed, file:", result.stdout + result.stderr)
        self.assertIn("module start error.", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
