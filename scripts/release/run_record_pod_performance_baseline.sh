#!/usr/bin/env bash
set -euo pipefail

PROTOBUF_RECORD=""
MIXED_RECORD=""
OUTDIR=""
ROUNDS=5
ALLOW_NETWORK_FS=false

while [ "$#" -gt 0 ]; do
  case "$1" in
    --protobuf-record) PROTOBUF_RECORD="$2"; shift 2 ;;
    --mixed-record) MIXED_RECORD="$2"; shift 2 ;;
    --outdir) OUTDIR="$2"; shift 2 ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    --allow-network-fs) ALLOW_NETWORK_FS=true; shift ;;
    -h|--help)
      echo "Usage: $0 --protobuf-record FILE --mixed-record FILE [--outdir DIR] [--rounds N] [--allow-network-fs]"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

if [ -z "$PROTOBUF_RECORD" ] || [ -z "$MIXED_RECORD" ] ||
   [ ! -s "$PROTOBUF_RECORD" ] || [ ! -s "$MIXED_RECORD" ]; then
  echo "Both non-empty --protobuf-record and --mixed-record are required." >&2
  exit 2
fi
if [ "$ROUNDS" -lt 2 ]; then
  echo "--rounds must be at least 2." >&2
  exit 2
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"
OUTDIR="${OUTDIR:-artifacts/performance/record-pod-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUTDIR"

FS_INFO="$(findmnt -T "$PROTOBUF_RECORD" -o TARGET,SOURCE,FSTYPE,OPTIONS -n || true)"
if [ "$ALLOW_NETWORK_FS" != true ] &&
   printf '%s\n' "$FS_INFO" | grep -Eiq '(^|[[:space:]])(nfs|nfs4|cifs|smb)([[:space:]]|$)'; then
  echo "Refusing network filesystem for publish performance baseline:" >&2
  echo "$FS_INFO" >&2
  echo "Copy records to local storage or pass --allow-network-fs for diagnostic data." >&2
  exit 3
fi

bazel build --config=ci //examples/record_play:record_play_tool_example
TOOL="$REPO_ROOT/bazel-bin/examples/record_play/record_play_tool_example"

for round in $(seq 1 "$ROUNDS"); do
  "$TOOL" --mode=protobuf_benchmark --source="$PROTOBUF_RECORD" \
    --max_per_channel=0 > "$OUTDIR/round-${round}.protobuf.log"
  "$TOOL" --mode=mixed_benchmark --source="$MIXED_RECORD" \
    --max_per_channel=0 > "$OUTDIR/round-${round}.mixed.log"
done

python3 - "$OUTDIR" "$PROTOBUF_RECORD" "$MIXED_RECORD" "$ROUNDS" <<'PY'
import json
import math
import os
import re
import statistics
import sys

outdir, protobuf_record, mixed_record, rounds = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])

def read_result(path, prefix):
    text = open(path, encoding="utf-8").read()
    line = next((line for line in text.splitlines() if line.startswith(prefix)), "")
    if not line:
        raise SystemExit(f"missing benchmark result in {path}")
    values = {}
    for key, value in re.findall(r"([A-Za-z_]+)=([^\t ]+)", line):
        values[key] = float(value) if key in {"MBps", "msgps", "read_s", "process_s"} else int(value)
    if values.get("failed", 1) != 0:
        raise SystemExit(f"benchmark failed: {path}")
    return values

results = []
for round_id in range(1, rounds + 1):
    protobuf = read_result(os.path.join(outdir, f"round-{round_id}.protobuf.log"), "protobuf_total")
    mixed = read_result(os.path.join(outdir, f"round-{round_id}.mixed.log"), "mixed_total")
    if protobuf["messages"] != mixed["messages"]:
        raise SystemExit(f"message count mismatch in round {round_id}")
    results.extend([
        {"round": round_id, "format": "protobuf", **protobuf},
        {"round": round_id, "format": "mixed_pod", **mixed},
    ])

def median(format_name, key):
    return statistics.median(item[key] for item in results if item["format"] == format_name)

summary = {
    "protobuf_record": protobuf_record,
    "mixed_record": mixed_record,
    "rounds": rounds,
    "messages": results[0]["messages"],
    "results": results,
    "median": {
        "protobuf": {key: median("protobuf", key) for key in ("bytes", "MBps", "msgps", "read_s", "process_s")},
        "mixed_pod": {key: median("mixed_pod", key) for key in ("bytes", "MBps", "msgps", "read_s", "process_s")},
    },
}
summary["median"]["speedup"] = {
    "MBps": summary["median"]["mixed_pod"]["MBps"] / summary["median"]["protobuf"]["MBps"],
    "msgps": summary["median"]["mixed_pod"]["msgps"] / summary["median"]["protobuf"]["msgps"],
    "process": summary["median"]["protobuf"]["process_s"] / summary["median"]["mixed_pod"]["process_s"],
}
with open(os.path.join(outdir, "record_baseline.json"), "w", encoding="utf-8") as handle:
    json.dump(summary, handle, indent=2)

with open(os.path.join(outdir, "summary.md"), "w", encoding="utf-8") as handle:
    p, m, s = summary["median"]["protobuf"], summary["median"]["mixed_pod"], summary["median"]["speedup"]
    handle.write("# Record protobuf/POD performance baseline\n\n")
    handle.write(f"Messages per run: {summary['messages']}; rounds: {rounds}\n\n")
    handle.write("| Format | Payload bytes | MB/s | msg/s | Read s | Process s |\n")
    handle.write("| --- | ---: | ---: | ---: | ---: | ---: |\n")
    handle.write(f"| Protobuf | {p['bytes']:.0f} | {p['MBps']:.3f} | {p['msgps']:.3f} | {p['read_s']:.3f} | {p['process_s']:.3f} |\n")
    handle.write(f"| Mixed POD | {m['bytes']:.0f} | {m['MBps']:.3f} | {m['msgps']:.3f} | {m['read_s']:.3f} | {m['process_s']:.3f} |\n\n")
    handle.write(f"Mixed/Protobuf throughput: {s['MBps']:.3f}x MB/s, {s['msgps']:.3f}x msg/s.\n")
    handle.write(f"Protobuf/Mixed application processing: {s['process']:.3f}x.\n")
    handle.write("Publish only local-storage results; network filesystem runs are diagnostic.\n")
PY

{
  echo "generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_sha=$(git rev-parse HEAD)"
  echo "bazel_version=$(bazel --version)"
  echo "protobuf_record=$PROTOBUF_RECORD"
  echo "mixed_record=$MIXED_RECORD"
  stat -c 'protobuf_size_bytes=%s' "$PROTOBUF_RECORD"
  stat -c 'mixed_size_bytes=%s' "$MIXED_RECORD"
  echo "filesystem=$FS_INFO"
  uname -a
} > "$OUTDIR/metadata.txt"

echo "Record performance reports: $OUTDIR/record_baseline.json and $OUTDIR/summary.md"
