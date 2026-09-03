#!/usr/bin/env python3

# Copyright 2026 WheelOS. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.

import errno
import os
import re
import select
import shutil
import signal
import socket
import subprocess
import threading
import time
import unittest
import zlib
from pathlib import Path


CHURN_ROUNDS = 10
CHURN_DOMAIN_ID = "0"
CHURN_CYBER_PATH = None
MAX_STDERR_BYTES = 64 * 1024


def _runfile(*parts):
    repository_root = Path(os.path.abspath(__file__)).parents[2]
    return str(repository_root.joinpath(*parts))


def _environment(**updates):
    return {
        **os.environ,
        "CYBER_PATH": CHURN_CYBER_PATH or _runfile("cyber"),
        "CYBER_DOMAIN_ID": CHURN_DOMAIN_ID,
        "PYTHONUNBUFFERED": "1",
        **updates,
    }


class _PortNamespace:
    _BASE = 38000
    _SLOT_COUNT = 25
    # Fast DDS uses participant IDs through 255 at two ports per ID. Keep each
    # reserved range wider than that full participant span.
    _SLOT_WIDTH = 1024

    def __init__(self):
        identity = "{}:{}:{}".format(
            os.environ.get("TEST_TARGET", "cross_process_churn_test"),
            os.environ.get("TEST_SHARD_INDEX", "0"),
            os.getpid(),
        )
        start = zlib.crc32(identity.encode("utf-8")) % self._SLOT_COUNT
        self._socket = None
        self.port_base = None
        for attempt in range(self._SLOT_COUNT):
            slot = (start + attempt) % self._SLOT_COUNT
            reservation = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            try:
                reservation.bind(
                    "\0wheelos_cross_process_churn_ports_{}".format(slot)
                )
            except OSError as error:
                reservation.close()
                if error.errno == errno.EADDRINUSE:
                    continue
                raise
            self._socket = reservation
            self.port_base = self._BASE + slot * self._SLOT_WIDTH
            return
        raise RuntimeError("no isolated DDS port namespace is available")

    def close(self):
        if self._socket is not None:
            self._socket.close()
            self._socket = None


class _BoundedStderr:
    def __init__(self, stream):
        self._stream = stream
        self._buffer = bytearray()
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._drain, daemon=True)
        self._thread.start()

    def _drain(self):
        while True:
            chunk = self._stream.read(4096)
            if not chunk:
                return
            with self._lock:
                self._buffer.extend(chunk)
                if len(self._buffer) > MAX_STDERR_BYTES:
                    del self._buffer[:-MAX_STDERR_BYTES]

    def text(self):
        with self._lock:
            return _decode(bytes(self._buffer))

    def close(self):
        self._thread.join(timeout=2)
        self._stream.close()


def _start(command, **environment):
    process = subprocess.Popen(
        command,
        env=_environment(**environment),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    process._churn_stderr = _BoundedStderr(process.stderr)
    return process


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
    raise AssertionError(
        "expected {!r}, output={!r}, returncode={!r}, stderr={!r}".format(
            expected,
            _decode(output + buffered),
            returncode,
            process._churn_stderr.text(),
        )
    )


def _close_process_streams(process):
    if process.stdout is not None:
        process.stdout.close()
    process._churn_stderr.close()


def _wait_for_success(process, timeout=8):
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(
            "process {} did not exit within {} seconds; stderr={!r}".format(
                process.pid, timeout, process._churn_stderr.text()
            )
        ) from error
    stderr = process._churn_stderr.text()
    _close_process_streams(process)
    if process.returncode != 0:
        raise AssertionError(
            "process exited with {}, stderr={!r}".format(
                process.returncode, stderr
            )
        )
    return stderr


def _stop(process, timeout=8):
    if process.poll() is None:
        try:
            process.send_signal(signal.SIGINT)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        stderr = process._churn_stderr.text()
        _close_process_streams(process)
        raise AssertionError(
            "process did not exit within {} seconds after SIGINT; "
            "killed exact pid {}, stderr={!r}".format(
                timeout, process.pid, stderr
            )
        )
    stderr = process._churn_stderr.text()
    _close_process_streams(process)
    if process.returncode != 0:
        raise AssertionError(
            "process exited with {}, stderr={!r}".format(
                process.returncode, stderr
            )
        )


def _drain_stdout(process, quiet_period=0.2):
    output = bytearray()
    buffered = getattr(process, "_churn_stdout_buffer", None)
    if buffered:
        output.extend(buffered)
        buffered.clear()

    quiet_deadline = time.monotonic() + quiet_period
    while process.poll() is None:
        remaining = quiet_deadline - time.monotonic()
        if remaining <= 0:
            break
        ready, _, _ = select.select([process.stdout], [], [], remaining)
        if not ready:
            break
        chunk = os.read(process.stdout.fileno(), 4096)
        if not chunk:
            break
        output.extend(chunk)
        quiet_deadline = time.monotonic() + quiet_period
    return _decode(output)


def _assert_two_monotonic_deliveries(output, token):
    sequences = [
        int(sequence)
        for sequence in re.findall(
            r"DELIVERY {} count=[12] seq=(\d+)".format(re.escape(token)),
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
        global CHURN_CYBER_PATH
        cls.port_namespace = _PortNamespace()
        scratch_parent = os.environ.get(
            "TEST_UNDECLARED_OUTPUTS_DIR", os.getcwd()
        )
        cls.scratch = Path(scratch_parent).joinpath(
            "cross_process_churn_{}".format(os.getpid())
        )
        cls.scratch.mkdir(parents=True, exist_ok=False)
        cyber_path = cls.scratch.joinpath("cyber")
        cyber_path.joinpath("conf").mkdir(parents=True)
        cyber_path.joinpath("conf", "cyber.pb.conf").write_text(
            """transport_conf {
  shm_conf { notifier_type: "multicast" }
  participant_attr {
    lease_duration: 3
    announcement_period: 1
    domain_id_gain: 1024
    port_base: %d
  }
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
            % cls.port_namespace.port_base,
            encoding="utf-8",
        )
        CHURN_CYBER_PATH = str(cyber_path)
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

    @classmethod
    def tearDownClass(cls):
        global CHURN_CYBER_PATH
        CHURN_CYBER_PATH = None
        shutil.rmtree(cls.scratch)
        cls.port_namespace.close()

    def test_reader_writer_bidirectional_restart_churn(self):
        suffix = str(os.getpid())
        channel = "/tests/churn/pubsub_" + suffix

        writer = _start([self.helper, "writer", channel, "persistent-writer"])
        try:
            _wait_for_line(writer, "ENDPOINT_CREATED writer")
            for round_index in range(CHURN_ROUNDS):
                reader = _start([self.helper, "reader", channel])
                try:
                    _wait_for_line(reader, "ENDPOINT_CREATED reader")
                    _wait_for_line(writer, "TOPOLOGY_OBSERVED reader")
                    _wait_for_line(reader, "TOPOLOGY_OBSERVED writer")
                    try:
                        delivery = _wait_for_line(
                            reader, "DELIVERY persistent-writer count=2"
                        )
                    except AssertionError as error:
                        raise AssertionError(
                            "persistent-writer reader round {} failed; "
                            "writer_output={!r}; {}".format(
                                round_index, _drain_stdout(writer), error
                            )
                        ) from error
                    _assert_two_monotonic_deliveries(
                        delivery, "persistent-writer"
                    )
                finally:
                    _stop(reader)
                _wait_for_line(writer, "TOPOLOGY_LOST reader")
                self.assertIsNone(writer.poll())
        finally:
            _stop(writer)

        reader = _start([self.helper, "reader", channel])
        try:
            _wait_for_line(reader, "ENDPOINT_CREATED reader")
            for round_index in range(CHURN_ROUNDS):
                token = "writer-{}".format(round_index)
                writer = _start([self.helper, "writer", channel, token])
                try:
                    _wait_for_line(writer, "ENDPOINT_CREATED writer")
                    _wait_for_line(reader, "TOPOLOGY_OBSERVED writer")
                    _wait_for_line(writer, "TOPOLOGY_OBSERVED reader")
                    delivery = _wait_for_line(
                        reader, "DELIVERY " + token + " count=2"
                    )
                    _assert_two_monotonic_deliveries(delivery, token)
                finally:
                    _stop(writer)
                _wait_for_line(reader, "TOPOLOGY_LOST writer")
                self.assertIsNone(reader.poll())
        finally:
            _stop(reader)

    def test_client_server_bidirectional_restart_churn(self):
        suffix = str(os.getpid())
        service = "tests/churn/service_" + suffix

        server = _start([self.helper, "server", service, "persistent-server"])
        try:
            _wait_for_line(server, "ENDPOINT_CREATED server")
            for round_index in range(CHURN_ROUNDS):
                token = "client-{}".format(round_index)
                client = _start([self.helper, "client", service, token])
                try:
                    try:
                        _wait_for_line(client, "ENDPOINT_CREATED client")
                        _wait_for_line(
                            client, "SERVICE_TOPOLOGY present", timeout=15
                        )
                        _wait_for_line(
                            server, "REQUEST_DELIVERED " + token, timeout=15
                        )
                        _wait_for_line(
                            client,
                            "RESPONSE persistent-server:" + token,
                            timeout=15,
                        )
                    except AssertionError as error:
                        raise AssertionError(
                            "persistent-server client round {} failed; {}".format(
                                round_index, error
                            )
                        ) from error
                    stderr = _wait_for_success(client, timeout=5)
                    self.assertNotIn("WRITE_FAILED", stderr)
                finally:
                    if client.poll() is None:
                        _stop(client)
                self.assertIsNone(server.poll())
        finally:
            _stop(server)

        client = _start([self.helper, "client_watch", service, "watch-client"])
        try:
            _wait_for_line(client, "ENDPOINT_CREATED client")
            _wait_for_line(client, "SERVICE_TOPOLOGY absent")
            previous_token = None
            for round_index in range(CHURN_ROUNDS):
                token = "server-{}".format(round_index)
                server = _start([self.helper, "server", service, token])
                try:
                    _wait_for_line(server, "ENDPOINT_CREATED server")
                    _wait_for_line(
                        client, "SERVICE_TOPOLOGY present", timeout=15
                    )
                    responses = _wait_for_line(
                        client,
                        "RESPONSE " + token + ":watch-client",
                        timeout=15,
                    )
                    _wait_for_line(
                        server, "REQUEST_DELIVERED watch-client", timeout=15
                    )
                    if previous_token is not None:
                        self.assertNotIn(
                            "RESPONSE " + previous_token + ":watch-client",
                            responses,
                        )
                finally:
                    _stop(server)
                _wait_for_line(
                    client, "SERVICE_TOPOLOGY absent", timeout=15
                )
                previous_token = token
                self.assertIsNone(client.poll())
        finally:
            _stop(client)

    def test_recorder_replay_component_restart_churn(self):
        suffix = str(os.getpid())
        input_channel = "/tests/churn/component_input_" + suffix
        ack_channel = "/tests/churn/component_ack_" + suffix
        output_dir = self.scratch.joinpath("component_churn_record")
        output_dir.mkdir()
        record_path = str(output_dir.joinpath("component.record"))

        source = _start(
            [self.helper, "writer", input_channel, "recorded-message"]
        )
        recorder = None
        try:
            _wait_for_line(source, "ENDPOINT_CREATED writer")
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
            _wait_for_line(source, "TOPOLOGY_OBSERVED reader", timeout=15)
            _wait_for_line(
                source, "WRITE_ACCEPTED_AFTER_TOPOLOGY count=100", timeout=15
            )
            running_recorder = recorder
            recorder = None
            _stop(running_recorder, timeout=15)
            _wait_for_line(source, "TOPOLOGY_LOST reader", timeout=15)
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

        dag_path = self.scratch.joinpath("recorder_component.dag")
        dag_path.write_text(
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
            % (self.component, suffix, input_channel),
            encoding="utf-8",
        )

        ack_reader = _start([self.helper, "reader", ack_channel])
        player = None
        try:
            _wait_for_line(ack_reader, "ENDPOINT_CREATED reader")
            player = _start(
                [self.recorder, "play", "-f", record_path, "--loop"]
            )
            for round_index in range(CHURN_ROUNDS):
                token = "component-{}".format(round_index)
                component = _start(
                    [
                        self.mainboard,
                        "--process_name",
                        "recorder-component-{}".format(round_index),
                        "-d",
                        str(dag_path),
                    ],
                    CYBER_TEST_ACK_CHANNEL=ack_channel,
                    CYBER_TEST_COMPONENT_TOKEN=token,
                )
                try:
                    _wait_for_line(
                        ack_reader, "TOPOLOGY_OBSERVED writer", timeout=30
                    )
                    _wait_for_line(
                        ack_reader,
                        "DELIVERY " + token + ":recorded-message",
                        timeout=30,
                    )
                finally:
                    _stop(component, timeout=15)
                _wait_for_line(
                    ack_reader, "TOPOLOGY_LOST writer", timeout=30
                )
                self.assertIsNone(player.poll())
        finally:
            if player is not None:
                _stop(player, timeout=15)
            _stop(ack_reader)
            dag_path.unlink()
            shutil.rmtree(output_dir)


if __name__ == "__main__":
    unittest.main()
