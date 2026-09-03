#!/usr/bin/env python3

# Copyright 2026 WheelOS. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.

import json
import signal
import shutil
import subprocess
import types
import unittest
from unittest import mock
from pathlib import Path

from zero_loss_capacity import (
    ExecutionCoordinator,
    ModeExecutor,
    Outcome,
    ProbeBudgetExceeded,
    _run_benchmark_subprocess,
    build_parser,
    derive_outer_wall_timeout_s,
    evaluate_case,
    geometric_frequencies,
    search_boundary,
)


def _result(
    mode="protobuf",
    frequency_hz=100,
    payload_mib=1,
    measured_sent_messages=None,
    measured_received_messages=None,
    measured_send_duration_ns=5_000_000_000,
    measured_receive_duration_ns=5_000_000_000,
):
    if measured_sent_messages is None:
        measured_sent_messages = int(
            frequency_hz * measured_send_duration_ns / 1_000_000_000
        )
    if measured_received_messages is None:
        measured_received_messages = int(
            frequency_hz * measured_receive_duration_ns / 1_000_000_000
        )
    transport = "shm" if mode == "protobuf" else "iceoryx"
    notes = (
        "real_multi_process=true | transport_mode={} | "
        "pub_transport_mode_seen={} | sub_transport_mode_seen={} | "
        "loan_supported={} | zero_copy_borrowed_messages={} | "
        "zero_copy_copy_count=0"
    ).format(
        transport,
        transport,
        transport,
        "true" if mode == "pod" else "false",
        100 if mode == "pod" else 0,
    )
    return {
        "scenario": "zero_copy_vs_protobuf",
        "coverage": "inter_process",
        "message_type": mode,
        "topology": "1_pub_1_sub",
        "publishers": 1,
        "subscribers": 1,
        "frequency_hz": frequency_hz,
        "payload_bytes": payload_mib * 1024 * 1024,
        "duration_s": 5,
        "success": True,
        "notes": notes,
        "shm_loan_supported": mode == "pod",
        "execution": {
            "endpoints_ready": True,
            "warmup_confirmed": True,
            "measured_delivery_confirmed": True,
            "shutdown_confirmed": True,
            "commands": ["benchmark_pub", "benchmark_sub"],
        },
        "latency": {"measured_sample_count": 100},
        "throughput": {
            "measured_sent_messages": measured_sent_messages,
            "measured_received_messages": measured_received_messages,
            "measured_send_duration_ns": measured_send_duration_ns,
            "measured_receive_duration_ns": measured_receive_duration_ns,
            "send_failures": 0,
            "measured_send_failures": 0,
            "final_send_failures": 0,
            "loan_publish_successes": (
                measured_sent_messages if mode == "pod" else 0
            ),
            "measured_loan_publish_successes": (
                measured_sent_messages if mode == "pod" else 0
            ),
            "fallback_transmit_attempts": 0,
            "measured_fallback_transmit_attempts": 0,
            "fallback_transmit_successes": 0,
            "measured_fallback_transmit_successes": 0,
        },
        "reliability": {
            "loss_rate": 0,
            "total_loss": 0,
            "final_drained_total_loss": 0,
            "duplicates": 0,
            "reordered": 0,
            "duplicate_or_reordered": 0,
        },
    }


class FakeExecutor:
    def __init__(self, mode, payload_mib, decide, max_runs=None):
        self.mode = mode
        self.payload_mib = payload_mib
        self.decide = decide
        self.max_runs = max_runs
        self.history = []
        self.calls = []

    def probe(self, frequency_hz, purpose, force=False):
        del force
        if self.max_runs is not None and len(self.calls) >= self.max_runs:
            raise ProbeBudgetExceeded(
                "per_search",
                self.max_runs,
                len(self.calls),
                frequency_hz,
                purpose,
            )
        accepted = self.decide(frequency_hz, purpose, len(self.calls) + 1)
        outcome = Outcome(
            self.mode,
            self.payload_mib,
            frequency_hz,
            accepted,
            [] if accepted else ["synthetic loss"],
            purpose,
            len(self.calls) + 1,
        )
        self.calls.append((frequency_hz, purpose))
        self.history.append(outcome)
        return outcome


def _executor_args(max_runs_per_search=10):
    return types.SimpleNamespace(
        cpu_set="0",
        process_timeout_s=30,
        readiness_timeout_s=5,
        startup_wait_ms=0,
        cooldown_wait_ms=0,
        duration_s=5,
        min_achieved_rate_ratio=0.95,
        dry_run=False,
        max_runs_per_search=max_runs_per_search,
    )


class SearchAlgorithmTest(unittest.TestCase):
    def test_geometric_search_includes_integer_ceiling(self):
        self.assertEqual(
            geometric_frequencies(10, 95, 2.0),
            [10, 20, 40, 80, 95],
        )

    def test_reports_only_resolution_bounded_confirmed_boundary(self):
        executor = FakeExecutor(
            "protobuf", 1, lambda hz, _purpose, _run: hz <= 730
        )
        result = search_boundary(
            "protobuf",
            100,
            2000,
            2.0,
            3,
            executor.probe,
            executor.history,
            100,
            3,
        )
        self.assertEqual(result["status"], "resolution_bounded_boundary")
        self.assertEqual(result["confirmed_passing_lower_bound_hz"], 700)
        self.assertEqual(result["confirmed_failing_upper_bound_hz"], 800)
        self.assertEqual(result["boundary_width_hz"], 100)
        self.assertEqual(result["lower_confirmation"]["passes"], 3)
        self.assertEqual(result["upper_confirmation"]["failures"], 3)
        self.assertFalse(result["claimed_maximum"])
        self.assertNotIn("maximum_hz", result)

    def test_ceiling_is_only_a_confirmed_lower_bound(self):
        executor = FakeExecutor(
            "pod", 7, lambda _hz, _purpose, _run: True
        )
        result = search_boundary(
            "pod",
            10,
            40,
            2.0,
            3,
            executor.probe,
            executor.history,
            100,
            3,
        )
        self.assertEqual(result["status"], "confirmed_lower_bound")
        self.assertEqual(result["confirmed_passing_lower_bound_hz"], 40)
        self.assertEqual(result["lower_confirmation"]["pass_fraction"], 1.0)
        self.assertFalse(result["claimed_maximum"])

    def test_transient_upper_failure_that_later_passes_extends_search(self):
        attempts = {}

        def decide(hz, _purpose, _run):
            attempts[hz] = attempts.get(hz, 0) + 1
            if hz == 800 and attempts[hz] == 1:
                return False
            return hz <= 900

        executor = FakeExecutor("protobuf", 1, decide)
        result = search_boundary(
            "protobuf",
            100,
            1600,
            2.0,
            3,
            executor.probe,
            executor.history,
            100,
            3,
        )
        self.assertEqual(result["status"], "resolution_bounded_boundary")
        self.assertGreaterEqual(
            result["confirmed_passing_lower_bound_hz"], 800
        )
        self.assertGreater(
            result["confirmed_failing_upper_bound_hz"],
            result["confirmed_passing_lower_bound_hz"],
        )
        self.assertTrue(
            any(
                item.get("reason")
                == "transient_failure_confirmed_passing"
                for item in result["unstable_candidates"]
            )
        )

    def test_mixed_upper_confirmation_is_inconclusive(self):
        upper_confirmation_count = 0

        def decide(hz, purpose, _run):
            nonlocal upper_confirmation_count
            if hz < 800:
                return True
            if purpose == "upper_bound_confirmation":
                upper_confirmation_count += 1
                return upper_confirmation_count == 2
            return False

        executor = FakeExecutor("pod", 4, decide)
        result = search_boundary(
            "pod",
            100,
            1600,
            2.0,
            3,
            executor.probe,
            executor.history,
            100,
            3,
        )
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(result["reason"], "upper_bound_confirmation_mixed")
        self.assertNotIn("maximum_hz", result)

    def test_stale_conflicting_outcomes_are_all_retained(self):
        attempts = {}

        def decide(hz, purpose, _run):
            attempts[hz] = attempts.get(hz, 0) + 1
            if hz == 700 and purpose == "lower_bound_confirmation":
                return attempts[hz] != 2
            return hz <= 700

        executor = FakeExecutor("protobuf", 1, decide)
        result = search_boundary(
            "protobuf",
            100,
            1600,
            2.0,
            3,
            executor.probe,
            executor.history,
            100,
            2,
        )
        point = next(
            point
            for point in result["tested_points"]
            if point["frequency_hz"] == 700
        )
        self.assertGreater(point["runs"], 1)
        self.assertGreater(point["passes"], 0)
        self.assertLess(point["passes"], point["runs"])
        self.assertNotEqual(result["status"], "stable_maximum")

    def test_per_search_budget_exhaustion_is_inconclusive(self):
        executor = FakeExecutor(
            "protobuf",
            7,
            lambda _hz, _purpose, _run: True,
            max_runs=4,
        )
        result = search_boundary(
            "protobuf",
            100,
            3200,
            2.0,
            3,
            executor.probe,
            executor.history,
            100,
            3,
        )
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(result["reason"], "per_search_run_budget_exceeded")
        self.assertEqual(result["budget"]["completed_runs"], 4)
        self.assertTrue(result["tested_points"])


class ModeExecutorTest(unittest.TestCase):
    def test_default_mode_selection_is_both(self):
        args = build_parser().parse_args([])
        self.assertEqual(args.comparison_message_type, "both")
        self.assertEqual(args.min_achieved_rate_ratio, 0.95)

    def test_probe_launches_and_records_only_selected_mode(self):
        calls = []

        def runner(command, _output_json, _output_log):
            calls.append(command)
            return 0, {"results": [_result("protobuf")]}, None

        coordinator = ExecutionCoordinator(0)
        executor = ModeExecutor(
            _executor_args(),
            Path("."),
            Path(".copilot-artifacts/capacity-unit"),
            Path("benchmark_monitor"),
            {},
            1,
            "protobuf",
            coordinator,
            runner,
        )
        outcome = executor.probe(100, "test")
        self.assertTrue(outcome.accepted)
        self.assertEqual([item.mode for item in executor.history], ["protobuf"])
        self.assertEqual(coordinator.run_records[0]["mode"], "protobuf")
        self.assertEqual(
            coordinator.run_records[0]["mode_result"]["rate_evidence"][
                "target_frequency_hz"
            ],
            100,
        )
        self.assertIn("--comparison_message_type=protobuf", calls[0])
        self.assertIn("--min_achieved_rate_ratio=0.95", calls[0])
        self.assertNotIn("--comparison_message_type=pod", calls[0])

    def test_outer_timeout_budget_covers_readiness_and_worker_phases(self):
        self.assertEqual(derive_outer_wall_timeout_s(_executor_args()), 50)

    def test_timeout_terminates_exact_process_group(self):
        class FakeProcess:
            pid = 4321

            def __init__(self):
                self.wait_calls = 0

            def wait(self, timeout):
                self.wait_calls += 1
                if self.wait_calls <= 2:
                    raise subprocess.TimeoutExpired(["benchmark"], timeout)
                return -signal.SIGKILL

        output_dir = Path(".copilot-artifacts/capacity-unit")
        output_dir.mkdir(parents=True, exist_ok=True)
        output_log = output_dir / "timeout-process-group.log"
        fake_process = FakeProcess()
        try:
            with mock.patch(
                "zero_loss_capacity.subprocess.Popen",
                return_value=fake_process,
            ) as popen, mock.patch(
                "zero_loss_capacity.os.killpg"
            ) as killpg:
                exit_code, evidence = _run_benchmark_subprocess(
                    ["benchmark"],
                    Path("."),
                    {},
                    output_log,
                    7,
                )
            self.assertEqual(exit_code, 124)
            self.assertTrue(evidence["timed_out"])
            self.assertEqual(evidence["process_group_id"], 4321)
            self.assertEqual(
                evidence["termination_signals"], ["SIGTERM", "SIGKILL"]
            )
            self.assertEqual(
                killpg.call_args_list,
                [
                    mock.call(4321, signal.SIGTERM),
                    mock.call(4321, signal.SIGKILL),
                ],
            )
            self.assertTrue(
                popen.call_args.kwargs["start_new_session"]
            )
        finally:
            output_log.unlink(missing_ok=True)

    def test_timeout_is_rejected_and_next_bounded_probe_continues(self):
        output_dir = Path(".copilot-artifacts/capacity-timeout-unit")
        shutil.rmtree(output_dir, ignore_errors=True)
        calls = 0

        def run_process(command, _cwd, _environment, _output_log, timeout_s):
            nonlocal calls
            calls += 1
            evidence = {
                "timed_out": calls == 1,
                "outer_wall_timeout_s": timeout_s,
                "process_group_id": 5000 + calls,
                "termination_signals": ["SIGTERM"] if calls == 1 else [],
            }
            if calls == 1:
                return 124, evidence
            output_json = Path(
                next(
                    argument.split("=", 1)[1]
                    for argument in command
                    if argument.startswith("--output_json=")
                )
            )
            output_json.write_text(
                json.dumps({"results": [_result("protobuf", frequency_hz=100)]}),
                encoding="utf-8",
            )
            return 0, evidence

        executor = ModeExecutor(
            _executor_args(max_runs_per_search=2),
            Path("."),
            output_dir,
            Path("benchmark_monitor"),
            {},
            1,
            "protobuf",
            ExecutionCoordinator(0),
        )
        try:
            with mock.patch(
                "zero_loss_capacity._run_benchmark_subprocess",
                side_effect=run_process,
            ):
                timed_out = executor.probe(100, "search")
                completed = executor.probe(100, "confirmation")
            self.assertFalse(timed_out.accepted)
            self.assertIn(
                "benchmark outer wall timeout after 50s",
                timed_out.reasons,
            )
            self.assertTrue(timed_out.timeout_evidence["timed_out"])
            self.assertTrue(completed.accepted, completed.reasons)
            self.assertTrue(
                executor.coordinator.run_records[0]["timeout_evidence"][
                    "timed_out"
                ]
            )
            self.assertEqual(len(executor.history), 2)
        finally:
            shutil.rmtree(output_dir, ignore_errors=True)

    def test_saturated_zero_loss_target_is_a_failing_upper_point(self):
        def runner(command, _output_json, _output_log):
            frequency_hz = int(
                next(
                    argument.split("=", 1)[1]
                    for argument in command
                    if argument.startswith("--scaling_frequency_hz=")
                )
            )
            achieved_hz = min(frequency_hz, 8_000)
            return (
                0,
                {
                    "results": [
                        _result(
                            "protobuf",
                            frequency_hz,
                            measured_sent_messages=achieved_hz,
                            measured_received_messages=achieved_hz,
                            measured_send_duration_ns=1_000_000_000,
                            measured_receive_duration_ns=1_000_000_000,
                        )
                    ]
                },
                None,
            )

        executor = ModeExecutor(
            _executor_args(max_runs_per_search=10),
            Path("."),
            Path(".copilot-artifacts/capacity-unit"),
            Path("benchmark_monitor"),
            {},
            1,
            "protobuf",
            ExecutionCoordinator(0),
            runner,
        )
        result = search_boundary(
            "protobuf",
            5_000,
            10_000,
            2.0,
            3,
            executor.probe,
            executor.history,
            5_000,
            1,
        )
        self.assertEqual(result["status"], "resolution_bounded_boundary")
        self.assertEqual(result["confirmed_passing_lower_bound_hz"], 5_000)
        self.assertEqual(result["confirmed_failing_upper_bound_hz"], 10_000)
        upper = next(
            point
            for point in result["tested_points"]
            if point["frequency_hz"] == 10_000
        )
        self.assertEqual(upper["passes"], 0)
        for evidence in upper["rate_evidence"]:
            self.assertEqual(
                evidence["saturation_reason"],
                "send_and_receive_rates_below_target_ratio",
            )
            self.assertEqual(
                evidence["observed_zero_loss_throughput_hz"], 8_000
            )

    def test_every_probe_executes_once_without_cache_conflict_resolution(self):
        calls = 0

        def runner(_command, _output_json, _output_log):
            nonlocal calls
            calls += 1
            result = _result("pod")
            if calls == 2:
                result["reliability"]["total_loss"] = 1
            return 0, {"results": [result]}, None

        executor = ModeExecutor(
            _executor_args(),
            Path("."),
            Path(".copilot-artifacts/capacity-unit"),
            Path("benchmark_monitor"),
            {},
            1,
            "pod",
            ExecutionCoordinator(0),
            runner,
        )
        first = executor.probe(100, "search")
        second = executor.probe(100, "confirmation")
        self.assertEqual(calls, 2)
        self.assertTrue(first.accepted)
        self.assertFalse(second.accepted)
        self.assertEqual(len(executor.history), 2)

    def test_per_search_budgets_are_independent(self):
        def runner(_command, _output_json, _output_log):
            return 0, {"results": [_result("protobuf")]}, None

        coordinator = ExecutionCoordinator(0)
        first = ModeExecutor(
            _executor_args(max_runs_per_search=1),
            Path("."),
            Path(".copilot-artifacts/capacity-unit"),
            Path("benchmark_monitor"),
            {},
            1,
            "protobuf",
            coordinator,
            runner,
        )
        second = ModeExecutor(
            _executor_args(max_runs_per_search=1),
            Path("."),
            Path(".copilot-artifacts/capacity-unit"),
            Path("benchmark_monitor"),
            {},
            4,
            "protobuf",
            coordinator,
            runner,
        )
        first.probe(100, "search")
        with self.assertRaises(ProbeBudgetExceeded) as raised:
            first.probe(200, "search")
        self.assertEqual(raised.exception.budget_scope, "per_search")
        second.probe(100, "search")
        self.assertEqual(coordinator.completed_runs, 2)

    def test_optional_global_budget_caps_independent_searches(self):
        def runner(_command, _output_json, _output_log):
            return 0, {"results": [_result("protobuf")]}, None

        coordinator = ExecutionCoordinator(1)
        first = ModeExecutor(
            _executor_args(),
            Path("."),
            Path(".copilot-artifacts/capacity-unit"),
            Path("benchmark_monitor"),
            {},
            1,
            "protobuf",
            coordinator,
            runner,
        )
        second = ModeExecutor(
            _executor_args(),
            Path("."),
            Path(".copilot-artifacts/capacity-unit"),
            Path("benchmark_monitor"),
            {},
            4,
            "protobuf",
            coordinator,
            runner,
        )
        first.probe(100, "search")
        with self.assertRaises(ProbeBudgetExceeded) as raised:
            second.probe(100, "search")
        self.assertEqual(raised.exception.budget_scope, "global")

    def test_targeted_json_has_exactly_one_matching_result(self):
        def runner(command, _output_json, _output_log):
            self.assertIn("--comparison_message_type=pod", command)
            document = {"results": [_result("pod")]}
            matching = [
                result
                for result in document["results"]
                if result["message_type"] == "pod"
            ]
            self.assertEqual(len(document["results"]), 1)
            self.assertEqual(len(matching), 1)
            return 0, document, None

        executor = ModeExecutor(
            _executor_args(),
            Path("."),
            Path(".copilot-artifacts/capacity-unit"),
            Path("benchmark_monitor"),
            {},
            1,
            "pod",
            ExecutionCoordinator(0),
            runner,
        )
        self.assertTrue(executor.probe(100, "test").accepted)


class ResultParsingTest(unittest.TestCase):
    def test_accepts_strict_protobuf_and_pod_results(self):
        for mode in ("protobuf", "pod"):
            evaluation = evaluate_case(
                {"results": [_result(mode)]}, mode, 1, 100, 5
            )
            self.assertTrue(evaluation["accepted"], evaluation["reasons"])

    def test_rejects_loss_and_missing_pod_zero_copy_evidence(self):
        result = _result("pod")
        result["reliability"]["total_loss"] = 1
        result["notes"] = result["notes"].replace(
            "zero_copy_borrowed_messages=100",
            "zero_copy_borrowed_messages=0",
        )
        evaluation = evaluate_case({"results": [result]}, "pod", 1, 100, 5)
        self.assertFalse(evaluation["accepted"])
        self.assertIn("reliability.total_loss != 0", evaluation["reasons"])
        self.assertIn("zero_copy_borrowed_messages <= 0", evaluation["reasons"])

    def test_rejects_pod_publisher_fallback_or_incomplete_loan_publish(self):
        result = _result("pod")
        result["throughput"]["measured_loan_publish_successes"] -= 1
        result["throughput"]["measured_fallback_transmit_attempts"] = 1
        result["throughput"]["measured_fallback_transmit_successes"] = 1

        evaluation = evaluate_case({"results": [result]}, "pod", 1, 100, 5)

        self.assertFalse(evaluation["accepted"])
        self.assertIn(
            "loan_publish_successes != measured sends",
            evaluation["reasons"],
        )
        self.assertIn(
            "fallback_transmit_attempts != 0",
            evaluation["reasons"],
        )
        self.assertIn(
            "fallback_transmit_successes != 0",
            evaluation["reasons"],
        )

    def test_rejects_zero_loss_target_when_actual_rate_plateaus(self):
        result = _result(
            frequency_hz=10_000,
            measured_sent_messages=10_000,
            measured_received_messages=10_000,
            measured_send_duration_ns=1_250_000_000,
            measured_receive_duration_ns=1_250_000_000,
        )
        evaluation = evaluate_case(
            {"results": [result]}, "protobuf", 1, 10_000, 5, 0.95
        )
        self.assertFalse(evaluation["accepted"])
        evidence = evaluation["rate_evidence"]
        self.assertEqual(evidence["achieved_send_hz"], 8_000)
        self.assertEqual(evidence["achieved_receive_hz"], 8_000)
        self.assertEqual(
            evidence["saturation_reason"],
            "send_and_receive_rates_below_target_ratio",
        )
        self.assertEqual(
            evidence["observed_zero_loss_throughput_hz"], 8_000
        )
        self.assertEqual(
            evidence["send_rate_source"],
            "throughput.measured_sent_messages/"
            "measured_send_duration_ns",
        )

    def test_accepts_exact_rate_ratio_threshold(self):
        result = _result(
            frequency_hz=10_000,
            measured_sent_messages=9_500,
            measured_received_messages=9_500,
            measured_send_duration_ns=1_000_000_000,
            measured_receive_duration_ns=1_000_000_000,
        )
        evaluation = evaluate_case(
            {"results": [result]}, "protobuf", 1, 10_000, 5, 0.95
        )
        self.assertTrue(evaluation["accepted"], evaluation["reasons"])
        self.assertEqual(
            evaluation["rate_evidence"]["achieved_send_ratio"], 0.95
        )
        self.assertEqual(
            evaluation["rate_evidence"]["achieved_receive_ratio"], 0.95
        )

    def test_accepts_exact_achieved_target(self):
        result = _result(
            frequency_hz=10_000,
            measured_sent_messages=10_000,
            measured_received_messages=10_000,
            measured_send_duration_ns=1_000_000_000,
            measured_receive_duration_ns=1_000_000_000,
        )
        evaluation = evaluate_case(
            {"results": [result]}, "protobuf", 1, 10_000, 5, 0.95
        )
        self.assertTrue(evaluation["accepted"], evaluation["reasons"])
        evidence = evaluation["rate_evidence"]
        self.assertEqual(evidence["achieved_send_hz"], 10_000)
        self.assertEqual(evidence["achieved_receive_hz"], 10_000)
        self.assertIsNone(evidence["saturation_reason"])

    def test_pair_results_are_classified_independently(self):
        protobuf = _result("protobuf")
        pod = _result("pod")
        pod["throughput"]["send_failures"] = 1
        document = {"results": [protobuf, pod]}
        self.assertTrue(
            evaluate_case(document, "protobuf", 1, 100, 5)["accepted"]
        )
        self.assertFalse(evaluate_case(document, "pod", 1, 100, 5)["accepted"])

    def test_ignores_non_executable_comparison_summary(self):
        real_result = _result("protobuf")
        summary_result = dict(real_result)
        summary_result["execution"] = {"commands": []}
        evaluation = evaluate_case(
            {"results": [real_result, summary_result]}, "protobuf", 1, 100, 5
        )
        self.assertTrue(evaluation["accepted"], evaluation["reasons"])


if __name__ == "__main__":
    unittest.main()
