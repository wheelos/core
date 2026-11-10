#!/usr/bin/env python3
"""Reject unclassified repository tests; hardware and fixture gates stay explicit."""

import ast
from pathlib import Path


def inventory(root):
    tests = set()
    suites = {}
    manual = set()
    for package in ("cyber", "examples", "runtime", "scripts", "tests"):
        for path in (root / package).rglob("BUILD"):
            tree = ast.parse(path.read_text())
            prefix = "//" + str(path.parent.relative_to(root))
            for node in tree.body:
                if not isinstance(node, ast.Expr) or not isinstance(
                    node.value, ast.Call
                ):
                    continue
                call = node.value
                rule = getattr(call.func, "id", "")
                fields = {item.arg: item.value for item in call.keywords}
                if rule not in (
                    "cc_test",
                    "py_test",
                    "sh_test",
                    "test_suite",
                    "runtime_analysis_tests",
                ):
                    continue
                label = prefix + ":" + ast.literal_eval(fields["name"])
                if rule == "test_suite":
                    suites[label] = [
                        prefix + item if item.startswith(":") else item
                        for item in ast.literal_eval(fields["tests"])
                    ]
                else:
                    tests.add(label)
                    if "manual" in ast.literal_eval(
                        fields.get("tags", ast.List(elts=[]))
                    ):
                        manual.add(label)
    return tests, suites, manual


def check(root):
    tests, suites, manual = inventory(root)
    classified = set()

    def expand(label):
        if label in suites:
            for member in suites[label]:
                expand(member)
        else:
            classified.add(label)

    for layer in ("unit", "integration", "performance_regression", "hardware"):
        expand("//tests:" + layer)
    missing = tests - classified - manual
    stale = classified - tests
    if missing or stale:
        raise ValueError(
            f"Unclassified tests: {sorted(missing)}; nonexistent tests: {sorted(stale)}"
        )
    print(f"Classified {len(tests)} tests; fixture-only manual tests: {sorted(manual)}")


if __name__ == "__main__":
    check(Path(__file__).resolve().parents[2])
