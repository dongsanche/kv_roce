#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Report per-priority read latency for a KV-offload incast runlog.

Maps taskId -> priority from traffic.csv, then reads WQE Starts/Completes from
TaskTrace_node_*.csv to compute per-priority P50/P99/mean latency. Used to
verify QoS isolation: high-priority flows should show lower latency than
low-priority flows under incast.

Usage:
    python3 analyze_qos.py scratch/kv-offload-incast
"""
import csv
import glob
import sys
from collections import defaultdict


def main(argv):
    case_dir = argv[1] if len(argv) > 1 else "scratch/kv-offload-incast"

    priority = {}
    with open(f"{case_dir}/traffic.csv") as f:
        for row in csv.DictReader(f):
            priority[int(row["taskId"])] = int(row["priority"])

    starts = {}
    ends = {}
    for path in glob.glob(f"{case_dir}/runlog/TaskTrace_node_*.csv"):
        with open(path) as f:
            for row in csv.DictReader(f):
                t = int(row["time_ps"])
                event = row["event"]
                tid = int(row["task_id"])
                if event == "WQE Starts":
                    starts[tid] = min(starts.get(tid, 1 << 60), t)
                elif event == "WQE Completes":
                    ends[tid] = max(ends.get(tid, 0), t)

    latencies = defaultdict(list)
    for tid, end in ends.items():
        latencies[priority.get(tid, -1)].append((end - starts.get(tid, 0)) / 1e6)

    if not latencies:
        print("no completed tasks")
        return 1

    for pri in sorted(latencies):
        vals = sorted(latencies[pri])
        mean = sum(vals) / len(vals)
        print(f"priority {pri}: n={len(vals)} P50={vals[len(vals) // 2]:.1f}us "
              f"P99={vals[-1]:.1f}us mean={mean:.1f}us")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
