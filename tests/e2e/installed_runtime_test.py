#!/usr/bin/env python3
"""Package-only acceptance; /cases and /fixtures never supply runtime libraries.

Run: python3 /cases/installed_runtime_test.py --artifacts /artifacts
     --fixtures /fixtures --outdir /results
Artifacts contain core/*.deb, pycyber/*.whl, manifest.txt and SHA256SUMS.
The outdir must be writable and may be reused; each run gets a private work tree.
--wheel-only explicitly limits qualification to Python pub/sub, RPC and record
APIs, without claiming native recorder, mainboard or Debian package coverage.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import traceback
import uuid
from pathlib import Path


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def select_artifacts(root, wheel_only=False):
    root = Path(root).resolve()
    require((root / "manifest.txt").is_file(), "missing manifest.txt")
    selected = []
    patterns = ("pycyber/*.whl",) if wheel_only else ("core/*.deb", "pycyber/*.whl")
    for pattern in patterns:
        candidates = sorted(root.glob(pattern))
        require(len(candidates) == 1, f"expected exactly one {pattern}: {candidates}")
        selected.append(candidates[0])
    checksums = {}
    for line in (root / "SHA256SUMS").read_text().splitlines():
        digest, relative = line.split(maxsplit=1)
        relative = relative.lstrip("*")
        path = (root / relative).resolve()
        require(
            path.is_relative_to(root), f"checksum escapes artifact root: {relative}"
        )
        require(re.fullmatch(r"[a-fA-F0-9]{64}", digest), "invalid SHA256 digest")
        require(path not in checksums, f"duplicate checksum entry: {relative}")
        checksums[path] = digest.lower()
        hasher = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                hasher.update(chunk)
        require(hasher.hexdigest() == digest.lower(), f"checksum mismatch: {relative}")
    sdists = list(root.glob("pycyber/pycyber-*.tar.gz"))
    require(len(sdists) <= 1, f"expected at most one pycyber sdist: {sdists}")
    for path in selected + sdists:
        require(path.resolve() in checksums, f"artifact not checksummed: {path.name}")
    return tuple(selected)


class Processes:
    def __init__(self, work, env):
        self.work = work
        self.env = env
        self.children = []

    def start(self, name, command, env=None):
        log_path = self.work / f"{name}.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [str(arg) for arg in command],
                cwd=self.work,
                env=env or self.env,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        self.children.append(process)
        process.e2e_log = log_path
        return process

    def wait(self, process, timeout=30):
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.stop(process, strict=False)
            raise AssertionError(
                f"process timed out; log: {process.e2e_log}"
            ) from error
        require(code == 0, f"exit {code}; log: {process.e2e_log}")

    def stop(self, process, strict=True):
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGINT)
            except ProcessLookupError:
                pass
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
            if strict:
                raise AssertionError(f"SIGINT timeout; log: {process.e2e_log}")
        if strict:
            require(
                process.returncode == 0,
                f"unclean stop {process.returncode}; log: {process.e2e_log}",
            )

    def close(self):
        errors = []
        children, self.children = self.children, []
        for process in reversed(children):
            try:
                self.stop(process, strict=False)
            except Exception as error:
                errors.append(str(error))
        require(not errors, "child cleanup failed: " + "; ".join(errors))


def wait_for(predicate, processes=(), timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        for process in processes:
            require(
                process.poll() is None,
                f"premature exit {process.returncode}; log: {process.e2e_log}",
            )
        time.sleep(0.05)
    raise AssertionError("readiness deadline exceeded")


class Harness:
    def __init__(self, artifacts, fixtures, outdir, wheel_only=False):
        self.wheel_only = wheel_only
        self.artifacts = Path(artifacts).resolve()
        self.fixtures = Path(fixtures).resolve()
        self.outdir = Path(outdir).resolve()
        self.outdir.mkdir(parents=True, exist_ok=True)
        self.token = uuid.uuid4().hex
        self.work = self.outdir / ("work-" + self.token)
        self.work.mkdir()
        self.env = {
            key: value
            for key, value in os.environ.items()
            if key
            not in {
                "PYTHONPATH",
                "PYTHONHOME",
                "LD_LIBRARY_PATH",
                "CYBER_PATH",
                "APOLLO_ROOT_DIR",
                "WHEELOS_CONFIG_ROOT",
                "WHEELOS_ASSET_ROOT",
                "RUNFILES_DIR",
                "RUNFILES_MANIFEST_FILE",
            }
        }
        self.env.update(
            HOME=str(self.work / "home"),
            TMPDIR=str(self.work / "scratch"),
            PYTHONNOUSERSITE="1",
            PYTHONUNBUFFERED="1",
            CYBER_IP="127.0.0.1",
            CYBER_DOMAIN_ID=str(20 + int(self.token[:4], 16) % 180),
            GLOG_alsologtostderr="1",
            GLOG_log_dir=str(self.work / "log"),
        )
        for name in ("home", "scratch", "log"):
            (self.work / name).mkdir()
        self.processes = Processes(self.work, self.env)
        self.python = self.work / "venv/bin/python"
        self.worker_script = Path(__file__).resolve().with_name("runtime_worker.py")

    def command(self, name, command, timeout=30, env=None):
        process = self.processes.start(name, command, env)
        self.processes.wait(process, timeout)
        return process.e2e_log.read_text(errors="replace")

    def install(self):
        artifacts = select_artifacts(self.artifacts, self.wheel_only)
        wheel = artifacts[-1]
        if not self.wheel_only:
            deb = artifacts[0]
            prefix = self.work / "native"
            self.command("extract", ["dpkg-deb", "-x", deb, prefix])
            self.native = prefix / "opt/wheelos_core"
            setup = self.native / "setup.bash"
            require(setup.is_file(), "native artifact does not contain setup.bash")
            sourced = subprocess.run(
                ["bash", "-c", 'source "$1" && env -0', "e2e", str(setup)],
                env=self.env,
                cwd=self.work,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=15,
                check=True,
            )
            self.env = dict(
                entry.decode().split("=", 1)
                for entry in sourced.stdout.split(b"\0")
                if entry and b"=" in entry
            )
            software_root = self.env.get("APOLLO_ROOT_DIR", "")
            require(
                software_root and Path(software_root).is_absolute(),
                "installed setup.bash must export an absolute APOLLO_ROOT_DIR; "
                "ResourceManager otherwise defaults to /apollo",
            )
            require(
                Path(software_root).resolve() == self.native.resolve(),
                "installed setup.bash APOLLO_ROOT_DIR must select the extracted "
                f"installation prefix: {self.native}, got {software_root}",
            )
            self.processes.env = self.env
            for tool in ("cyber_recorder", "mainboard"):
                require(
                    (self.native / "bin" / tool).is_file(), f"missing installed {tool}"
                )
        self.command("venv", [sys.executable, "-m", "venv", self.work / "venv"])
        self.command(
            "pip",
            [
                self.python,
                "-m",
                "pip",
                "install",
                "--disable-pip-version-check",
                "--no-input",
                wheel,
            ],
            timeout=300,
        )
        if self.wheel_only:
            configs = list(
                (self.work / "venv").glob("lib/python*/site-packages/pycyber/conf")
            )
            require(len(configs) == 1, "wheel must contain exactly one runtime config")
            self.resources = self.work / "wheel-resources"
            shutil.copytree(configs[0], self.resources / "conf")
        else:
            self.resources = self.native / "resources/cyber"
        self.artifact_names = {"pycyber": wheel.name}
        if not self.wheel_only:
            self.artifact_names["native"] = deb.name
        sdists = list(self.artifacts.glob("pycyber/pycyber-*.tar.gz"))
        if sdists:
            self.install_sdist(sdists[0])
            self.artifact_names["sdist"] = {
                "name": sdists[0].name,
                "fresh_venv_import": True,
            }
        return self.artifact_names

    def install_sdist(self, sdist):
        venv = self.work / "sdist-venv"
        python = venv / "bin/python"
        self.command("sdist-venv", [sys.executable, "-m", "venv", venv])
        self.command(
            "sdist-pip",
            [
                python,
                "-m",
                "pip",
                "install",
                "--disable-pip-version-check",
                "--no-input",
                sdist,
            ],
            timeout=300,
        )
        wheel_version = self.command(
            "wheel-version",
            [
                self.python,
                "-c",
                "from importlib.metadata import version; print(version('pycyber'))",
            ],
        ).strip()
        self.command(
            "sdist-import",
            [
                python,
                "-c",
                "import sys; from pathlib import Path; "
                "from importlib.metadata import version; "
                "import pycyber; "
                "from pycyber import cyber, cyber_time, cyber_timer, "
                "parameter, record; "
                "from pycyber.proto import record_pb2; "
                "assert Path(pycyber.__file__).resolve().is_relative_to("
                "Path(sys.prefix).resolve()); "
                "assert version('pycyber') == sys.argv[1], version('pycyber')",
                wheel_version,
            ],
        )

    def mode_environment(self, mode):
        target = self.work / mode / "cyber"
        shutil.copytree(self.resources, target)
        config = target / "conf/cyber.pb.conf"
        text, changes = re.subn(
            r"\bdiff_proc\s*:\s*[A-Z_]+", "diff_proc: " + mode, config.read_text()
        )
        require(changes == 1, "cannot select explicit diff_proc transport")
        config.write_text(text)
        return {**self.env, "CYBER_PATH": str(target)}

    def worker(self, role, name, env, channel, token, **extra):
        base = self.work / name
        options = {
            "role": role,
            "token": token,
            "channel": channel,
            "venv": self.work / "venv",
            "ready": base.with_suffix(".ready"),
            "connected": base.with_suffix(".connected"),
            "gate": base.with_suffix(".gate"),
            "result": base.with_suffix(".json"),
            **extra,
        }
        command = [self.python, self.worker_script]
        for option, value in options.items():
            command.extend(["--" + option, value])
        return self.processes.start(name, command, env), options

    def pubsub(self, mode, record_native=False):
        env = self.mode_environment(mode)
        token = self.token + "_" + mode
        channel = "/wheelos/e2e/" + token
        recording = self.work / "native.record"
        recorder = None
        if record_native:
            recorder = self.processes.start(
                "recorder",
                [
                    self.native / "bin/cyber_recorder",
                    "record",
                    "-o",
                    recording,
                    "-c",
                    channel,
                    "-i",
                    "0",
                    "-m",
                    "0",
                ],
                env,
            )
        subscriber, sub = self.worker(
            "subscriber", "subscriber_" + mode, env, channel, token
        )
        publisher, pub = self.worker(
            "publisher", "publisher_" + mode, env, channel, token
        )
        active = [subscriber, publisher] + ([recorder] if recorder else [])
        wait_for(lambda: Path(sub["connected"]).exists(), active)
        if recorder:
            wait_for(
                lambda: re.search(
                    r"\b1 channels, [1-9]\d* messages",
                    recorder.e2e_log.read_text(errors="replace"),
                ),
                active,
            )
        Path(pub["gate"]).touch()
        self.processes.wait(publisher)
        self.processes.wait(subscriber)
        received = json.loads(Path(sub["result"]).read_text())
        require(received["sequences"] == list(range(1, 41)), "subscriber sequence")
        if recorder:
            self.processes.stop(recorder)
            require(recording.is_file(), "native recorder produced no file")
            info = self.command(
                "record-info",
                [self.native / "bin/cyber_recorder", "info", recording],
                env=env,
            )
            require(
                channel in info, "native record info does not list recorded channel"
            )
            inspector, inspection = self.worker(
                "inspect-record",
                "record-inspect",
                env,
                channel,
                token,
                record=recording,
            )
            self.processes.wait(inspector)
            replay, replay_options = self.worker(
                "subscriber", "replay-subscriber", env, channel, token
            )
            wait_for(lambda: Path(replay_options["ready"]).exists(), [replay])
            player = self.processes.start(
                "record-play",
                [
                    self.native / "bin/cyber_recorder",
                    "play",
                    "-f",
                    recording,
                    "-d",
                    "3",
                ],
                env,
            )
            wait_for(
                lambda: Path(replay_options["connected"]).exists(), [replay, player]
            )
            self.processes.wait(player, timeout=40)
            self.processes.wait(replay)
            require(
                json.loads(Path(inspection["result"]).read_text())
                == {
                    "sequences": received["sequences"],
                    "message_type": "google.protobuf.BytesValue",
                },
                "record differs from live received payload",
            )
            require(
                json.loads(Path(replay_options["result"]).read_text()) == received,
                "replay differs from live received sequence",
            )
        return {"diff_proc": mode, "messages": 40, "binary_payload": True}

    def rpc(self):
        env = {**self.env, "CYBER_PATH": str(self.work / "RTPS/cyber")}
        channel = "/wheelos/e2e/rpc/" + self.token
        service, server = self.worker("service", "service", env, channel, self.token)
        wait_for(lambda: Path(server["ready"]).exists(), [service])
        client, result = self.worker("client", "client", env, channel, self.token)
        self.processes.wait(client, timeout=75)
        self.processes.stop(service)
        replies = json.loads(Path(result["result"]).read_text())
        require(replies == {"replies": [1, 2, 3, 4, 5]}, "RPC response count")
        return replies

    def wheel_record(self):
        env = {**self.env, "CYBER_PATH": str(self.work / "RTPS/cyber")}
        channel = "/wheelos/e2e/wheel_record/" + self.token
        worker, options = self.worker(
            "wheel-record",
            "wheel-record",
            env,
            channel,
            self.token,
            record=self.work / "wheel.record",
        )
        self.processes.wait(worker)
        result = json.loads(Path(options["result"]).read_text())
        require(result["sequences"] == list(range(1, 41)), "wheel record sequence")
        return result

    def mainboard(self):
        library = self.fixtures / "libinstalled_e2e_component.so"
        require(library.is_file(), f"missing separately staged fixture: {library}")
        marker = self.work / "component-ticks.txt"
        dag = self.native / "resources/cyber/dag/installed.dag"
        dag.parent.mkdir(parents=True, exist_ok=True)
        dag.write_text(
            "module_config {\n"
            f"  module_library: {json.dumps(str(library))}\n"
            "  timer_components {\n"
            '    class_name: "InstalledE2EComponent"\n'
            f'    config {{ name: "installed_{self.token}" interval: 20 }}\n'
            "  }\n}\n"
        )
        env = {**self.env, "WHEELOS_E2E_COMPONENT_MARKER": str(marker)}
        mainboard = self.processes.start(
            "mainboard",
            [
                self.native / "bin/mainboard",
                "--process_name",
                "installed_" + self.token,
                "-d",
                dag,
            ],
            env,
        )
        wait_for(
            lambda: marker.exists() and len(marker.read_text().splitlines()) >= 5,
            [mainboard],
        )
        self.processes.stop(mainboard)
        lines = marker.read_text().splitlines()
        require(
            [int(line) for line in lines] == list(range(1, len(lines) + 1)), "ticks"
        )
        time.sleep(0.1)
        require(
            marker.read_text().splitlines() == lines, "component survived mainboard"
        )
        return {"ticks": len(lines), "clean_sigint": True}


def run_suite(harness):
    results = []
    scenarios = [
        ("install_artifacts", harness.install),
        (
            "rtps_pubsub" if harness.wheel_only else "rtps_pubsub_record_replay",
            lambda: harness.pubsub("RTPS", record_native=not harness.wheel_only),
        ),
        ("shm_pubsub", lambda: harness.pubsub("SHM")),
        ("rpc", harness.rpc),
        (
            ("wheel_record_write_read", harness.wheel_record)
            if harness.wheel_only
            else ("mainboard_component_lifecycle", harness.mainboard)
        ),
    ]
    for name, run in scenarios:
        started = time.monotonic()
        try:
            details = run()
            result = {"name": name, "status": "passed", "details": details}
        except Exception as error:
            result = {
                "name": name,
                "status": "failed",
                "error": str(error),
                "traceback": traceback.format_exc(),
            }
        finally:
            try:
                harness.processes.close()
            except Exception as error:
                result = {
                    "name": name,
                    "status": "failed",
                    "error": f"cleanup failed: {error}",
                    "traceback": traceback.format_exc(),
                }
        result["duration_seconds"] = round(time.monotonic() - started, 3)
        results.append(result)
        if name == "install_artifacts" and result["status"] == "failed":
            break
    passed = len(results) == len(scenarios) and all(
        item["status"] == "passed" for item in results
    )
    report = {
        "schema_version": 1,
        "status": "passed" if passed else "failed",
        "overall_status": "passed" if passed else "failed",
        "scope": "wheel-only" if harness.wheel_only else "full",
        "package_only": True,
        "workdir": str(harness.work),
        "scenarios": results,
    }
    (harness.outdir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if passed else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("artifacts", "fixtures", "outdir"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument(
        "--wheel-only",
        action="store_true",
        help="qualify only installed wheel APIs; explicitly exclude native scenarios",
    )
    args = parser.parse_args(argv)
    harness = Harness(args.artifacts, args.fixtures, args.outdir, args.wheel_only)
    try:
        return run_suite(harness)
    finally:
        harness.processes.close()


if __name__ == "__main__":
    sys.exit(main())
