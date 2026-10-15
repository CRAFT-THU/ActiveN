#!/usr/bin/env python3
"""Profile and BOLT-optimize one simulator, retaining the complete evidence."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time


class Interrupted(BaseException):
    def __init__(self, signum):
        self.signum = signum
        super().__init__(f"interrupted by signal {signum}")


def interrupt(signum, frame):
    raise Interrupted(signum)


def file_record(path):
    path = Path(path).resolve()
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": digest.hexdigest()}


def classify_training(log, exit_status, kind, max_cycles):
    """Reject startup failures, interrupts and failed payloads, including at cap."""
    tag = "System" if kind == "sim_system" else "Single"
    results = re.findall(rf"^\[{tag}\] Result: (\d+) \(0x[0-9a-f]+\)$", log, re.M)
    cycles = re.findall(rf"^\[{tag}\] Cycles: (\d+)$", log, re.M)
    if results:
        if results != ["0"] or exit_status != 0 or len(cycles) != 1 or int(cycles[0]) <= 0:
            raise RuntimeError("BOLT training payload did not complete successfully")
        return {"outcome": "completed", "cycles": int(cycles[0])}
    timed_out = re.findall(rf"^\[{tag}\] Timed out at cycle (\d+)$", log, re.M)
    if exit_status != 1 or timed_out != [str(max_cycles)]:
        raise RuntimeError("BOLT training failed before a valid completion or cycle limit")
    if kind == "sim_system":
        loaded = re.search(r"^\[System\] Loaded [1-9]\d* memory image\(s\)$", log, re.M)
        heartbeat = re.findall(r"^\[HEARTBEAT (\d+)\] mc_reqs=(\d+) periph_reqs=(\d+)", log, re.M)
        progressed = any(int(c) <= max_cycles and int(m) > 0 and int(p) > 0
                         for c, m, p in heartbeat)
        timer = re.findall(r"^\[System\] Timer: ([1-9]\d*) cycles$", log, re.M)
        if not loaded or not progressed or len(timer) != 1:
            raise RuntimeError("BOLT cycle limit has no verified progress into the timed workload")
    else:
        # The single-core driver has no request heartbeat. Require completion.
        raise RuntimeError("Single-core BOLT training must complete its payload")
    return {"outcome": "cycle-limit-with-progress", "cycles": max_cycles}


def profile_has_counts(path):
    """Require an executed edge; LLVM BOLT performs the full format validation."""
    with Path(path).open(encoding="ascii") as stream:
        for line in stream:
            fields = line.split()
            if len(fields) >= 8 and fields[0] in {"0", "1"}:
                try:
                    if int(fields[-1]) > 0:
                        return True
                except ValueError:
                    pass
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bolt", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--kind", choices=("sim_single", "sim_system"), required=True)
    parser.add_argument("--max-cycles", type=int, required=True)
    parser.add_argument("--input", action="append", type=Path, default=[])
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.max_cycles <= 0:
        parser.error("--max-cycles must be positive")
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    binary, bolt = args.binary.resolve(), args.llvm_bolt.resolve()
    root = args.evidence_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    evidence = Path(tempfile.mkdtemp(prefix=f"{timestamp}-", dir=root))
    original = evidence / "sim.unbolted"
    instrumented = evidence / "sim.instrumented"
    optimized = evidence / "sim.optimized"
    profile = evidence / "profile.fdata"
    manifest = evidence / "manifest.json"
    recorded_names = {"PATH", "LD_LIBRARY_PATH", "OMP_NUM_THREADS", "LANG", "LC_ALL"}
    prefixes = ("SOFT_", "MORRISON_", "COSIM_", "VERILATOR_")
    record = {"format": "activen-bolt-build-v1", "status": "started",
              "cwd": str(Path.cwd()), "kind": args.kind,
              "max_cycles": args.max_cycles, "arguments": arguments,
              "environment": {k: v for k, v in os.environ.items()
                              if k in recorded_names or k.startswith(prefixes)},
              "commands": [], "inputs": [], "started_utc": timestamp}
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, interrupt)

    def save():
        temporary = manifest.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(record, indent=2) + "\n")
        os.replace(temporary, manifest)

    def run(label, argv):
        log = evidence / f"{label}.log"
        entry = {"argv": [str(x) for x in argv], "cwd": str(Path.cwd()),
                 "log": str(log), "started_utc": datetime.now(timezone.utc).isoformat()}
        record["commands"].append(entry)
        save()
        start = time.monotonic()
        process = None
        try:
            with log.open("wb") as stream:
                # Defer interruption until Popen has returned ownership of the
                # child. Do not block signals inherited by the child itself.
                pending_signals = []
                handlers = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM)}
                for s in handlers:
                    signal.signal(s, lambda signum, frame: pending_signals.append(signum))
                try:
                    process = subprocess.Popen(entry["argv"], stdout=stream,
                                               stderr=subprocess.STDOUT, start_new_session=True)
                    entry["pid"] = process.pid
                finally:
                    for s, handler in handlers.items():
                        signal.signal(s, handler)
                if pending_signals:
                    raise Interrupted(pending_signals[0])
                save()
                process.wait()
        except BaseException as error:
            if isinstance(error, (Interrupted, KeyboardInterrupt)):
                signum = int(getattr(error, "signum", signal.SIGINT))
                entry["interrupted_signal"] = signum
            else:
                signum = signal.SIGTERM
                entry["execution_error"] = str(error)
            if process is not None and process.poll() is None:
                try:
                    os.killpg(process.pid, signum)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    entry["forced_kill"] = True
            raise
        finally:
            entry.update(exit_status=process.returncode if process else None,
                         elapsed_seconds=time.monotonic() - start,
                         log_sha256=file_record(log)["sha256"] if log.exists() else None)
            save()
        return process.returncode, log

    try:
        shutil.copy2(binary, original)
        shutil.copy2(__file__, evidence / "bolt_optimize.py")
        candidates = {p.resolve() for p in args.input}
        candidates.update(Path(a).resolve() for a in arguments if Path(a).is_file())
        explicit = {p.resolve() for p in args.input}
        for index, path in enumerate(sorted(candidates)):
            item = file_record(path)
            if path in explicit:
                frozen = evidence / "inputs" / f"{index:03}-{path.name}"
                frozen.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, frozen)
                item["frozen"] = str(frozen)
            record["inputs"].append(item)
        record["unbolted"] = file_record(original)
        record["optimizer"] = file_record(bolt)
        record["helper"] = file_record(evidence / "bolt_optimize.py")
        save()
        code, _ = run("version", [bolt, "--version"])
        if code:
            raise RuntimeError("Cannot identify LLVM BOLT")
        code, _ = run("instrument", [bolt, original, "--instrument",
                       f"--instrumentation-file={profile}", "-o", instrumented])
        if code:
            raise RuntimeError("BOLT instrumentation failed")
        record["instrumented"] = file_record(instrumented)
        code, log = run("training", [instrumented, "--max-cycles", str(args.max_cycles), *arguments])
        record["training"] = classify_training(log.read_text(errors="replace"), code,
                                               args.kind, args.max_cycles)
        if not profile.is_file() or not profile_has_counts(profile):
            raise RuntimeError("BOLT training produced no executed profile counts")
        record["profile"] = file_record(profile)
        code, _ = run("optimize", [bolt, original, "-o", optimized, f"-data={profile}",
                      "-reorder-blocks=ext-tsp", "-reorder-functions=hfsort",
                      "-split-functions", "-split-all-cold"])
        if code:
            raise RuntimeError("BOLT optimization rejected the training profile")
        record["optimized"] = file_record(optimized)
        # Retain both versions and publish only after every validation succeeds.
        published = evidence / "sim.publish"
        shutil.copy2(optimized, published)
        os.replace(published, binary)
        record["published"] = file_record(binary)
        record["status"] = "completed"
        return 0
    except (Interrupted, KeyboardInterrupt) as error:
        signum = int(getattr(error, "signum", signal.SIGINT))
        record.update(status="interrupted", signal=signum, error=str(error))
        print(f"BOLT interrupted. Evidence: {manifest}", file=sys.stderr)
        return 128 + signum
    except Exception as error:
        record.update(status="failed", error=str(error))
        print(f"BOLT failed: {error}. Evidence: {manifest}", file=sys.stderr)
        return 1
    finally:
        record["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save()
        print(f"BOLT evidence: {manifest}")


if __name__ == "__main__":
    sys.exit(main())
