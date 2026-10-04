#!/usr/bin/env python3
"""Validate against native C, then time interleaved Cterpreter/Python runs."""
import argparse
from datetime import datetime, timezone
import json
import hashlib
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
CASES = ("fib", "calls", "integers", "sieve", "bubble", "matrix",
         "mandelbrot", "switch", "strings")


def run(command):
    start = time.perf_counter_ns()
    result = subprocess.run(command, capture_output=True, timeout=120)
    elapsed = (time.perf_counter_ns() - start) / 1e6
    if result.returncode or result.stderr:
        raise RuntimeError(f"{command}: status {result.returncode}\n"
                           f"{result.stderr.decode(errors='replace')}")
    return result.stdout, elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="append", required=True,
                        metavar="LABEL=PATH", help="repeat for before/after binaries")
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--repeats", type=int, default=9)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--cpu", type=int, help="default: first allowed CPU on Linux")
    parser.add_argument("--output", type=Path, help="write metadata and all raw timings as JSON")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    engines = {}
    for spec in args.engine:
        label, sep, path = spec.partition("=")
        if not sep or not label or label == "python" or label in engines:
            parser.error("engines require unique LABEL=PATH; 'python' is reserved")
        engines[label] = str(Path(path).resolve())
    affinity = None
    if hasattr(os, "sched_getaffinity"):
        allowed = os.sched_getaffinity(0)
        cpu = args.cpu if args.cpu is not None else min(allowed)
        os.sched_setaffinity(0, {cpu})
        affinity = cpu
    elif args.cpu is not None:
        parser.error("CPU affinity is unavailable on this platform")
    python = shutil.which(args.python) or args.python
    metadata = {
        "utc": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(), "cpu_affinity": affinity,
        "cpu_model": next((line.split(":", 1)[1].strip()
                           for line in Path("/proc/cpuinfo").read_text().splitlines()
                           if line.startswith("model name")), "unknown")
                     if Path("/proc/cpuinfo").exists() else platform.processor(),
        "python": subprocess.check_output([python, "--version"], text=True).strip(),
        "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
        "engines": engines, "repeats": args.repeats,
        "engine_sha256": {label: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                          for label, path in engines.items()},
        "python_source_sha256": hashlib.sha256((HERE / "workloads.py").read_bytes()).hexdigest(),
        "method": "one warmup; interleaved shuffled runs; wall time includes startup/parsing; medians",
    }
    print(json.dumps(metadata, indent=2), flush=True)
    results = {}
    rng = random.Random(20261004)
    with tempfile.TemporaryDirectory(prefix="cterpreter-bench-") as temporary:
        for name in args.case or CASES:
            source = HERE / f"{name}.c"
            native = str(Path(temporary) / name)
            subprocess.run([args.cc, "-O2", "-std=c17", str(source), "-o", native], check=True)
            expected, _ = run([native])
            commands = {label: [path, "--no-config", "--no-history", "--max-steps", "1000000000", str(source)]
                        for label, path in engines.items()}
            commands["python"] = [python, "-B", str(HERE / "workloads.py"), name]
            samples = {label: [] for label in commands}

            def measure(label, timed):
                output, elapsed = run(commands[label])
                if output != expected:
                    raise RuntimeError(f"{name}/{label}: {output!r} != native {expected!r}")
                if timed:
                    samples[label].append(elapsed)

            for label in commands:
                measure(label, False)
            for _ in range(args.repeats):
                labels = list(commands)
                rng.shuffle(labels)
                for label in labels:
                    measure(label, True)
            medians = {label: statistics.median(values) for label, values in samples.items()}
            results[name] = {"output": expected.decode(), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                             "samples_ms": samples, "median_ms": medians}
            print(name + ": " + ", ".join(f"{label} {ms:.2f} ms" for label, ms in medians.items()), flush=True)
    labels = list(engines) + ["python"]
    totals = {label: sum(case["median_ms"][label] for case in results.values()) for label in labels}
    print("Sum of medians: " + ", ".join(f"{label} {ms:.2f} ms" for label, ms in totals.items()))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"metadata": metadata, "cases": results, "total_ms": totals}, indent=2) + "\n")


if __name__ == "__main__":
    main()
