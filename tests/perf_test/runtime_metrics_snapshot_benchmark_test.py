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
        return Path(runfiles, workspace, "tests/perf_test/runtime_metrics_snapshot_benchmark")
    return Path(__file__).resolve().with_name("runtime_metrics_snapshot_benchmark")


class RuntimeMetricsSnapshotBenchmarkTest(unittest.TestCase):
    def test_active_series_and_output_shape(self):
        for series in (1, 32):
            with self.subTest(series=series):
                result = subprocess.run(
                    [str(benchmark_path()), str(series), "3"],
                    capture_output=True, text=True, timeout=15, check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report["series"], series)
                self.assertEqual(report["iterations"], 3)
                for key in (
                    "snapshot_p50_ns", "snapshot_p99_ns", "json_p50_ns",
                    "json_p99_ns", "cpu_ns", "json_bytes", "max_rss_kb",
                ):
                    self.assertIs(type(report[key]), int)
                    self.assertGreater(report[key], 0)
                self.assertLessEqual(report["snapshot_p50_ns"], report["snapshot_p99_ns"])
                self.assertLessEqual(report["json_p50_ns"], report["json_p99_ns"])

    def test_invalid_series_is_rejected(self):
        for series in ("0", "4097", "-1", "not-a-number"):
            with self.subTest(series=series):
                result = subprocess.run(
                    [str(benchmark_path()), series, "1"],
                    capture_output=True, text=True, timeout=5, check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)


if __name__ == "__main__":
    unittest.main()
