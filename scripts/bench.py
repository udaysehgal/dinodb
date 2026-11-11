#!/usr/bin/env python3
"""Benchmark DinoDB against Redis with redis-benchmark.

Starts each server configuration in turn on a scratch port, runs the same
redis-benchmark workloads against it, and prints markdown tables of the
median over several runs of:

- throughput, computed from wall-clock time. redis-benchmark's own "rps"
  is quantized in --threads mode (short runs snap to values like exactly
  1,000,000 or 2,000,000), so it isn't used;
- p99 latency, as reported by redis-benchmark;
- server CPU time (user + sys) per request, from the server process's
  rusage. This is independent of whether the client or the server is the
  bottleneck, which on a single machine is often the client.

The request count scales with pipeline depth so that every run lasts
several seconds.

Usage:
    python3 scripts/bench.py [--dinodb build/dinodb] [--runs 3]
                             [--requests 500000] [--clients 50]
                             [--pipelines 1,16] [--tests set,get,incr,lpush]
                             [--configs redis,dinodb-1,dinodb-4]

Config names: "redis", "redis-io<N>" (Redis with io-threads N),
"dinodb-<N>" (DinoDB with N worker threads). Requires redis-server and
redis-benchmark on PATH.
"""
import argparse
import csv
import io
import os
import platform
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time

PORT = 7499


def wait_for_port(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"server did not start on port {port}")


def start_server(config, dinodb, workdir):
    if config == "redis" or config.startswith("redis-io"):
        cmd = ["redis-server", "--port", str(PORT), "--save", "", "--appendonly", "no",
               "--dir", workdir, "--protected-mode", "no"]
        if config.startswith("redis-io"):
            cmd += ["--io-threads", config[len("redis-io"):]]
    elif config.startswith("dinodb-"):
        cmd = [dinodb, "--port", str(PORT), "--workers", config[len("dinodb-"):],
               "--autosave", "0", "--rdb", os.path.join(workdir, "bench.rdb")]
    else:
        raise ValueError(f"unknown config {config!r}")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_for_port(PORT)
    return proc


def stop_server(proc):
    """Stops the server and returns the CPU seconds (user + sys) it used."""
    proc.terminate()
    deadline = time.time() + 10
    while True:
        pid, _, usage = os.wait4(proc.pid, os.WNOHANG)
        if pid:
            proc.returncode = 0  # reaped by wait4; keep Popen from waiting again
            return usage.ru_utime + usage.ru_stime
        if time.time() > deadline:
            proc.kill()
        time.sleep(0.02)


# Server-bound workloads: every request reads one shared list and the server
# encodes N elements per reply, so the work is in the server, not the client.
# The list is filled before timing starts so the timed run is pure LRANGE.
LRANGE_TESTS = {"lrange_100": 99, "lrange_300": 299, "lrange_600": 599}
LIST_LEN = 600


def prefill_list():
    s = socket.create_connection(("127.0.0.1", PORT))
    args = ["RPUSH", "mylist"] + [f"element-{i:04d}" for i in range(LIST_LEN)]
    s.sendall(f"*{len(args)}\r\n".encode() + b"".join(
        f"${len(a)}\r\n{a}\r\n".encode() for a in args))
    reply = s.recv(64)
    s.close()
    if not reply.startswith(b":"):
        raise RuntimeError(f"prefill failed: {reply!r}")


def run_benchmark(test, pipeline, requests, args):
    cmd = ["redis-benchmark", "-p", str(PORT), "-n", str(requests),
           "-c", str(args.clients), "-P", str(pipeline),
           "--threads", str(args.bench_threads), "--csv", "-q"]
    if test in LRANGE_TESTS:
        prefill_list()
        cmd += ["LRANGE", "mylist", "0", str(LRANGE_TESTS[test])]
    else:
        cmd += ["-t", test, "-r", str(args.keyspace)]
    t0 = time.perf_counter()
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    elapsed = time.perf_counter() - t0
    rows = list(csv.DictReader(io.StringIO(out)))
    if not rows:
        raise RuntimeError(f"no output from: {' '.join(cmd)}")
    return requests / elapsed, float(rows[0]["p99_latency_ms"])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dinodb", default=os.path.join(os.path.dirname(__file__), "..", "build", "dinodb"))
    p.add_argument("--configs", default="redis,redis-io4,dinodb-1,dinodb-2,dinodb-4")
    p.add_argument("--tests", default="set,get,incr",
                   help="redis-benchmark -t names, or " + ", ".join(LRANGE_TESTS))
    p.add_argument("--pipelines", default="1,16")
    p.add_argument("--requests", type=int, default=1_000_000,
                   help="requests per run at pipeline depth 1; multiplied by the depth otherwise")
    p.add_argument("--clients", type=int, default=50)
    p.add_argument("--keyspace", type=int, default=1_000_000, help="random key range (-r)")
    p.add_argument("--bench-threads", type=int, default=4, help="redis-benchmark --threads")
    p.add_argument("--runs", type=int, default=3)
    args = p.parse_args()

    for tool in ("redis-server", "redis-benchmark"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found on PATH")
    args.dinodb = os.path.abspath(args.dinodb)

    configs = args.configs.split(",")
    tests = args.tests.split(",")
    pipelines = [int(x) for x in args.pipelines.split(",")]
    results = {}  # (config, test, pipeline) -> (rps, p99_ms, server_cpu_us_per_request)

    with tempfile.TemporaryDirectory() as workdir:
        for config in configs:
            for pipeline in pipelines:
                for test in tests:
                    requests = args.requests * pipeline
                    samples = []
                    for _ in range(args.runs):
                        # Fresh server per run so earlier runs' data doesn't skew later ones.
                        proc = start_server(config, args.dinodb, workdir)
                        try:
                            rps, p99 = run_benchmark(test, pipeline, requests, args)
                        finally:
                            cpu = stop_server(proc)
                        samples.append((rps, p99, cpu / requests * 1e6))
                    med = tuple(statistics.median(s[i] for s in samples) for i in range(3))
                    results[(config, test, pipeline)] = med
                    print(f"{config:>10} P={pipeline:<3} {test:>6}: {med[0]:>12,.0f} rps  "
                          f"p99 {med[1]:.3f} ms  cpu {med[2]:.2f} us/req", file=sys.stderr)

    print(f"\n{platform.machine()} / {platform.system()}, {os.cpu_count()} CPUs; "
          f"{args.clients} clients, keyspace {args.keyspace:,}, "
          f"redis-benchmark --threads {args.bench_threads}, median of {args.runs} runs\n")
    for pipeline in pipelines:
        print(f"**Pipeline depth {pipeline}**: throughput in requests/sec (p99 latency ms) "
              f"· server CPU µs per request\n")
        header = " | ".join(f"{t.upper()} | {t.upper()} CPU" for t in tests)
        print(f"| Config | {header} |")
        print("|---|" + "---:|---:|" * len(tests))
        for config in configs:
            cells = []
            for t in tests:
                rps, p99, cpu = results[(config, t, pipeline)]
                cells.append(f"{rps:,.0f} ({p99:.2f}) | {cpu:.2f}")
            print(f"| {config} | " + " | ".join(cells) + " |")
        print()


if __name__ == "__main__":
    main()
