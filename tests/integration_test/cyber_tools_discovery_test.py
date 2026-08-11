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
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from cyber.proto.unit_test_pb2 import ChatterBenchmark
from cyber.python.cyber_py3 import cyber


def _tool_path(package, name):
    return os.path.join(
        os.environ["TEST_SRCDIR"],
        os.environ["TEST_WORKSPACE"],
        "cyber",
        "tools",
        package,
        name,
    )


def _write_rtps_config():
    root = tempfile.mkdtemp(prefix="cyber_tools_rtps_")
    config_dir = os.path.join(root, "cyber", "conf")
    os.makedirs(config_dir)
    with open(os.path.join(config_dir, "cyber.pb.conf"), "w") as config:
        config.write(
            """transport_conf {
  shm_conf { notifier_type: "multicast" }
  communication_mode {
    same_proc: INTRA
    diff_proc: RTPS
    diff_host: RTPS
  }
  slow_consumer_policy: DROP
}
run_mode_conf {
  run_mode: MODE_REALITY
  clock_mode: MODE_CYBER
}
scheduler_conf {
  routine_num: 100
  default_proc_num: 16
}
"""
        )
    return os.path.join(root, "cyber"), root


def _publisher(node_name, channel_name, service_name):
    cyber.init("cyber_tools_discovery_publisher")
    node = cyber.Node(node_name)
    writer = node.create_writer(channel_name, ChatterBenchmark, 8)
    node.create_service(
        service_name,
        ChatterBenchmark,
        ChatterBenchmark,
        lambda request: ChatterBenchmark(
            seq=request.seq, content="service-response"
        ),
    )
    message = ChatterBenchmark(content="cross-process-tool-message")
    print("READY", flush=True)
    sequence = 0
    try:
        while cyber.ok():
            message.seq = sequence
            writer.write(message)
            sequence += 1
            time.sleep(0.1)
    finally:
        cyber.shutdown()


def _wait_for_output(command, expected, timeout=10):
    process = subprocess.Popen(
        command,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    output = ""
    try:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([process.stdout], [], [], 0.2)
            if ready:
                line = process.stdout.readline()
                if not line and process.poll() is not None:
                    break
                output += line
                if expected in output:
                    return output
        stderr = process.stderr.read() if process.poll() is not None else ""
        raise AssertionError(
            "expected {!r} in output {!r}; stderr={!r}".format(
                expected, output, stderr
            )
        )
    finally:
        process.terminate()
        process.wait(timeout=5)
        process.stdout.close()
        process.stderr.close()


def _run_until_contains(command, expected, timeout=15):
    deadline = time.monotonic() + timeout
    last_result = None
    while time.monotonic() < deadline:
        last_result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=10,
        )
        if expected in last_result.stdout:
            return last_result.stdout
        time.sleep(0.2)
    raise AssertionError(
        "expected {!r} in output {!r}; returncode={!r}; stderr={!r}".format(
            expected,
            last_result.stdout,
            last_result.returncode,
            last_result.stderr,
        )
    )


class CyberToolsDiscoveryTest(unittest.TestCase):
    def test_in_process_and_cross_process_discovery(self):
        suffix = str(os.getpid())
        in_process_channel = "/tests/tools/in_process_" + suffix
        received = threading.Event()

        reader_node = cyber.Node("tools_in_process_reader_" + suffix)
        reader_node.create_reader(
            in_process_channel,
            ChatterBenchmark,
            lambda message: received.set()
            if message.content == "in-process-tool-message"
            else None,
        )
        writer_node = cyber.Node("tools_in_process_writer_" + suffix)
        writer = writer_node.create_writer(
            in_process_channel, ChatterBenchmark, 8
        )
        message = ChatterBenchmark(content="in-process-tool-message")
        deadline = time.monotonic() + 5
        while not received.is_set() and time.monotonic() < deadline:
            writer.write(message)
            received.wait(0.05)
        self.assertTrue(received.is_set(), "in-process message was not received")

        node_name = "tools_cross_process_writer_" + suffix
        channel_name = "/tests/tools/cross_process_" + suffix
        publisher = subprocess.Popen(
            [
                sys.executable,
                os.path.abspath(__file__),
                "--publisher",
                node_name,
                channel_name,
                "tools_service_" + suffix,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            ready, _, _ = select.select([publisher.stdout], [], [], 10)
            self.assertTrue(ready, "publisher did not become ready")
            self.assertEqual("READY", publisher.stdout.readline().strip())

            _run_until_contains(
                [_tool_path("cyber_node", "cyber_node"), "list"],
                node_name,
            )

            _run_until_contains(
                [_tool_path("cyber_node", "cyber_node"), "info", node_name],
                channel_name,
            )

            _run_until_contains(
                [_tool_path("cyber_channel", "cyber_channel"), "list"],
                channel_name,
            )

            _run_until_contains(
                [
                    _tool_path("cyber_channel", "cyber_channel"),
                    "info",
                    channel_name,
                ],
                node_name,
            )

            _run_until_contains(
                [
                    _tool_path("cyber_channel", "cyber_channel"),
                    "type",
                    channel_name,
                ],
                ChatterBenchmark.DESCRIPTOR.full_name,
            )

            service_name = "tools_service_" + suffix
            _run_until_contains(
                [_tool_path("cyber_service", "cyber_service"), "list"],
                service_name,
            )

            _run_until_contains(
                [
                    _tool_path("cyber_service", "cyber_service"),
                    "info",
                    service_name,
                ],
                node_name,
            )

            _wait_for_output(
                [
                    _tool_path("cyber_channel", "cyber_channel"),
                    "echo",
                    channel_name,
                ],
                "cross-process-tool-message",
            )

            _wait_for_output(
                [
                    _tool_path("cyber_channel", "cyber_channel"),
                    "hz",
                    "-w",
                    "10",
                    channel_name,
                ],
                "average rate:",
            )
            _wait_for_output(
                [
                    _tool_path("cyber_channel", "cyber_channel"),
                    "bw",
                    "-w",
                    "10",
                    channel_name,
                ],
                "average:",
            )
        finally:
            publisher.terminate()
            publisher.wait(timeout=5)
            publisher.stdout.close()
            publisher.stderr.close()

    def test_recorder_records_and_replays_across_processes(self):
        suffix = str(os.getpid())
        node_name = "tools_recorder_writer_" + suffix
        channel_name = "/tests/tools/recorder_" + suffix
        service_name = "tools_recorder_service_" + suffix
        output_dir = tempfile.mkdtemp(prefix="cyber_recorder_e2e_")
        record_path = os.path.join(output_dir, "capture.record")
        publisher = subprocess.Popen(
            [
                sys.executable,
                os.path.abspath(__file__),
                "--publisher",
                node_name,
                channel_name,
                service_name,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        recorder = None
        publisher_running = True
        try:
            ready, _, _ = select.select([publisher.stdout], [], [], 10)
            self.assertTrue(ready, "publisher did not become ready")
            self.assertEqual("READY", publisher.stdout.readline().strip())

            recorder = subprocess.Popen(
                [
                    _tool_path("cyber_recorder", "cyber_recorder"),
                    "record",
                    "-o",
                    record_path,
                    "-c",
                    channel_name,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            time.sleep(4)
            recorder.send_signal(signal.SIGINT)
            recorder.wait(timeout=15)
            self.assertEqual(0, recorder.returncode, recorder.stderr.read())
            if not os.path.isfile(record_path):
                record_path += ".00000"
            self.assertTrue(os.path.isfile(record_path))
            self.assertGreater(os.path.getsize(record_path), 0)
            publisher.terminate()
            publisher.wait(timeout=5)
            publisher_running = False

            info = subprocess.run(
                [_tool_path("cyber_recorder", "cyber_recorder"), "info", record_path],
                capture_output=True,
                check=True,
                text=True,
                timeout=15,
            )
            self.assertIn(channel_name, info.stdout)

            split_path = os.path.join(output_dir, "split.record")
            subprocess.run(
                [
                    _tool_path("cyber_recorder", "cyber_recorder"),
                    "split",
                    "-f",
                    record_path,
                    "-o",
                    split_path,
                    "-c",
                    channel_name,
                ],
                check=True,
                timeout=15,
            )
            if not os.path.isfile(split_path):
                split_path += ".00000"
            self.assertTrue(os.path.isfile(split_path))

            recovered_path = os.path.join(output_dir, "recovered.record")
            subprocess.run(
                [
                    _tool_path("cyber_recorder", "cyber_recorder"),
                    "recover",
                    "-f",
                    record_path,
                    "-o",
                    recovered_path,
                ],
                check=True,
                timeout=15,
            )
            self.assertTrue(os.path.isfile(recovered_path))

            replayed = threading.Event()
            reader_node = cyber.Node("tools_replay_reader_" + suffix)
            reader_node.create_reader(
                channel_name,
                ChatterBenchmark,
                lambda message: replayed.set()
                if message.content == "cross-process-tool-message"
                else None,
            )
            player = subprocess.Popen(
                [
                    _tool_path("cyber_recorder", "cyber_recorder"),
                    "play",
                    "-f",
                    record_path,
                    "-l",
                    "-p",
                    "1",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                self.assertTrue(replayed.wait(15), "record replay was not received")
                player.send_signal(signal.SIGINT)
                player.wait(timeout=20)
                self.assertEqual(0, player.returncode, player.stderr.read())
            finally:
                if player.poll() is None:
                    player.send_signal(signal.SIGINT)
                    player.wait(timeout=5)
                player.stdout.close()
                player.stderr.close()
        finally:
            if recorder is not None:
                if recorder.poll() is None:
                    recorder.send_signal(signal.SIGINT)
                    recorder.wait(timeout=5)
                recorder.stdout.close()
                recorder.stderr.close()
            if publisher_running:
                publisher.terminate()
                publisher.wait(timeout=5)
            publisher.stdout.close()
            publisher.stderr.close()
            shutil.rmtree(output_dir)

    def test_monitor_and_launch_lifecycle(self):
        monitor = subprocess.run(
            [_tool_path("cyber_monitor", "cyber_monitor"), "-h"],
            capture_output=True,
            check=True,
            text=True,
            timeout=10,
        )
        self.assertIn("Usage:", monitor.stdout)

        launch_dir = tempfile.mkdtemp(prefix="cyber_launch_e2e_")
        launch_file = os.path.join(launch_dir, "tools.launch")
        with open(launch_file, "w") as launch:
            launch.write(
                """<launch>
  <environment><TOOLS_LAUNCH_TEST>enabled</TOOLS_LAUNCH_TEST></environment>
  <module>
    <name>tools-launch-sleeper</name>
    <dag_conf>unused</dag_conf>
    <process_name>/bin/sleep 30</process_name>
    <type>binary</type>
  </module>
</launch>
"""
            )
        launcher = subprocess.Popen(
            [_tool_path("cyber_launch", "cyber_launch"), "start", launch_file],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            time.sleep(2)
            self.assertIsNone(launcher.poll())
            stopped = subprocess.run(
                [
                    _tool_path("cyber_launch", "cyber_launch"),
                    "stop",
                    launch_file,
                ],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(0, stopped.returncode, stopped.stderr)
            launcher.wait(timeout=10)
            self.assertEqual(0, launcher.returncode, launcher.stderr.read())
        finally:
            if launcher.poll() is None:
                launcher.send_signal(signal.SIGINT)
                launcher.wait(timeout=5)
            launcher.stdout.close()
            launcher.stderr.close()
            shutil.rmtree(launch_dir)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--publisher":
        _publisher(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        cyber_path, temp_root = _write_rtps_config()
        os.environ["CYBER_PATH"] = cyber_path
        cyber.init("cyber_tools_discovery_test")
        try:
            unittest.main()
        finally:
            cyber.shutdown()
            shutil.rmtree(temp_root)
