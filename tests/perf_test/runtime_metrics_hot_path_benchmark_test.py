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
        return Path(
            runfiles, workspace, "tests/perf_test/runtime_metrics_hot_path_benchmark"
        )
    return Path(__file__).resolve().with_name("runtime_metrics_hot_path_benchmark")


class RuntimeMetricsHotPathBenchmarkTest(unittest.TestCase):
    def test_modes_have_identical_checksum_and_valid_machine_output(self):
        benchmark = benchmark_path()
        reports = {}
        for mode in ("off", "basic", "detailed"):
            run = subprocess.run(
                [str(benchmark), mode, "1000", "5000"],
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(run.returncode, 0, run.stderr)
            report = json.loads(run.stdout)
            self.assertEqual(report["mode"], mode)
            self.assertEqual(report["warmup"], 1000)
            self.assertEqual(report["iterations"], 5000)
            self.assertGreater(report["elapsed_ns"], 0)
            self.assertGreater(report["cpu_ns"], 0)
            self.assertGreater(report["max_rss_kb"], 0)
            self.assertGreater(report["checksum"], 0)
            reports[mode] = report
        self.assertEqual(len({report["checksum"] for report in reports.values()}), 1)
        self.assertEqual(reports["off"]["live_series"], 0)
        self.assertEqual(reports["off"]["publish_count"], 0)
        for field in ("receive_count", "enqueue_count", "dequeue_count",
                      "callback_count", "publish_window_samples",
                      "queue_window_samples", "callback_window_samples"):
            self.assertEqual(reports["off"][field], 0)
        for mode in ("basic", "detailed"):
            self.assertEqual(reports[mode]["live_series"], 3)
            for field in ("publish_count", "receive_count", "enqueue_count",
                          "dequeue_count", "callback_count"):
                self.assertEqual(reports[mode][field], 6000)
            for field in ("publish_window_samples", "queue_window_samples",
                          "callback_window_samples"):
                self.assertGreater(reports[mode][field], 0)
                self.assertLessEqual(reports[mode][field], 6000)

    def test_invalid_mode_or_counts_fail(self):
        for args in (("invalid", "1", "1"), ("off", "0", "1"),
                     ("basic", "1", "not-a-count")):
            run = subprocess.run(
                [str(benchmark_path()), *args],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            self.assertNotEqual(run.returncode, 0)
            self.assertTrue(run.stderr)


if __name__ == "__main__":
    unittest.main()
