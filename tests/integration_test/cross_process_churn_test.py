#!/usr/bin/env python3

# Copyright 2026 WheelOS. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.

import os
import re
import select
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


CHURN_ROUNDS = 10


def _runfile(*parts):
    repository_root = Path(os.path.abspath(__file__)).parents[2]
    return str(repository_root.joinpath(*parts))


def _environment(**updates):
    return {
        **os.environ,
        "CYBER_PATH": _runfile("cyber"),
        "PYTHONUNBUFFERED": "1",
        **updates,
    }


def _start(command, **environment):
    return subprocess.Popen(
        command,
        env=_environment(**environment),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def _decode(output):
    if isinstance(output, str):
        return output
    return output.decode("utf-8", errors="replace")


def _wait_for_line(process, expected, timeout=12):
    deadline = time.monotonic() + timeout
    expected_bytes = expected.encode("utf-8")
    output = bytearray()
    buffered = getattr(process, "_churn_stdout_buffer", None)
    if buffered is None:
        buffered = bytearray()
        process._churn_stdout_buffer = buffered

    reached_eof = False
    while True:
        newline = buffered.find(b"\n")
        if newline >= 0 or (reached_eof and buffered):
            end = newline + 1 if newline >= 0 else len(buffered)
            line = bytes(buffered[:end])
            del buffered[:end]
            output.extend(line)
            if expected_bytes in line:
                return _decode(output)
            continue
        if reached_eof:
            break

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        ready, _, _ = select.select(
            [process.stdout], [], [], min(0.2, remaining)
        )
        if not ready:
            continue
        chunk = os.read(process.stdout.fileno(), 4096)
        if chunk:
            buffered.extend(chunk)
        else:
            reached_eof = True

    returncode = process.poll()
    stderr = process.stderr.read() if returncode is not None else b""
    raise AssertionError(
        "expected {!r}, output={!r}, returncode={!r}, stderr={!r}".format(
            expected,
            _decode(output + buffered),
            returncode,
            _decode(stderr),
        )
    )


def _stop(process, timeout=8):
    if process.poll() is None:
        try:
            process.send_signal(signal.SIGINT)
        except ProcessLookupError:
            pass
    try:
        _, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        _, stderr = process.communicate()
        raise AssertionError(
            "process did not exit within {} seconds after SIGINT; "
            "killed exact pid {}, stderr={!r}".format(
                timeout, process.pid, _decode(stderr)
            )
        )
    if process.returncode != 0:
        raise AssertionError(
            "process exited with {}, stderr={!r}".format(
                process.returncode, _decode(stderr)
            )
        )


def _assert_two_monotonic_deliveries(output, token):
    sequences = [
        int(sequence)
        for sequence in re.findall(
            r"RECEIVED {} delivery=[12] seq=(\d+)".format(re.escape(token)),
            output,
        )
    ]
    if len(sequences) != 2 or sequences[1] <= sequences[0]:
        raise AssertionError(
            "expected two strictly increasing deliveries for {!r}, got {!r} "
            "in output {!r}".format(token, sequences, output)
        )


class CrossProcessChurnTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = _runfile(
            "tests", "integration_test", "cross_process_lifecycle_helper"
        )
        cls.mainboard = _runfile("cyber", "mainboard", "mainboard")
        cls.recorder = _runfile(
            "cyber", "tools", "cyber_recorder", "cyber_recorder"
        )
        cls.component = _runfile(
            "tests",
            "integration_test",
            "librecorder_input_component.so",
        )

    def test_reader_writer_bidirectional_restart_churn(self):
        suffix = str(os.getpid())
        channel = "/tests/churn/pubsub_" + suffix

        writer = _start([self.helper, "writer", channel, "persistent-writer"])
        try:
            _wait_for_line(writer, "READY")
            for _ in range(CHURN_ROUNDS):
                reader = _start([self.helper, "reader", channel])
                try:
                    _wait_for_line(reader, "READY")
                    delivery = _wait_for_line(
                        reader,
                        "RECEIVED persistent-writer delivery=2",
                    )
                    _assert_two_monotonic_deliveries(
                        delivery, "persistent-writer"
                    )
                finally:
                    _stop(reader)
                self.assertIsNone(writer.poll())
        finally:
            _stop(writer)

        reader = _start([self.helper, "reader", channel])
        try:
            _wait_for_line(reader, "READY")
            for round_index in range(CHURN_ROUNDS):
                token = "writer-{}".format(round_index)
                writer = _start([self.helper, "writer", channel, token])
                try:
                    _wait_for_line(writer, "READY")
                    delivery = _wait_for_line(
                        reader, "RECEIVED " + token + " delivery=2"
                    )
                    _assert_two_monotonic_deliveries(delivery, token)
                finally:
                    _stop(writer)
                self.assertIsNone(reader.poll())
        finally:
            _stop(reader)

    def test_client_server_bidirectional_restart_churn(self):
        suffix = str(os.getpid())
        service = "tests/churn/service_" + suffix

        server = _start([self.helper, "server", service, "persistent-server"])
        try:
            _wait_for_line(server, "READY")
            for round_index in range(CHURN_ROUNDS):
                token = "client-{}".format(round_index)
                client = _start([self.helper, "client", service, token])
                try:
                    _wait_for_line(
                        client,
                        "RESPONSE persistent-server:" + token,
                        timeout=15,
                    )
                    _, stderr = client.communicate(timeout=5)
                    self.assertEqual(0, client.returncode)
                    self.assertNotIn("WRITE_FAILED", _decode(stderr))
                finally:
                    if client.poll() is None:
                        _stop(client)
                self.assertIsNone(server.poll())
        finally:
            _stop(server)

        client = _start([self.helper, "client_watch", service, "watch-client"])
        try:
            _wait_for_line(client, "READY")
            for round_index in range(CHURN_ROUNDS):
                token = "server-{}".format(round_index)
                server = _start([self.helper, "server", service, token])
                try:
                    _wait_for_line(server, "READY")
                    _wait_for_line(
                        client,
                        "RESPONSE " + token + ":watch-client",
                        timeout=15,
                    )
                finally:
                    _stop(server)
                self.assertIsNone(client.poll())
        finally:
            _stop(client)

    def test_recorder_replay_component_restart_churn(self):
        suffix = str(os.getpid())
        input_channel = "/tests/churn/component_input_" + suffix
        ack_channel = "/tests/churn/component_ack_" + suffix
        output_dir = tempfile.mkdtemp(prefix="component_churn_record_")
        record_path = os.path.join(output_dir, "component.record")

        source = _start(
            [self.helper, "writer", input_channel, "recorded-message"]
        )
        recorder = None
        try:
            _wait_for_line(source, "READY")
            time.sleep(3)
            recorder = _start(
                [
                    self.recorder,
                    "record",
                    "-o",
                    record_path,
                    "-c",
                    input_channel,
                ]
            )
            time.sleep(6)
            running_recorder = recorder
            recorder = None
            _stop(running_recorder, timeout=15)
        finally:
            if recorder is not None:
                _stop(recorder)
            _stop(source)

        if not os.path.isfile(record_path):
            record_path += ".00000"
        self.assertTrue(os.path.isfile(record_path))
        self.assertGreater(os.path.getsize(record_path), 0)
        record_info = subprocess.run(
            [self.recorder, "info", record_path],
            env=_environment(),
            capture_output=True,
            check=True,
            text=True,
            timeout=15,
        )
        self.assertIn(input_channel, record_info.stdout)
        self.assertNotIn("message_number: 0", record_info.stdout)

        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".dag", delete=False
        ) as dag:
            dag.write(
                """module_config {
  module_library: "%s"
  components {
    class_name: "RecorderInputComponent"
    config {
      name: "recorder-input-component-%s"
      readers { channel: "%s" pending_queue_size: 32 }
    }
  }
}
"""
                % (self.component, suffix, input_channel)
            )
            dag_path = dag.name

        ack_reader = _start([self.helper, "reader", ack_channel])
        player = None
        try:
            _wait_for_line(ack_reader, "READY")
            player = _start(
                [self.recorder, "play", "-f", record_path, "--loop"]
            )
            time.sleep(1)
            for round_index in range(CHURN_ROUNDS):
                token = "component-{}".format(round_index)
                component = _start(
                    [
                        self.mainboard,
                        "--process_name",
                        "recorder-component-{}".format(round_index),
                        "-d",
                        dag_path,
                    ],
                    CYBER_TEST_ACK_CHANNEL=ack_channel,
                    CYBER_TEST_COMPONENT_TOKEN=token,
                )
                try:
                    _wait_for_line(
                        ack_reader,
                        "RECEIVED " + token + ":recorded-message",
                        timeout=15,
                    )
                finally:
                    _stop(component, timeout=10)
                self.assertIsNone(player.poll())
        finally:
            if player is not None:
                _stop(player, timeout=15)
            _stop(ack_reader)
            os.unlink(dag_path)
            shutil.rmtree(output_dir)


if __name__ == "__main__":
    unittest.main()
