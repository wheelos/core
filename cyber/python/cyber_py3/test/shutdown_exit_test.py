#!/usr/bin/env python3

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


def repo_root() -> Path:
    test_srcdir = os.environ.get("TEST_SRCDIR")
    test_workspace = os.environ.get("TEST_WORKSPACE")
    if test_srcdir and test_workspace:
        return Path(test_srcdir) / test_workspace
    return Path(__file__).resolve().parents[5]


def child_env(root: Path) -> dict[str, str]:
    env = os.environ.copy()
    log_dir = Path(tempfile.mkdtemp(prefix="pycyber-exit-log-"))
    path_entries = [str(root), *sys.path]
    existing_pythonpath = env.get("PYTHONPATH")
    if existing_pythonpath:
        path_entries.append(existing_pythonpath)
    env["PYTHONPATH"] = os.pathsep.join(path_entries)
    env["CYBER_PATH"] = str(root / "cyber")
    env["CYBER_DOMAIN_ID"] = "81"
    env["CYBER_IP"] = "127.0.0.1"
    env["GLOG_log_dir"] = str(log_dir)
    env["GLOG_alsologtostderr"] = "0"
    env["GLOG_colorlogtostderr"] = "1"
    env["GLOG_minloglevel"] = "0"
    env["TERMINFO"] = "/lib/terminfo/"
    env["sysmo_start"] = "0"
    return env


class ShutdownExitTest(unittest.TestCase):
    def run_child(self, script: str, timeout: int = 20) -> None:
        root = repo_root()
        result = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(script)],
            cwd=root,
            env=child_env(root),
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"child process exited with code {result.returncode}\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )

    def test_process_exit_without_explicit_shutdown(self):
        self.run_child(
            """
            import time

            from cyber.python.cyber_py3 import cyber
            from cyber.python.cyber_py3 import cyber_timer
            from cyber.python.cyber_py3 import parameter

            cyber.init()
            node = cyber.Node("pycyber_exit_test")
            server = parameter.ParameterServer(node)
            server.set_parameter(parameter.Parameter("answer", 42))
            timer = cyber_timer.Timer(10, lambda: None, 0)
            timer.start()
            time.sleep(0.05)
            """
        )

    def test_shutdown_inside_timer_callback(self):
        self.run_child(
            """
            import threading
            import time

            from cyber.python.cyber_py3 import cyber
            from cyber.python.cyber_py3 import cyber_timer

            fired = threading.Event()

            cyber.init()

            def on_tick():
                fired.set()
                cyber.shutdown()

            timer = cyber_timer.Timer(10, on_tick, 0)
            timer.start()

            deadline = time.time() + 5
            while time.time() < deadline and not cyber.is_shutdown():
                time.sleep(0.01)

            if not fired.is_set():
                raise RuntimeError("timer callback did not run")
            if not cyber.is_shutdown():
                raise RuntimeError("shutdown from timer callback did not complete")
            """
        )

    def test_node_close_inside_reader_callback(self):
        self.run_child(
            """
            import threading
            import time

            from cyber.python.cyber_py3 import cyber
            from cyber.proto.unit_test_pb2 import ChatterBenchmark

            closed = threading.Event()

            cyber.init()
            node = cyber.Node("pycyber_reader_close_test")
            writer_node = cyber.Node("pycyber_writer_close_test")

            def on_message(message):
                node.close()
                closed.set()

            node.create_reader("channel/callback_close", ChatterBenchmark, on_message)
            writer = writer_node.create_writer(
                "channel/callback_close", ChatterBenchmark, 1
            )

            msg = ChatterBenchmark()
            msg.content = "callback close"
            msg.seq = 1

            deadline = time.time() + 5
            while time.time() < deadline and not closed.is_set():
                writer.write(msg)
                time.sleep(0.05)

            if not closed.is_set():
                raise RuntimeError("reader callback did not close node")

            writer_node.close()
            cyber.shutdown()
            """
        )


if __name__ == "__main__":
    unittest.main()
