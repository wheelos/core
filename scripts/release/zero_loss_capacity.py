#!/usr/bin/env python3

# Copyright 2026 WheelOS. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.

import argparse
import dataclasses
import datetime
import json
import math
import os
import platform
import signal
import shlex
import subprocess
from pathlib import Path


MODES = ("protobuf", "pod")
MIB = 1024 * 1024


@dataclasses.dataclass
class Outcome:
    mode: str
    payload_mib: int
    frequency_hz: int
    accepted: bool
    reasons: list
    purpose: str
    run_id: int
    result: dict | None = None
    rate_evidence: dict | None = None
    timeout_evidence: dict | None = None


class ProbeBudgetExceeded(RuntimeError):
    def __init__(
        self, budget_scope, max_runs, completed_runs, frequency_hz, purpose
    ):
        super().__init__("{} run budget exceeded".format(budget_scope))
        self.budget_scope = budget_scope
        self.max_runs = max_runs
        self.completed_runs = completed_runs
        self.frequency_hz = frequency_hz
        self.purpose = purpose


def geometric_frequencies(start_hz, ceiling_hz, growth_factor):
    frequencies = []
    current = start_hz
    while True:
        frequencies.append(current)
        if current >= ceiling_hz:
            return frequencies
        current = min(
            ceiling_hz,
            max(current + 1, int(math.ceil(current * growth_factor))),
        )


def parse_notes(notes):
    fields = {}
    for item in notes.split("|"):
        item = item.strip()
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        fields[key.strip()] = value.strip()
    return fields


def _require(condition, message, reasons):
    if not condition:
        reasons.append(message)


def _positive_finite(value):
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) and parsed > 0 else None


def _measured_rate(throughput, rate_field, count_field, duration_field):
    explicit_rate = _positive_finite(throughput.get(rate_field))
    if explicit_rate is not None:
        return explicit_rate, "throughput.{}".format(rate_field)
    count = _positive_finite(throughput.get(count_field))
    duration_ns = _positive_finite(throughput.get(duration_field))
    if count is None or duration_ns is None:
        return None, None
    return count * 1e9 / duration_ns, "throughput.{}/{}".format(
        count_field, duration_field
    )


def derive_outer_wall_timeout_s(args):
    readiness_budget_s = 2 * args.readiness_timeout_s
    scheduled_case_budget_s = (
        args.duration_s
        + math.ceil(args.startup_wait_ms / 1000)
        + math.ceil(args.cooldown_wait_ms / 1000)
        + 20
    )
    worker_budget_s = max(args.process_timeout_s, scheduled_case_budget_s)
    return readiness_budget_s + worker_budget_s + 10


def _run_benchmark_subprocess(
    command, cwd, environment, output_log, timeout_s
):
    evidence = {
        "timed_out": False,
        "outer_wall_timeout_s": timeout_s,
        "process_group_id": None,
        "termination_signals": [],
    }
    with output_log.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env={**os.environ, **environment},
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
        evidence["process_group_id"] = process.pid
        try:
            return process.wait(timeout=timeout_s), evidence
        except subprocess.TimeoutExpired:
            evidence["timed_out"] = True
            try:
                os.killpg(process.pid, signal.SIGTERM)
                evidence["termination_signals"].append("SIGTERM")
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                    evidence["termination_signals"].append("SIGKILL")
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    evidence["reap_timed_out"] = True
            return 124, evidence


def evaluate_case(
    document,
    mode,
    payload_mib,
    frequency_hz,
    duration_s,
    min_achieved_rate_ratio=0.95,
):
    candidates = [
        result
        for result in document.get("results", [])
        if result.get("scenario") == "zero_copy_vs_protobuf"
        and result.get("coverage") == "inter_process"
        and result.get("message_type") == mode
        and result.get("topology") == "1_pub_1_sub"
        and result.get("publishers") == 1
        and result.get("subscribers") == 1
        and result.get("frequency_hz") == frequency_hz
        and result.get("payload_bytes") == payload_mib * MIB
        and result.get("duration_s") == duration_s
        and result.get("execution", {}).get("commands")
    ]
    if len(candidates) != 1:
        return {
            "accepted": False,
            "reasons": [
                "expected exactly one executable {} result, found {}".format(
                    mode, len(candidates)
                )
            ],
            "result": candidates[0] if candidates else None,
        }

    result = candidates[0]
    reasons = []
    execution = result.get("execution", {})
    throughput = result.get("throughput", {})
    reliability = result.get("reliability", {})
    latency = result.get("latency", {})
    notes = parse_notes(result.get("notes", ""))

    _require(result.get("success") is True, "suite result success=false", reasons)
    for field in (
        "endpoints_ready",
        "warmup_confirmed",
        "measured_delivery_confirmed",
        "shutdown_confirmed",
    ):
        _require(execution.get(field) is True, "execution.{} is false".format(field), reasons)

    _require(throughput.get("measured_sent_messages", 0) > 0, "no measured sends", reasons)
    _require(
        throughput.get("measured_received_messages", 0) > 0,
        "no measured deliveries",
        reasons,
    )
    _require(latency.get("measured_sample_count", 0) > 0, "no measured latency samples", reasons)
    for field in ("send_failures", "measured_send_failures", "final_send_failures"):
        _require(throughput.get(field) == 0, "throughput.{} != 0".format(field), reasons)
    for field in (
        "total_loss",
        "final_drained_total_loss",
        "duplicates",
        "reordered",
        "duplicate_or_reordered",
    ):
        _require(reliability.get(field) == 0, "reliability.{} != 0".format(field), reasons)
    _require(reliability.get("loss_rate") == 0, "reliability.loss_rate != 0", reasons)

    achieved_send_hz, send_rate_source = _measured_rate(
        throughput,
        "measured_send_rate_hz",
        "measured_sent_messages",
        "measured_send_duration_ns",
    )
    achieved_receive_hz, receive_rate_source = _measured_rate(
        throughput,
        "measured_receive_rate_hz",
        "measured_received_messages",
        "measured_receive_duration_ns",
    )
    required_rate_hz = frequency_hz * min_achieved_rate_ratio
    rate_failures = []
    if achieved_send_hz is None:
        rate_failures.append("send_rate_unavailable")
        reasons.append("measured send rate unavailable")
    elif achieved_send_hz < required_rate_hz:
        rate_failures.append("send_rate_below_target_ratio")
        reasons.append(
            "achieved send rate {:.3f} Hz is below required {:.3f} Hz "
            "(target {} Hz * ratio {:.3f})".format(
                achieved_send_hz,
                required_rate_hz,
                frequency_hz,
                min_achieved_rate_ratio,
            )
        )
    if achieved_receive_hz is None:
        rate_failures.append("receive_rate_unavailable")
        reasons.append("measured receive rate unavailable")
    elif achieved_receive_hz < required_rate_hz:
        rate_failures.append("receive_rate_below_target_ratio")
        reasons.append(
            "achieved receive rate {:.3f} Hz is below required {:.3f} Hz "
            "(target {} Hz * ratio {:.3f})".format(
                achieved_receive_hz,
                required_rate_hz,
                frequency_hz,
                min_achieved_rate_ratio,
            )
        )

    expected_transport = "shm" if mode == "protobuf" else "iceoryx"
    for field in ("transport_mode", "pub_transport_mode_seen", "sub_transport_mode_seen"):
        _require(
            notes.get(field) == expected_transport,
            "{}={} instead of {}".format(field, notes.get(field), expected_transport),
            reasons,
        )

    if mode == "pod":
        _require(result.get("shm_loan_supported") is True, "shm_loan_supported=false", reasons)
        _require(notes.get("loan_supported") == "true", "loan_supported note is not true", reasons)
        measured_sends = throughput.get("measured_sent_messages", 0)
        loan_publish_successes = throughput.get(
            "measured_loan_publish_successes",
            throughput.get("loan_publish_successes", -1),
        )
        fallback_attempts = throughput.get(
            "measured_fallback_transmit_attempts",
            throughput.get("fallback_transmit_attempts", -1),
        )
        fallback_successes = throughput.get(
            "measured_fallback_transmit_successes",
            throughput.get("fallback_transmit_successes", -1),
        )
        _require(
            loan_publish_successes == measured_sends,
            "loan_publish_successes != measured sends",
            reasons,
        )
        _require(
            fallback_attempts == 0,
            "fallback_transmit_attempts != 0",
            reasons,
        )
        _require(
            fallback_successes == 0,
            "fallback_transmit_successes != 0",
            reasons,
        )
        try:
            borrowed = int(notes.get("zero_copy_borrowed_messages", "0"))
            copies = int(notes.get("zero_copy_copy_count", "-1"))
        except ValueError:
            borrowed = 0
            copies = -1
        _require(borrowed > 0, "zero_copy_borrowed_messages <= 0", reasons)
        _require(copies == 0, "zero_copy_copy_count != 0", reasons)

    if rate_failures == [
        "send_rate_below_target_ratio",
        "receive_rate_below_target_ratio",
    ]:
        saturation_reason = "send_and_receive_rates_below_target_ratio"
    else:
        saturation_reason = "_and_".join(rate_failures) or None
    non_rate_reasons = [
        reason
        for reason in reasons
        if not reason.startswith("achieved ")
        and not reason.startswith("measured send rate ")
        and not reason.startswith("measured receive rate ")
    ]
    observed_zero_loss_throughput_hz = None
    if (
        not non_rate_reasons
        and achieved_send_hz is not None
        and achieved_receive_hz is not None
    ):
        observed_zero_loss_throughput_hz = min(
            achieved_send_hz, achieved_receive_hz
        )
    rate_evidence = {
        "target_frequency_hz": frequency_hz,
        "min_achieved_rate_ratio": min_achieved_rate_ratio,
        "required_rate_hz": required_rate_hz,
        "achieved_send_hz": achieved_send_hz,
        "achieved_receive_hz": achieved_receive_hz,
        "achieved_send_ratio": (
            achieved_send_hz / frequency_hz
            if achieved_send_hz is not None
            else None
        ),
        "achieved_receive_ratio": (
            achieved_receive_hz / frequency_hz
            if achieved_receive_hz is not None
            else None
        ),
        "send_rate_source": send_rate_source,
        "receive_rate_source": receive_rate_source,
        "saturation_reason": saturation_reason,
        "observed_zero_loss_throughput_hz": observed_zero_loss_throughput_hz,
    }
    return {
        "accepted": not reasons,
        "reasons": reasons,
        "result": result,
        "rate_evidence": rate_evidence,
    }


def _highest_pass_below(history, frequency_hz):
    passing = [
        outcome.frequency_hz
        for outcome in history
        if outcome.accepted and outcome.frequency_hz < frequency_hz
    ]
    return max(passing) if passing else None


def _resolution_midpoint(low_pass, high_fail, resolution_hz):
    midpoint = ((low_pass + high_fail) // 2 // resolution_hz) * resolution_hz
    if midpoint <= low_pass:
        midpoint = ((low_pass // resolution_hz) + 1) * resolution_hz
    if midpoint >= high_fail:
        midpoint = high_fail - resolution_hz
    if midpoint <= low_pass:
        midpoint = low_pass + 1
    return midpoint


def _binary_refine(probe, low_pass, high_fail, resolution_hz):
    while high_fail - low_pass > resolution_hz:
        midpoint = _resolution_midpoint(low_pass, high_fail, resolution_hz)
        outcome = probe(midpoint, "binary_refine", False)
        if outcome.accepted:
            low_pass = midpoint
        else:
            high_fail = midpoint
    return low_pass, high_fail


def _confirmation(probe, frequency_hz, purpose, confirm_runs):
    outcomes = [
        probe(frequency_hz, purpose, True) for _ in range(confirm_runs)
    ]
    passes = sum(outcome.accepted for outcome in outcomes)
    return outcomes, {
        "passes": passes,
        "failures": confirm_runs - passes,
        "runs": confirm_runs,
        "pass_fraction": passes / confirm_runs,
        "run_ids": [outcome.run_id for outcome in outcomes],
    }


def _next_geometric_frequency(frequency_hz, ceiling_hz, growth_factor):
    return min(
        ceiling_hz,
        max(frequency_hz + 1, int(math.ceil(frequency_hz * growth_factor))),
    )


def _tested_points(history):
    grouped = {}
    for outcome in history:
        grouped.setdefault(outcome.frequency_hz, []).append(outcome)
    points = []
    for frequency_hz in sorted(grouped):
        outcomes = grouped[frequency_hz]
        passes = sum(outcome.accepted for outcome in outcomes)
        failures = [outcome for outcome in outcomes if not outcome.accepted]
        points.append(
            {
                "frequency_hz": frequency_hz,
                "passes": passes,
                "runs": len(outcomes),
                "pass_fraction": passes / len(outcomes),
                "run_ids": [outcome.run_id for outcome in outcomes],
                "purposes": sorted({outcome.purpose for outcome in outcomes}),
                "failure_reasons": [
                    {
                        "run_id": outcome.run_id,
                        "reasons": outcome.reasons,
                        "rate_evidence": outcome.rate_evidence,
                        "timeout_evidence": outcome.timeout_evidence,
                    }
                    for outcome in failures
                ],
                "rate_evidence": [
                    {
                        "run_id": outcome.run_id,
                        "purpose": outcome.purpose,
                        "accepted": outcome.accepted,
                        **(outcome.rate_evidence or {}),
                    }
                    for outcome in outcomes
                ],
            }
        )
    return points


def _with_evidence(result, history):
    result["tested_points"] = _tested_points(history)
    return result


def _outcome_metrics(outcome):
    result = outcome.result or {}
    throughput = result.get("throughput", {})
    latency = result.get("latency", {})
    resource = result.get("resource", {})
    metrics = {
        "run_id": outcome.run_id,
        "measured_messages_per_s": throughput.get("messages_per_s"),
        "measured_mib_per_s": throughput.get("mb_per_s"),
        "p99_latency_ns": latency.get("p99_ns"),
        "p99_9_latency_ns": latency.get("p99_9_ns"),
        "cpu_utilization_percent": resource.get("cpu_utilization_percent"),
        "rss_kb_end": resource.get("rss_kb_end"),
    }
    metrics.update(outcome.rate_evidence or {})
    return metrics


def search_boundary(
    mode,
    start_hz,
    ceiling_hz,
    growth_factor,
    confirm_runs,
    probe,
    history,
    resolution_hz=100,
    max_confirmation_rounds=3,
):
    low_pass = None
    high_fail = None
    try:
        for frequency_hz in geometric_frequencies(
            start_hz, ceiling_hz, growth_factor
        ):
            outcome = probe(frequency_hz, "geometric_search", False)
            if outcome.accepted:
                low_pass = frequency_hz
            else:
                high_fail = frequency_hz
                break

        if low_pass is None:
            return _with_evidence(
                {
                    "status": "no_passing_frequency",
                    "mode": mode,
                    "start_hz": start_hz,
                    "failure_hz": high_fail,
                    "resolution_hz": resolution_hz,
                    "claimed_maximum": False,
                },
                history,
            )

        unstable_candidates = []
        for confirmation_round in range(1, max_confirmation_rounds + 1):
            if high_fail is None:
                confirmations, confirmation_evidence = _confirmation(
                    probe,
                    low_pass,
                    "lower_bound_confirmation",
                    confirm_runs,
                )
                if confirmation_evidence["passes"] == confirm_runs:
                    return _with_evidence(
                        {
                            "status": "confirmed_lower_bound",
                            "mode": mode,
                            "confirmed_passing_lower_bound_hz": low_pass,
                            "lower_bound_hz": low_pass,
                            "resolution_hz": resolution_hz,
                            "lower_confirmation": confirmation_evidence,
                            "representative_metrics": _outcome_metrics(
                                confirmations[-1]
                            ),
                            "unstable_candidates": unstable_candidates,
                            "ceiling_reached": low_pass == ceiling_hz,
                            "claimed_maximum": False,
                        },
                        history,
                    )
                return _with_evidence(
                    {
                        "status": "inconclusive",
                        "reason": "lower_bound_confirmation_mixed"
                        if confirmation_evidence["passes"]
                        else "lower_bound_confirmation_failed",
                        "mode": mode,
                        "candidate_hz": low_pass,
                        "resolution_hz": resolution_hz,
                        "lower_confirmation": confirmation_evidence,
                        "unstable_candidates": unstable_candidates,
                        "claimed_maximum": False,
                    },
                    history,
                )

            low_pass, high_fail = _binary_refine(
                probe, low_pass, high_fail, resolution_hz
            )
            candidate = low_pass
            confirmations, confirmation_evidence = _confirmation(
                probe,
                candidate,
                "lower_bound_confirmation",
                confirm_runs,
            )
            if confirmation_evidence["passes"] != confirm_runs:
                unstable_candidates.append(
                    {
                        "frequency_hz": candidate,
                        "passes": confirmation_evidence["passes"],
                        "runs": confirm_runs,
                        "pass_fraction": confirmation_evidence[
                            "pass_fraction"
                        ],
                        "run_ids": [outcome.run_id for outcome in confirmations],
                    }
                )
                high_fail = candidate
                if confirmation_round == max_confirmation_rounds:
                    return _with_evidence(
                        {
                            "status": "inconclusive",
                            "reason": "lower_bound_confirmation_unstable",
                            "mode": mode,
                            "confirmation_rounds": confirmation_round,
                            "resolution_hz": resolution_hz,
                            "candidate_hz": candidate,
                            "lower_confirmation": confirmation_evidence,
                            "unstable_candidates": unstable_candidates,
                            "claimed_maximum": False,
                        },
                        history,
                    )
                lower = _highest_pass_below(history, candidate)
                if lower is None:
                    return _with_evidence(
                        {
                            "status": "no_durable_passing_frequency",
                            "mode": mode,
                            "candidate_hz": candidate,
                            "confirmation_round": confirmation_round,
                            "resolution_hz": resolution_hz,
                            "unstable_candidates": unstable_candidates,
                            "claimed_maximum": False,
                        },
                        history,
                    )
                low_pass, high_fail = _binary_refine(
                    probe, lower, high_fail, resolution_hz
                )
                continue

            upper_outcomes, upper_confirmation = _confirmation(
                probe,
                high_fail,
                "upper_bound_confirmation",
                confirm_runs,
            )
            if upper_confirmation["failures"] == confirm_runs:
                return _with_evidence(
                    {
                        "status": "resolution_bounded_boundary",
                        "mode": mode,
                        "description": (
                            "confirmed passing lower bound with confirmed "
                            "failing upper bound"
                        ),
                        "confirmed_passing_lower_bound_hz": candidate,
                        "confirmed_failing_upper_bound_hz": high_fail,
                        "boundary_lower_hz": candidate,
                        "boundary_upper_hz": high_fail,
                        "boundary_width_hz": high_fail - candidate,
                        "resolution_hz": resolution_hz,
                        "lower_confirmation": confirmation_evidence,
                        "upper_confirmation": upper_confirmation,
                        "representative_metrics": _outcome_metrics(
                            confirmations[-1]
                        ),
                        "boundary_failure_run_ids": [
                            outcome.run_id for outcome in upper_outcomes
                        ],
                        "boundary_failure_reasons": [
                            {
                                "run_id": outcome.run_id,
                                "reasons": outcome.reasons,
                                "rate_evidence": outcome.rate_evidence,
                                "timeout_evidence": outcome.timeout_evidence,
                            }
                            for outcome in upper_outcomes
                        ],
                        "unstable_candidates": unstable_candidates,
                        "ceiling_reached": False,
                        "claimed_maximum": False,
                    },
                    history,
                )

            if upper_confirmation["passes"] not in (0, confirm_runs):
                return _with_evidence(
                    {
                        "status": "inconclusive",
                        "reason": "upper_bound_confirmation_mixed",
                        "mode": mode,
                        "confirmed_passing_lower_bound_hz": candidate,
                        "tested_upper_hz": high_fail,
                        "resolution_hz": resolution_hz,
                        "lower_confirmation": confirmation_evidence,
                        "upper_confirmation": upper_confirmation,
                        "unstable_candidates": unstable_candidates,
                        "claimed_maximum": False,
                    },
                    history,
                )

            unstable_candidates.append(
                {
                    "frequency_hz": high_fail,
                    "passes": upper_confirmation["passes"],
                    "runs": confirm_runs,
                    "pass_fraction": upper_confirmation["pass_fraction"],
                    "run_ids": upper_confirmation["run_ids"],
                    "reason": "transient_failure_confirmed_passing",
                }
            )
            low_pass = high_fail
            if low_pass >= ceiling_hz:
                high_fail = None
                continue
            next_hz = _next_geometric_frequency(
                low_pass, ceiling_hz, growth_factor
            )
            high_fail = None
            for frequency_hz in geometric_frequencies(
                next_hz, ceiling_hz, growth_factor
            ):
                outcome = probe(
                    frequency_hz, "geometric_search_extension", False
                )
                if outcome.accepted:
                    low_pass = frequency_hz
                else:
                    high_fail = frequency_hz
                    break

        return _with_evidence(
            {
                "status": "inconclusive",
                "reason": "confirmation_round_limit_reached",
                "mode": mode,
                "confirmation_rounds": max_confirmation_rounds,
                "resolution_hz": resolution_hz,
                "low_pass_hz": low_pass,
                "tested_failure_hz": high_fail,
                "unstable_candidates": unstable_candidates,
                "claimed_maximum": False,
            },
            history,
        )
    except ProbeBudgetExceeded as error:
        return _with_evidence(
            {
                "status": "inconclusive",
                "reason": "{}_run_budget_exceeded".format(
                    error.budget_scope
                ),
                "mode": mode,
                "resolution_hz": resolution_hz,
                "budget": {
                    "scope": error.budget_scope,
                    "max_runs": error.max_runs,
                    "completed_runs": error.completed_runs,
                    "attempted_frequency_hz": error.frequency_hz,
                    "attempted_purpose": error.purpose,
                },
                "low_pass_hz": low_pass,
                "tested_failure_hz": high_fail,
                "claimed_maximum": False,
            },
            history,
        )


class ExecutionCoordinator:
    def __init__(self, max_total_runs):
        self.max_total_runs = max_total_runs
        self.completed_runs = 0
        self.run_records = []

    def reserve(self, frequency_hz, purpose):
        if self.max_total_runs and self.completed_runs >= self.max_total_runs:
            raise ProbeBudgetExceeded(
                "global",
                self.max_total_runs,
                self.completed_runs,
                frequency_hz,
                purpose,
            )
        self.completed_runs += 1
        return self.completed_runs


class ModeExecutor:
    def __init__(
        self,
        args,
        repo_root,
        output_dir,
        benchmark_binary,
        environment,
        payload_mib,
        mode,
        coordinator,
        command_runner=None,
    ):
        self.args = args
        self.repo_root = repo_root
        self.output_dir = output_dir
        self.benchmark_binary = benchmark_binary
        self.environment = environment
        self.payload_mib = payload_mib
        self.mode = mode
        self.coordinator = coordinator
        self.command_runner = command_runner
        self.history = []

    def _command(self, frequency_hz, output_json):
        return [
            str(self.benchmark_binary),
            "--output_json={}".format(output_json),
            "--cpu_set={}".format(self.args.cpu_set),
            "--process_case_timeout_s={}".format(self.args.process_timeout_s),
            "--readiness_timeout_s={}".format(self.args.readiness_timeout_s),
            "--startup_wait_ms={}".format(self.args.startup_wait_ms),
            "--cooldown_wait_ms={}".format(self.args.cooldown_wait_ms),
            "--max_loss_rate=0",
            "--min_achieved_rate_ratio={}".format(
                self.args.min_achieved_rate_ratio
            ),
            "--use_real_inter_process=true",
            "--run_shm_zero_copy_probe=false",
            "--run_comparison_bandwidth_search=false",
            "--run_frequency_sweep=false",
            "--run_bandwidth_sweep=false",
            "--run_subscriber_scaling=false",
            "--run_publisher_scaling=false",
            "--run_cpu_interference=false",
            "--run_flood_mode_comparison=false",
            "--run_payload_sweep_comparison=false",
            "--run_fanout_scaling_comparison=false",
            "--enable_long_run=false",
            "--comparison_message_type={}".format(self.mode),
            "--scaling_frequency_hz={}".format(frequency_hz),
            "--scaling_payload_bytes={}".format(self.payload_mib * MIB),
            "--scaling_case_duration_s={}".format(self.args.duration_s),
        ]

    def _execute(self, run_id, frequency_hz, purpose):
        case_dir = (
            self.output_dir
            / "cases"
            / "payload-{}mib".format(self.payload_mib)
            / self.mode
        )
        case_dir.mkdir(parents=True, exist_ok=True)
        stem = "run-{:04d}-{}hz-{}".format(
            run_id, frequency_hz, purpose.replace("_", "-")
        )
        output_json = case_dir / "{}.json".format(stem)
        output_log = case_dir / "{}.log".format(stem)
        command = self._command(frequency_hz, output_json)
        environment_prefix = " ".join(
            "{}={}".format(key, shlex.quote(value))
            for key, value in sorted(self.environment.items())
        )
        exact_command = "{} {}".format(environment_prefix, shlex.join(command))
        record = {
            "run_id": run_id,
            "payload_mib": self.payload_mib,
            "mode": self.mode,
            "frequency_hz": frequency_hz,
            "purpose": purpose,
            "command": exact_command,
            "output_json": str(output_json),
            "output_log": str(output_log),
            "outer_wall_timeout_s": derive_outer_wall_timeout_s(self.args),
        }

        if self.args.dry_run:
            record["dry_run"] = True
            outcome = Outcome(
                self.mode,
                self.payload_mib,
                frequency_hz,
                True,
                [],
                purpose,
                run_id,
                rate_evidence={
                    "target_frequency_hz": frequency_hz,
                    "min_achieved_rate_ratio": self.args.min_achieved_rate_ratio,
                    "required_rate_hz": (
                        frequency_hz * self.args.min_achieved_rate_ratio
                    ),
                    "achieved_send_hz": None,
                    "achieved_receive_hz": None,
                    "achieved_send_ratio": None,
                    "achieved_receive_ratio": None,
                    "send_rate_source": None,
                    "receive_rate_source": None,
                    "saturation_reason": None,
                    "observed_zero_loss_throughput_hz": None,
                },
            )
            record["mode_result"] = {
                "accepted": True,
                "reasons": ["dry_run"],
                "rate_evidence": outcome.rate_evidence,
            }
            self.coordinator.run_records.append(record)
            return outcome

        if self.command_runner is None:
            exit_code, timeout_evidence = _run_benchmark_subprocess(
                command,
                self.repo_root,
                self.environment,
                output_log,
                record["outer_wall_timeout_s"],
            )
            record["timeout_evidence"] = timeout_evidence
            try:
                with output_json.open(encoding="utf-8") as handle:
                    document = json.load(handle)
                parse_error = None
            except (OSError, json.JSONDecodeError) as error:
                document = None
                parse_error = str(error)
        else:
            exit_code, document, parse_error = self.command_runner(
                command, output_json, output_log
            )
        record["exit_code"] = exit_code
        if parse_error:
            record["parse_error"] = parse_error

        if document is None:
            evaluation = {
                "accepted": False,
                "reasons": ["benchmark JSON missing or invalid"],
                "result": None,
            }
        else:
            evaluation = evaluate_case(
                document,
                self.mode,
                self.payload_mib,
                frequency_hz,
                self.args.duration_s,
                self.args.min_achieved_rate_ratio,
            )
        if exit_code != 0:
            evaluation["accepted"] = False
            failure_reasons = []
            if record.get("timeout_evidence", {}).get("timed_out"):
                failure_reasons.append(
                    "benchmark outer wall timeout after {}s".format(
                        record["outer_wall_timeout_s"]
                    )
                )
            failure_reasons.append("benchmark exit code {}".format(exit_code))
            evaluation["reasons"] = (
                list(evaluation["reasons"]) + failure_reasons
            )
        outcome = Outcome(
            self.mode,
            self.payload_mib,
            frequency_hz,
            evaluation["accepted"],
            evaluation["reasons"],
            purpose,
            run_id,
            evaluation["result"],
            evaluation.get("rate_evidence"),
            record.get("timeout_evidence"),
        )
        record["mode_result"] = {
            "accepted": outcome.accepted,
            "reasons": outcome.reasons,
            "result": outcome.result,
            "rate_evidence": outcome.rate_evidence,
            "timeout_evidence": outcome.timeout_evidence,
        }
        self.coordinator.run_records.append(record)
        return outcome

    def probe(self, frequency_hz, purpose, force=False):
        del force
        if len(self.history) >= self.args.max_runs_per_search:
            raise ProbeBudgetExceeded(
                "per_search",
                self.args.max_runs_per_search,
                len(self.history),
                frequency_hz,
                purpose,
            )
        run_id = self.coordinator.reserve(frequency_hz, purpose)
        outcome = self._execute(run_id, frequency_hz, purpose)
        self.history.append(outcome)
        return outcome


def _command_output(command, cwd):
    try:
        return subprocess.check_output(
            command, cwd=cwd, text=True, stderr=subprocess.STDOUT
        ).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        return "unavailable: {}".format(error)


def environment_metadata(repo_root, args, fixed_environment):
    cpu_model = "unknown"
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    return {
        "generated_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "repo_root": str(repo_root),
        "git_sha": _command_output(["git", "rev-parse", "HEAD"], repo_root),
        "git_dirty": bool(_command_output(["git", "status", "--porcelain"], repo_root)),
        "bazel_version": _command_output(["bazel", "--version"], repo_root),
        "python_version": platform.python_version(),
        "platform": platform.platform(),
        "uname": " ".join(platform.uname()),
        "cpu_model": cpu_model,
        "logical_cpu_count": os.cpu_count(),
        "process_affinity": affinity,
        "cpu_set": args.cpu_set,
        "controlled_environment": fixed_environment,
    }


def write_markdown(summary, path):
    config = summary["config"]
    with path.open("w", encoding="utf-8") as handle:
        handle.write("# Zero-loss 1→1 capacity comparison\n\n")
        handle.write(
            "Selected inter-process SHM protobuf and Iceoryx POD modes are "
            "searched independently. "
            "All accepted points require suite success, readiness, warmup, measured "
            "delivery, clean shutdown, zero loss/send failures/duplicates/reorder; "
            "measured send and receive rates must each meet the configured fraction "
            "of the target. POD also requires loans, borrowed messages, and zero "
            "copies.\n\n"
        )
        handle.write("## Fixed controls\n\n")
        handle.write(
            "- Payloads: `{}` MiB\n"
            "- Duration: `{}` s; readiness: `{}` s; startup: `{}` ms; cooldown: `{}` ms\n"
            "- Inner process timeout: `{}` s; derived outer wall timeout: `{}` s\n"
            "- CPU set: `{}`; max loss rate: `0`; topology: `1 publisher → 1 subscriber`\n"
            "- Search: start `{}` Hz, growth `{}`, ceiling `{}` Hz, resolution `{}` Hz\n"
            "- Minimum achieved send/receive rate: `{:.3f}` × target\n"
            "- Modes: `{}`\n"
            "- Confirmation: `{}` runs, at most `{}` rounds; per-search run "
            "budget: `{}`; optional global run cap: `{}`\n\n".format(
                ",".join(str(value) for value in config["payloads_mib"]),
                config["duration_s"],
                config["readiness_timeout_s"],
                config["startup_wait_ms"],
                config["cooldown_wait_ms"],
                config["process_timeout_s"],
                config["outer_wall_timeout_s"],
                config["cpu_set"],
                config["start_hz"],
                config["growth_factor"],
                config["ceiling_hz"],
                config["resolution_hz"],
                config["min_achieved_rate_ratio"],
                ",".join(config["modes"]),
                config["confirm_runs"],
                config["max_confirmation_rounds"],
                config["max_runs_per_search"],
                config["max_total_runs"],
            )
        )
        handle.write("## Results\n\n")
        handle.write(
            "| Payload MiB | Mode | Result | Accepted target Hz | "
            "Achieved send Hz | Achieved receive Hz | Min ratio | "
            "Requested GiB/s | Measured MiB/s | p99 ms | CPU % | "
            "Saturation | Boundary evidence |\n"
        )
        handle.write(
            "| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | "
            "---: | ---: | ---: | --- | --- |\n"
        )
        for payload in summary["payload_results"]:
            for mode, result in payload["modes"].items():
                if result["status"] == "resolution_bounded_boundary":
                    frequency = result["confirmed_passing_lower_bound_hz"]
                    label = "resolution-bounded boundary ({} Hz)".format(
                        result["resolution_hz"]
                    )
                    lower = result["lower_confirmation"]
                    upper = result["upper_confirmation"]
                    evidence = (
                        "{} Hz {}/{} pass; {} Hz {}/{} fail"
                    ).format(
                        frequency,
                        lower["passes"],
                        lower["runs"],
                        result["confirmed_failing_upper_bound_hz"],
                        upper["failures"],
                        upper["runs"],
                    )
                elif result["status"] == "confirmed_lower_bound":
                    frequency = result["confirmed_passing_lower_bound_hz"]
                    label = "confirmed passing lower bound"
                    confirmation = result["lower_confirmation"]
                    evidence = "{}/{} pass; no maximum claimed".format(
                        confirmation["passes"], confirmation["runs"]
                    )
                else:
                    frequency = 0
                    label = result["status"].replace("_", " ")
                    if result.get("reason", "").endswith(
                        "_run_budget_exceeded"
                    ):
                        budget = result["budget"]
                        evidence = (
                            "budget {}/{} exhausted before {} Hz ({})"
                        ).format(
                            budget["completed_runs"],
                            budget["max_runs"],
                            budget["attempted_frequency_hz"],
                            budget["attempted_purpose"],
                        )
                    elif result.get("reason"):
                        evidence = result["reason"].replace("_", " ")
                    else:
                        evidence = "no maximum claimed"
                gib_s = payload["payload_mib"] * frequency / 1024.0
                metrics = result.get("representative_metrics", {})
                measured_mib_s = metrics.get("measured_mib_per_s")
                achieved_send_hz = metrics.get("achieved_send_hz")
                achieved_receive_hz = metrics.get("achieved_receive_hz")
                p99_ns = metrics.get("p99_latency_ns")
                cpu = metrics.get("cpu_utilization_percent")
                handle.write(
                    "| {} | {} | {} | {} | {} | {} | {:.3f} | {:.3f} | "
                    "{} | {} | {} | {} | {} |\n".format(
                        payload["payload_mib"],
                        "Protobuf/SHM" if mode == "protobuf" else "POD/Iceoryx",
                        label,
                        frequency or "n/a",
                        (
                            "{:.3f}".format(achieved_send_hz)
                            if achieved_send_hz is not None
                            else "n/a"
                        ),
                        (
                            "{:.3f}".format(achieved_receive_hz)
                            if achieved_receive_hz is not None
                            else "n/a"
                        ),
                        config["min_achieved_rate_ratio"],
                        gib_s,
                        (
                            "{:.3f}".format(measured_mib_s)
                            if measured_mib_s is not None
                            else "n/a"
                        ),
                        "{:.3f}".format(p99_ns / 1e6) if p99_ns is not None else "n/a",
                        "{:.2f}".format(cpu) if cpu is not None else "n/a",
                        metrics.get("saturation_reason") or "none",
                        evidence,
                    )
                )

        handle.write("\n## Achieved-rate evidence\n\n")
        handle.write(
            "| Payload MiB | Mode | Run | Purpose | Target Hz | Send Hz | "
            "Receive Hz | Send ratio | Receive ratio | Required ratio | "
            "Accepted | Saturation reason | Observed zero-loss Hz |\n"
        )
        handle.write(
            "| ---: | --- | ---: | --- | ---: | ---: | ---: | ---: | "
            "---: | ---: | --- | --- | ---: |\n"
        )
        for payload in summary["payload_results"]:
            for mode, result in payload["modes"].items():
                for point in result.get("tested_points", []):
                    for evidence in point.get("rate_evidence", []):
                        send_hz = evidence.get("achieved_send_hz")
                        receive_hz = evidence.get("achieved_receive_hz")
                        send_ratio = evidence.get("achieved_send_ratio")
                        receive_ratio = evidence.get("achieved_receive_ratio")
                        observed_hz = evidence.get(
                            "observed_zero_loss_throughput_hz"
                        )
                        handle.write(
                            "| {} | {} | {} | {} | {} | {} | {} | {} | "
                            "{} | {:.3f} | {} | {} | {} |\n".format(
                                payload["payload_mib"],
                                (
                                    "Protobuf/SHM"
                                    if mode == "protobuf"
                                    else "POD/Iceoryx"
                                ),
                                evidence["run_id"],
                                evidence["purpose"],
                                evidence["target_frequency_hz"],
                                (
                                    "{:.3f}".format(send_hz)
                                    if send_hz is not None
                                    else "n/a"
                                ),
                                (
                                    "{:.3f}".format(receive_hz)
                                    if receive_hz is not None
                                    else "n/a"
                                ),
                                (
                                    "{:.3f}".format(send_ratio)
                                    if send_ratio is not None
                                    else "n/a"
                                ),
                                (
                                    "{:.3f}".format(receive_ratio)
                                    if receive_ratio is not None
                                    else "n/a"
                                ),
                                evidence["min_achieved_rate_ratio"],
                                "yes" if evidence["accepted"] else "no",
                                evidence.get("saturation_reason") or "none",
                                (
                                    "{:.3f}".format(observed_hz)
                                    if observed_hz is not None
                                    else "n/a"
                                ),
                            )
                        )

        handle.write("\n## Pass fractions at all tested points\n\n")
        handle.write("| Payload MiB | Mode | Frequency Hz | Passes | Pass fraction |\n")
        handle.write("| ---: | --- | ---: | ---: | ---: |\n")
        for payload in summary["payload_results"]:
            for mode, result in payload["modes"].items():
                for point in result.get("tested_points", []):
                    handle.write(
                        "| {} | {} | {} | {}/{} | {:.3f} |\n".format(
                            payload["payload_mib"],
                            "Protobuf/SHM"
                            if mode == "protobuf"
                            else "POD/Iceoryx",
                            point["frequency_hz"],
                            point["passes"],
                            point["runs"],
                            point["pass_fraction"],
                        )
                    )

        if len(config["modes"]) == 2:
            handle.write("\n## POD versus protobuf capacity\n\n")
            handle.write(
                "| Payload MiB | POD/Protobuf lower-bound Hz ratio | "
                "Comparison quality |\n"
            )
            handle.write("| ---: | ---: | --- |\n")
            for payload in summary["payload_results"]:
                comparison = payload["comparison"]
                ratio = comparison.get("pod_over_protobuf_hz_ratio")
                handle.write(
                    "| {} | {} | {} |\n".format(
                        payload["payload_mib"],
                        "{:.3f}x".format(ratio) if ratio is not None else "n/a",
                        comparison["quality"],
                    )
                )

        handle.write("\n## Environment\n\n")
        environment = summary["environment"]
        for key in (
            "generated_at_utc",
            "git_sha",
            "git_dirty",
            "bazel_version",
            "platform",
            "cpu_model",
            "process_affinity",
        ):
            handle.write("- **{}:** `{}`\n".format(key, environment[key]))

        handle.write("\n## Exact commands\n\n")
        if summary.get("build_command"):
            handle.write("```text\n{}\n```\n\n".format(summary["build_command"]))
        for run in summary["runs"]:
            handle.write(
                "Run {} ({} MiB, {} Hz, {}):\n\n```text\n{}\n```\n\n".format(
                    run["run_id"],
                    run["payload_mib"],
                    run["frequency_hz"],
                    run["purpose"],
                    run["command"],
                )
            )


def build_parser():
    parser = argparse.ArgumentParser(
        description="Find durable zero-loss SHM protobuf and Iceoryx POD capacity."
    )
    parser.add_argument("--outdir")
    parser.add_argument("--payloads-mib", default="1,4,7")
    parser.add_argument("--start-hz", type=int, default=10)
    parser.add_argument("--ceiling-hz", type=int, default=20000)
    parser.add_argument("--growth-factor", type=float, default=2.0)
    parser.add_argument("--duration-s", type=int, default=5)
    parser.add_argument("--min-achieved-rate-ratio", type=float, default=0.95)
    parser.add_argument("--confirm-runs", type=int, default=3)
    parser.add_argument("--resolution-hz", type=int, default=100)
    parser.add_argument("--max-confirmation-rounds", type=int, default=3)
    parser.add_argument(
        "--comparison-message-type",
        choices=("both", "protobuf", "pod"),
        default="both",
    )
    parser.add_argument("--max-runs-per-search", type=int, default=60)
    parser.add_argument(
        "--max-total-runs",
        "--max-total-probes",
        dest="max_total_runs",
        type=int,
        default=0,
        help="optional global cap across all searches; 0 disables it",
    )
    parser.add_argument("--cpu-set", default="auto")
    parser.add_argument("--readiness-timeout-s", type=int, default=30)
    parser.add_argument("--startup-wait-ms", type=int, default=2000)
    parser.add_argument("--cooldown-wait-ms", type=int, default=500)
    parser.add_argument("--process-timeout-s", type=int, default=180)
    parser.add_argument("--domain-id", default=os.environ.get("CYBER_DOMAIN_ID", "0"))
    parser.add_argument("--benchmark-bin")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser


def _positive_csv(value):
    payloads = sorted({int(item.strip()) for item in value.split(",") if item.strip()})
    if not payloads or any(payload <= 0 for payload in payloads):
        raise ValueError("payloads must be positive MiB integers")
    return payloads


def _auto_cpu_set():
    if hasattr(os, "sched_getaffinity"):
        available = sorted(os.sched_getaffinity(0))
    else:
        available = list(range(os.cpu_count() or 1))
    return ",".join(str(cpu) for cpu in available[:2]) or "0"


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        payloads = _positive_csv(args.payloads_mib)
    except ValueError as error:
        parser.error(str(error))
    if args.quick:
        if args.payloads_mib == parser.get_default("payloads_mib"):
            payloads = [1]
        if args.start_hz == parser.get_default("start_hz"):
            args.start_hz = 5
        if args.ceiling_hz == parser.get_default("ceiling_hz"):
            args.ceiling_hz = 20
        if args.duration_s == parser.get_default("duration_s"):
            args.duration_s = 1
    if args.start_hz <= 0 or args.ceiling_hz < args.start_hz:
        parser.error("require 0 < --start-hz <= --ceiling-hz")
    if args.growth_factor <= 1.0:
        parser.error("--growth-factor must be > 1")
    if args.duration_s <= 0:
        parser.error("--duration-s must be positive")
    if not 0 < args.min_achieved_rate_ratio <= 1:
        parser.error("--min-achieved-rate-ratio must be in (0, 1]")
    if args.confirm_runs < 3:
        parser.error("--confirm-runs must be at least 3")
    if args.resolution_hz <= 0:
        parser.error("--resolution-hz must be positive")
    if args.max_confirmation_rounds <= 0:
        parser.error("--max-confirmation-rounds must be positive")
    if args.max_runs_per_search <= 0:
        parser.error("--max-runs-per-search must be positive")
    if args.max_total_runs < 0:
        parser.error("--max-total-runs must be non-negative")
    if args.readiness_timeout_s <= 0:
        parser.error("--readiness-timeout-s must be positive")
    if args.startup_wait_ms < 0 or args.cooldown_wait_ms < 0:
        parser.error("startup and cooldown waits must be non-negative")
    if args.process_timeout_s < 10:
        parser.error("--process-timeout-s must be at least 10")
    if args.cpu_set == "auto":
        args.cpu_set = _auto_cpu_set()

    repo_root = Path(
        _command_output(["git", "rev-parse", "--show-toplevel"], Path.cwd())
    )
    if not repo_root.is_dir():
        parser.error("unable to determine repository root")
    output_dir = Path(
        args.outdir
        or "artifacts/performance/zero-loss-capacity-{}".format(
            datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        )
    )
    if not output_dir.is_absolute():
        output_dir = repo_root / output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    benchmark_binary = Path(
        args.benchmark_bin
        or repo_root / "bazel-bin/tests/perf_test/cyber_rt_benchmark_suite"
    )
    build_command_list = [
        "bazel",
        "build",
        "--config=ci",
        "//tests/perf_test:cyber_rt_benchmark_suite",
        "//tests/perf_test:benchmark_pub",
        "//tests/perf_test:benchmark_sub",
    ]
    build_command = shlex.join(build_command_list)
    if not args.dry_run and not args.skip_build:
        subprocess.run(build_command_list, cwd=repo_root, check=True)
    if not args.dry_run and not benchmark_binary.is_file():
        parser.error("benchmark binary does not exist: {}".format(benchmark_binary))

    glog_dir = output_dir / "glog"
    glog_dir.mkdir(parents=True, exist_ok=True)
    fixed_environment = {
        "CYBER_PATH": str(repo_root / "cyber"),
        "CYBER_DOMAIN_ID": str(args.domain_id),
        "GLOG_log_dir": str(glog_dir),
    }
    selected_modes = (
        MODES
        if args.comparison_message_type == "both"
        else (args.comparison_message_type,)
    )
    coordinator = ExecutionCoordinator(args.max_total_runs)
    payload_results = []
    for payload_mib in payloads:
        modes = {}
        for mode in selected_modes:
            executor = ModeExecutor(
                args,
                repo_root,
                output_dir,
                benchmark_binary,
                fixed_environment,
                payload_mib,
                mode,
                coordinator,
            )
            modes[mode] = search_boundary(
                mode,
                args.start_hz,
                args.ceiling_hz,
                args.growth_factor,
                args.confirm_runs,
                executor.probe,
                executor.history,
                args.resolution_hz,
                args.max_confirmation_rounds,
            )
        frequencies = {}
        for mode, result in modes.items():
            frequencies[mode] = result.get(
                "confirmed_passing_lower_bound_hz",
                result.get("lower_bound_hz"),
            )
        ratio = None
        if frequencies.get("protobuf") and frequencies.get("pod"):
            ratio = frequencies["pod"] / frequencies["protobuf"]
        quality = (
            "confirmed passing lower bounds with confirmed failing upper bounds"
            if all(
                result["status"] == "resolution_bounded_boundary"
                for result in modes.values()
            )
            else "contains a confirmed lower bound or unresolved result"
        )
        payload_results.append(
            {
                "payload_mib": payload_mib,
                "modes": modes,
                "comparison": {
                    "pod_over_protobuf_hz_ratio": ratio,
                    "quality": quality,
                },
            }
        )

    acceptable = {"resolution_bounded_boundary", "confirmed_lower_bound"}
    all_resolved = all(
        mode["status"] in acceptable
        for payload in payload_results
        for mode in payload["modes"].values()
    )
    summary = {
        "schema_version": 1,
        "status": (
            "dry_run"
            if args.dry_run
            else ("completed" if all_resolved else "inconclusive")
        ),
        "config": {
            "payloads_mib": payloads,
            "modes": list(selected_modes),
            "start_hz": args.start_hz,
            "ceiling_hz": args.ceiling_hz,
            "growth_factor": args.growth_factor,
            "duration_s": args.duration_s,
            "min_achieved_rate_ratio": args.min_achieved_rate_ratio,
            "confirm_runs": args.confirm_runs,
            "resolution_hz": args.resolution_hz,
            "max_confirmation_rounds": args.max_confirmation_rounds,
            "max_runs_per_search": args.max_runs_per_search,
            "max_total_runs": args.max_total_runs,
            "cpu_set": args.cpu_set,
            "readiness_timeout_s": args.readiness_timeout_s,
            "startup_wait_ms": args.startup_wait_ms,
            "cooldown_wait_ms": args.cooldown_wait_ms,
            "process_timeout_s": args.process_timeout_s,
            "outer_wall_timeout_s": derive_outer_wall_timeout_s(args),
            "max_loss_rate": 0,
            "topology": "1_pub_1_sub",
        },
        "environment": environment_metadata(repo_root, args, fixed_environment),
        "build_command": None if args.skip_build else build_command,
        "benchmark_binary": str(benchmark_binary),
        "payload_results": payload_results,
        "runs": coordinator.run_records,
    }
    json_path = output_dir / "zero_loss_capacity.json"
    markdown_path = output_dir / "zero_loss_capacity.md"
    with json_path.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2)
        handle.write("\n")
    write_markdown(summary, markdown_path)
    print("Zero-loss capacity reports: {} and {}".format(json_path, markdown_path))

    if args.dry_run:
        return 0
    return 0 if all_resolved else 1


if __name__ == "__main__":
    raise SystemExit(main())
