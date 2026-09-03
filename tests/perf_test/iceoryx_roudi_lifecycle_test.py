#!/usr/bin/env python3

# Copyright 2026 WheelOS. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
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


def _runfile(path):
    runfiles = os.environ.get("RUNFILES_DIR")
    workspace = os.environ.get("TEST_WORKSPACE")
    if runfiles and workspace:
        candidate = Path(runfiles, workspace, path)
        if candidate.exists():
            return candidate
    return Path(__file__).resolve().parents[2] / path


class IceoryxRouDiLifecycleTest(unittest.TestCase):
    def test_two_sequential_pod_cases_share_clean_suite_owner(self):
        output_dir = Path(
            os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", os.getcwd())
        )
        result_path = output_dir / "iceoryx_roudi_lifecycle.json"
        suite = _runfile("tests/perf_test/cyber_rt_benchmark_suite")
        env = {
            **os.environ,
            "CYBER_PATH": str(_runfile("cyber")),
            "GLOG_log_dir": str(output_dir),
        }
        completed = subprocess.run(
            [
                str(suite),
                f"--output_json={result_path}",
                "--iceoryx_restart_regression=true",
                "--max_loss_rate=0",
                "--min_achieved_rate_ratio=0.9",
                "--startup_wait_ms=200",
                "--cooldown_wait_ms=200",
            ],
            cwd=output_dir,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=90,
            check=False,
        )
        self.assertEqual(
            completed.returncode,
            0,
            msg=f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
        )

        report = json.loads(result_path.read_text(encoding="utf-8"))
        self.assertEqual(report["result_count"], 2)
        self.assertTrue(report["iceoryx_roudi"]["started"])
        self.assertTrue(report["iceoryx_roudi"]["clean_shutdown"])
        self.assertEqual(report["iceoryx_roudi"]["exit_code"], 0)
        self.assertEqual(report["iceoryx_roudi"]["term_signal"], 0)
        self.assertEqual(
            report["iceoryx_roudi"]["chunk_count"],
            report["iceoryx_roudi"]["publisher_history_capacity"]
            + report["iceoryx_roudi"]["in_flight_margin"],
        )

        for result in report["results"]:
            self.assertTrue(result["success"], msg=result["error_message"])
            self.assertTrue(result["required_for_release"])
            self.assertEqual(result["message_type"], "pod")
            self.assertEqual(result["payload_bytes"], 1024 * 1024)
            self.assertTrue(result["execution"]["shutdown_confirmed"])
            throughput = result["throughput"]
            sent = throughput["measured_sent_messages"]
            received = throughput["measured_received_messages"]
            self.assertGreater(sent, 0)
            self.assertEqual(throughput["measured_loan_publish_successes"], sent)
            self.assertEqual(
                throughput["measured_fallback_transmit_attempts"], 0
            )
            self.assertEqual(
                throughput["measured_fallback_transmit_successes"], 0
            )
            self.assertEqual(throughput["measured_send_failures"], 0)
            self.assertEqual(received, sent)
            self.assertEqual(throughput["zero_copy_borrowed_messages"], received)
            self.assertEqual(throughput["zero_copy_copy_count"], 0)


if __name__ == "__main__":
    unittest.main()
