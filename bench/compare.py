#!/usr/bin/env python3
"""Benchmarks kvstore against redis-server with redis-benchmark under identical settings.

Both servers run on the same machine with the same persistence settings, and each test
runs against one server at a time. Results are printed as a Markdown table and written
as JSON to bench/results/.

    python3 bench/compare.py --binary build-release/kvstore --requests 1000000
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import platform
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def wait_ready(port: int, process: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            sys.exit(f"server on port {port} exited with {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2) as s:
                s.sendall(b"PING\r\n")
                if s.recv(16).startswith(b"+PONG"):
                    return
        except OSError:
            time.sleep(0.05)
    sys.exit(f"server on port {port} did not start")


def run_benchmark(port: int, tests: str, requests: int, clients: int, pipeline: int, size: int) -> list[dict]:
    command = [
        "redis-benchmark", "-p", str(port), "-t", tests, "-n", str(requests), "-c", str(clients),
        "-P", str(pipeline), "-d", str(size), "-r", "100000", "--csv",
    ]
    output = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    rows = []
    for row in csv.DictReader(io.StringIO(output)):
        rows.append({
            "test": row["test"].split()[0],
            "rps": float(row["rps"]),
            "p50_ms": float(row["p50_latency_ms"]),
            "p99_ms": float(row["p99_latency_ms"]),
        })
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default="build-release/kvstore")
    parser.add_argument("--tests", default="set,get,incr,lpush,lpop,hset")
    parser.add_argument("--requests", type=int, default=500_000)
    parser.add_argument("--clients", type=int, default=50)
    parser.add_argument("--pipelines", default="1,16")
    parser.add_argument("--size", type=int, default=64, help="value size in bytes")
    parser.add_argument("--persistence", choices=["none", "everysec", "always"], default="none")
    args = parser.parse_args()

    for tool in ("redis-server", "redis-benchmark"):
        if shutil.which(tool) is None:
            sys.exit(f"{tool} not found on PATH")

    persistence = [] if args.persistence == "none" else ["--appendonly", "yes", "--appendfsync", args.persistence]
    results: dict[str, list[dict]] = {}
    with tempfile.TemporaryDirectory() as kv_dir, tempfile.TemporaryDirectory() as redis_dir:
        servers = {
            "redis": ["redis-server", "--save", "", "--dir", redis_dir, *(persistence or ["--appendonly", "no"])],
            "kvstore": [args.binary, "--dir", kv_dir, *persistence],
        }
        processes = {}
        ports = {}
        try:
            for name, command in servers.items():
                ports[name] = free_port()
                processes[name] = subprocess.Popen(
                    [*command, "--port", str(ports[name])], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                )
                wait_ready(ports[name], processes[name])
            version = subprocess.run(["redis-server", "--version"], capture_output=True, text=True).stdout.split()[2]
            for pipeline in (int(p) for p in args.pipelines.split(",")):
                for name in servers:
                    print(f"benchmarking {name}, pipeline {pipeline} ...", file=sys.stderr)
                    rows = run_benchmark(ports[name], args.tests, args.requests, args.clients, pipeline, args.size)
                    results[f"{name}/{pipeline}"] = rows
        finally:
            for process in processes.values():
                process.terminate()
                process.wait()

    print(f"\n{args.requests:,} requests per test, {args.clients} clients, {args.size}-byte values, "
          f"persistence: {args.persistence}, {version.replace('v=', 'Redis ')}\n")
    print("| Test | Pipeline | Redis req/s | kvstore req/s | Ratio | Redis p50 / p99 ms | kvstore p50 / p99 ms |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    for pipeline in (int(p) for p in args.pipelines.split(",")):
        redis_rows = {r["test"]: r for r in results[f"redis/{pipeline}"]}
        for row in results[f"kvstore/{pipeline}"]:
            ref = redis_rows[row["test"]]
            print(
                f"| {row['test']} | {pipeline} | {ref['rps']:,.0f} | {row['rps']:,.0f} | {row['rps'] / ref['rps']:.2f} "
                f"| {ref['p50_ms']:.3f} / {ref['p99_ms']:.3f} | {row['p50_ms']:.3f} / {row['p99_ms']:.3f} |"
            )

    out_dir = Path(__file__).parent / "results"
    out_dir.mkdir(exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    report = {
        "generated_at": stamp,
        "machine": platform.platform(),
        "settings": vars(args),
        "redis_version": version,
        "results": results,
    }
    path = out_dir / f"compare-{stamp}.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"\nwritten to {path}", file=sys.stderr)


if __name__ == "__main__":
    main()
