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

import json
import os
import subprocess
import unittest
from pathlib import Path


def benchmark_path():
    runfiles = os.environ.get("RUNFILES_DIR")
    workspace = os.environ.get("TEST_WORKSPACE")
    if runfiles and workspace:
        return Path(runfiles, workspace, "tests/perf_test/runtime_metrics_pubsub_benchmark")
    return Path(__file__).resolve().with_name("runtime_metrics_pubsub_benchmark")


class RuntimeMetricsPubsubBenchmarkTest(unittest.TestCase):
    def test_node_pubsub_modes(self):
        for mode in ("off", "basic", "detailed"):
            with self.subTest(mode=mode):
                run = subprocess.run(
                    [str(benchmark_path()), mode, "10", "50", "128", "100"],
                    capture_output=True,
                    text=True,
                    timeout=35,
                    check=False,
                )
                self.assertEqual(run.returncode, 0, run.stderr)
                report = json.loads(run.stdout)
                self.assertEqual(report["mode"], mode)
                self.assertEqual(report["messages"], 50)
                self.assertEqual(report["payload_bytes"], 128)
                self.assertEqual(report["rate_hz"], 100)
                self.assertEqual(report["received"], 50)
                for field in ("elapsed_ns", "cpu_ns", "p99_ns", "max_rss_kb"):
                    self.assertGreater(report[field], 0)
                self.assertGreater(report["send_elapsed_ns"], 0)
                self.assertGreaterEqual(report["drain_elapsed_ns"], 0)
                self.assertEqual(
                    report["elapsed_ns"],
                    report["send_elapsed_ns"] + report["drain_elapsed_ns"],
                )
                if mode == "off":
                    for field in ("publish_count", "receive_count",
                                  "callback_count", "live_series"):
                        self.assertEqual(report[field], 0)
                else:
                    self.assertEqual(report["publish_count"], 60)
                    self.assertGreater(report["receive_count"], 0)
                    self.assertGreater(report["callback_count"], 0)
                    self.assertGreaterEqual(report["live_series"], 3)

    def test_invalid_mode_fails(self):
        run = subprocess.run(
            [str(benchmark_path()), "invalid", "10", "50", "128", "100"],
            capture_output=True, text=True, timeout=10, check=False,
        )
        self.assertNotEqual(run.returncode, 0)
        self.assertTrue(run.stderr)


if __name__ == "__main__":
    unittest.main()
