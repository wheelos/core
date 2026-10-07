# Agent Guide

## Rules

- Read the relevant source, configuration, and tests before making changes.
- Follow the existing architecture, naming, and Bazel directory conventions.
- Update or add adjacent tests for behavior changes.
- Change only what is required for the task; do not revert unrelated changes.
- Read the matching `.agents/skills/*/SKILL.md` before build, test, release, or review work.
- Read `.agents/knowledge/` as needed; do not copy implementation details here.

## Commands

- Default build: `bash scripts/build.sh`
- Message tests: `bazel test //cyber/message/...`
- CI baseline: `bash scripts/release/ubuntu2204_baseline.sh`
- C++/BUILD checks: `bash scripts/lint/lint.sh --cpp`
- Python checks: `bash scripts/lint/lint.sh --py`
- Shell checks: `bash scripts/lint/lint.sh --sh`
- Changed-file checks: `bash scripts/lint/lint.sh --all --diff origin/main`

## Knowledge

- Index: `.agents/knowledge/README.md`
- Architecture: `.agents/knowledge/architecture.md`
- Conventions: `.agents/knowledge/conventions.md`
- Troubleshooting: `.agents/knowledge/troubleshooting.md`

## Skills

- Testing: `.agents/skills/testing/SKILL.md`
- Review: `.agents/skills/review/SKILL.md`
- Release: `.agents/skills/release/SKILL.md`
- Build/release validation: `.agents/skills/build-release-validation/SKILL.md`

## Agent layout

- `.github/` owns GitHub governance and provider integration.
- `.agents/skills/README.md` indexes executable task workflows.
- `.agents/knowledge/` owns durable repository knowledge.
- `.agents/notes/README.md` describes ignored temporary investigations.
