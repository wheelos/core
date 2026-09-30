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

"""Repeat the Node pub/sub benchmark without claiming a performance gate."""

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

MODES = ("off", "basic", "detailed")
MEASUREMENTS = ("send_elapsed_ns", "cpu_ns", "p99_ns", "max_rss_kb")


def positive_int(value):
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def check_result(result, mode, warmup, messages, payload_bytes, rate_hz):
    if not isinstance(result, dict):
        raise ValueError("worker output must be a JSON object")
    for key, expected in (
        ("mode", mode),
        ("warmup", warmup),
        ("messages", messages),
        ("payload_bytes", payload_bytes),
        ("rate_hz", rate_hz),
        ("received", messages),
        ("publish_count", 0 if mode == "off" else warmup + messages),
    ):
        value = result.get(key)
        if value != expected or (isinstance(expected, int) and type(value) is not int):
            raise ValueError(f"{key}: expected {expected}, got {result.get(key)}")
    for key in MEASUREMENTS + ("elapsed_ns", "drain_elapsed_ns", "live_series",
                               "receive_count", "callback_count"):
        value = result.get(key)
        if type(value) is not int or value < 0:
            raise ValueError(f"{key}: expected nonnegative integer, got {value}")
    if any(result[key] == 0 for key in
           ("send_elapsed_ns", "elapsed_ns", "cpu_ns", "p99_ns", "max_rss_kb")):
        raise ValueError("measurement window is empty")
    if result["elapsed_ns"] != result["send_elapsed_ns"] + result["drain_elapsed_ns"]:
        raise ValueError("measurement windows do not add up")
    if mode == "off":
        if any(result[key] != 0 for key in
               ("live_series", "receive_count", "callback_count")):
            raise ValueError("off mode recorded metric series")
    elif (result["live_series"] < 3 or result["receive_count"] < messages
          or result["callback_count"] < messages):
        raise ValueError("enabled mode did not record pub/sub activity")


def compare(binary, rounds, warmup, messages, payload_bytes, rate_hz,
            run=subprocess.run):
    report = {
        "status": "incomplete",
        "workload": {
            "warmup": warmup,
            "messages": messages,
            "payload_bytes": payload_bytes,
            "rate_hz": rate_hz,
            "rounds": rounds,
        },
        "trials": [],
    }
    timeout = max(30, (warmup + messages) / rate_hz + 25)
    for round_index in range(rounds):
        for offset in range(len(MODES)):
            mode = MODES[(round_index + offset) % len(MODES)]
            command = [str(binary), mode, str(warmup), str(messages),
                       str(payload_bytes), str(rate_hz)]
            trial = {"round": round_index + 1, "mode": mode}
            report["trials"].append(trial)
            try:
                completed = run(command, capture_output=True, text=True,
                                timeout=timeout, check=False)
                if completed.returncode != 0:
                    raise ValueError(
                        f"worker exited {completed.returncode}: {completed.stderr.strip()}"
                    )
                result = json.loads(completed.stdout)
                trial["result"] = result
                check_result(result, mode, warmup, messages, payload_bytes, rate_hz)
            except (OSError, subprocess.TimeoutExpired, ValueError, TypeError) as exc:
                trial["error"] = str(exc)
                return report
    report["status"] = "complete"
    report["median_by_mode"] = {
        mode: {
            key: statistics.median(
                trial["result"][key] for trial in report["trials"]
                if trial["mode"] == mode
            )
            for key in MEASUREMENTS
        }
        for mode in MODES
    }
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--rounds", type=positive_int, default=3)
    parser.add_argument("--warmup", type=positive_int, default=100)
    parser.add_argument("--messages", type=positive_int, default=1000)
    parser.add_argument("--payload-bytes", type=positive_int, default=1024)
    parser.add_argument("--rate-hz", type=positive_int, default=1000)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    report = compare(args.binary, args.rounds, args.warmup, args.messages,
                     args.payload_bytes, args.rate_hz)
    output = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(output, encoding="utf-8")
    else:
        sys.stdout.write(output)
    if report["status"] != "complete":
        print(report["trials"][-1]["error"], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
