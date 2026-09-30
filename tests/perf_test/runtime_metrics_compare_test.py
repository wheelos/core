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
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

import runtime_metrics_compare as comparison


def result(mode, **updates):
    data = {
        "mode": mode, "warmup": 10, "messages": 100, "payload_bytes": 128,
        "rate_hz": 100, "received": 100, "elapsed_ns": 1100000000,
        "send_elapsed_ns": 1000000000, "drain_elapsed_ns": 100000000,
        "cpu_ns": 10000000, "p99_ns": 200000, "max_rss_kb": 20000,
        "live_series": 0 if mode == "off" else 3,
        "publish_count": 0 if mode == "off" else 110,
        "receive_count": 0 if mode == "off" else 110,
        "callback_count": 0 if mode == "off" else 110,
    }
    data.update(updates)
    return data


class RuntimeMetricsCompareTest(unittest.TestCase):
    def test_rotates_modes_and_keeps_raw_trials(self):
        worker = Mock(side_effect=lambda command, **kwargs:
                      subprocess.CompletedProcess(
                          command, 0, json.dumps(result(command[1])), ""))
        report = comparison.compare("bench", 3, 10, 100, 128, 100, run=worker)
        self.assertEqual(report["status"], "complete")
        self.assertEqual(
            [trial["mode"] for trial in report["trials"]],
            ["off", "basic", "detailed", "basic", "detailed", "off",
             "detailed", "off", "basic"],
        )
        self.assertEqual(report["median_by_mode"]["basic"]["p99_ns"], 200000)
        self.assertTrue(all(trial["result"]["received"] == 100
                            for trial in report["trials"]))
        self.assertEqual(worker.call_count, 9)

    def test_incomplete_delivery_has_no_comparison(self):
        worker = Mock(return_value=subprocess.CompletedProcess(
            ["bench"], 0, json.dumps(result("off", received=99)), ""))
        report = comparison.compare("bench", 3, 10, 100, 128, 100, run=worker)
        self.assertEqual(report["status"], "incomplete")
        self.assertIn("received", report["trials"][0]["error"])
        self.assertNotIn("median_by_mode", report)
        self.assertEqual(worker.call_count, 1)

    def test_wrong_mode_and_worker_failure_are_visible(self):
        for worker in (
            Mock(return_value=subprocess.CompletedProcess(
                ["bench"], 0, json.dumps(result("basic")), "")),
            Mock(return_value=subprocess.CompletedProcess(
                ["bench"], 0, json.dumps([]), "")),
            Mock(return_value=subprocess.CompletedProcess(
                ["bench"], 0, json.dumps(result("off", messages=True)), "")),
            Mock(return_value=subprocess.CompletedProcess(
                ["bench"], 2, "", "bad input")),
        ):
            with self.subTest(worker=worker):
                report = comparison.compare("bench", 1, 10, 100, 128, 100,
                                            run=worker)
                self.assertEqual(report["status"], "incomplete")
                self.assertTrue(report["trials"][0]["error"])

    def test_cli_writes_partial_report_and_exits_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"
            with unittest.mock.patch.object(comparison, "compare", return_value={
                "status": "incomplete", "trials": [{"error": "lost messages"}],
            }):
                self.assertEqual(comparison.main(
                    ["--binary", "bench", "--output", str(output)]), 1)
            self.assertEqual(json.loads(output.read_text())["status"], "incomplete")


if __name__ == "__main__":
    unittest.main()
