#!/usr/bin/env python3
"""Deterministic Linux archive benchmarks; performance is never a pass/fail gate."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import shutil
import statistics
import subprocess
import tempfile
import time

VERSION = 1
MIB = 1024 * 1024
PROFILES = {
    "standard": {"file_bytes": 4 * MIB, "large_bytes": 64 * MIB,
                 "small_files": 1000, "small_bytes": 128},
    "smoke": {"file_bytes": 4096, "large_bytes": 2 * MIB + 17,
              "small_files": 12, "small_bytes": 128},
}
WORDS = b"archive block compression data file directory byte stream memory reproducible benchmark text test input output".split()
EPOCH = 1700000000


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(MIB), b""):
            result.update(chunk)
    return result.hexdigest()


def chunks(kind, seed, size):
    """SHA-256 counter stream, independent of Python's random implementation."""
    counter = 0
    prefix = f"compressor-benchmark-v{VERSION}:{seed}:{kind}:".encode("ascii")
    while size:
        entropy = hashlib.sha256(prefix + counter.to_bytes(8, "little")).digest()
        if kind == "repeated":
            block = b"ABRACADABRA\x00" * 4096
        elif kind == "text":
            block = b" ".join(WORDS[value % len(WORDS)] for value in entropy) + b".\n"
        else:
            # Hash expansion provides reproducible, incompressible binary data.
            block = b"".join(hashlib.sha256(entropy + i.to_bytes(4, "little")).digest()
                             for i in range(1024))
        yield block[:size]
        size -= min(size, len(block))
        counter += 1


def generate(root, case, seed, config):
    root.mkdir()
    count = config["small_files"] if case == "small_files" else 1
    size = config["small_bytes"] if case == "small_files" else config[
        "large_bytes" if case == "large_file" else "file_bytes"]
    kind = {"small_files": "text", "large_file": "random"}.get(case, case)
    for index in range(count):
        path = root / f"file{index:08d}"
        with path.open("wb") as output:
            for chunk in chunks(kind, seed + index, size):
                output.write(chunk)
        path.chmod(0o644)
        os.utime(path, (EPOCH, EPOCH))
    root.chmod(0o755)
    os.utime(root, (EPOCH, EPOCH))
    return manifest(root)


def manifest(root):
    return [{"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size,
             "sha256": digest(path)} for path in sorted(root.rglob("*")) if path.is_file()]


def capture(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        return result.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def environment(executable, workdir, build_dir):
    repo = pathlib.Path(__file__).resolve().parents[1]
    cache = build_dir / "CMakeCache.txt"
    keys = ("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS", "SODIUM_VERSION:")
    settings = [line for line in cache.read_text().splitlines() if line.startswith(keys)] if cache.exists() else []
    compiler = next((line.split("=", 1)[1] for line in settings if line.startswith("CMAKE_CXX_COMPILER:")), None)
    return {
        "platform": platform.platform(), "machine": platform.machine(),
        "python": platform.python_version(), "cpu_count": os.cpu_count(),
        "cpu_affinity": sorted(os.sched_getaffinity(0)), "load_average": os.getloadavg(),
        "cgroup": pathlib.Path("/proc/self/cgroup").read_text(),
        "cgroup_limits": {name: (pathlib.Path("/sys/fs/cgroup") / name).read_text().strip()
                          for name in ("cpu.max", "memory.max")
                          if (pathlib.Path("/sys/fs/cgroup") / name).is_file()},
        "cpuinfo": pathlib.Path("/proc/cpuinfo").read_text(),
        "meminfo": pathlib.Path("/proc/meminfo").read_text(),
        "mounts": pathlib.Path("/proc/mounts").read_text(),
        "work_dir": str(workdir), "work_device": workdir.stat().st_dev,
        "executable": str(executable), "executable_sha256": digest(executable),
        "git_commit": capture(["git", "-C", str(repo), "rev-parse", "HEAD"]),
        "git_status": capture(["git", "-C", str(repo), "status", "--porcelain"]),
        "cmake_cache": str(cache), "build_settings": settings,
        "compiler_version": capture([compiler, "--version"]) if compiler else None,
        "gnu_time_version": capture(["/usr/bin/time", "--version"]),
        "locale": {key: os.environ.get(key) for key in ("LANG", "LC_ALL", "TZ")},
    }


def measure(command, root):
    rss = root / "rss.txt"
    started = time.perf_counter()
    result = subprocess.run(["/usr/bin/time", "-f", "%M", "-o", str(rss), *map(str, command)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    elapsed = time.perf_counter() - started
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command}\n{result.stderr}")
    return {"wall_seconds": elapsed, "peak_rss_kib": int(rss.read_text().strip())}


def run_case(executable, root, case, seed, config, repeats):
    source = root / "input"
    expected = generate(source, case, seed, config)
    logical = sum(item["bytes"] for item in expected)
    runs = []
    try:
        for index in range(repeats):
            archive = root / "archive.bwa"
            restored = root / "restored"
            compression = measure([executable, "compress", archive, source], root)
            size = archive.stat().st_size
            decompression = measure([executable, "decompress", archive, restored], root)
            if sorted(p.name for p in restored.iterdir()) != ["input"] or manifest(restored / "input") != expected:
                raise RuntimeError(f"Round-trip mismatch in {case}")
            runs.append({"run": index + 1, "archive_bytes": size,
                         "compression_ratio": logical / size,
                         "compress": compression, "decompress": decompression})
            archive.unlink()
            shutil.rmtree(restored)
    finally:
        shutil.rmtree(source)
    medians = {operation: {metric: statistics.median(run[operation][metric] for run in runs)
                          for metric in ("wall_seconds", "peak_rss_kib")}
               for operation in ("compress", "decompress")}
    medians["compression_ratio"] = statistics.median(run["compression_ratio"] for run in runs)
    return {"case": case, "seed": seed, "logical_bytes": logical, "files": expected,
            "runs": runs, "median": medians, "verified": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compressor", type=pathlib.Path, default=pathlib.Path("build/app/compressor"))
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build"), help="CMake build used for environment metadata")
    parser.add_argument("--output", type=pathlib.Path, required=True, help="new JSON report (never overwritten)")
    parser.add_argument("--work-dir", type=pathlib.Path, help="parent of temporary data; selects benchmark filesystem")
    parser.add_argument("--profile", choices=PROFILES, default="standard")
    parser.add_argument("--seed", type=int, default=20261121)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if args.repeats < 1 or args.seed < 0:
        parser.error("repeats must be positive and seed nonnegative")
    if args.output.exists():
        parser.error("output already exists")
    executable = args.compressor.resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        parser.error("compressor must be an executable file")
    if not pathlib.Path("/usr/bin/time").is_file():
        parser.error("GNU time is required at /usr/bin/time")
    with tempfile.TemporaryDirectory(prefix="compressor-benchmark-", dir=args.work_dir) as temp:
        root = pathlib.Path(temp).resolve()
        report = {"schema_version": VERSION, "generator": "sha256-counter-v1",
                  "started_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "profile": args.profile, "parameters": PROFILES[args.profile],
                  "seed": args.seed, "repeats": args.repeats,
                  "environment": environment(executable, root, args.build_dir.resolve()),
                  "method": {"ratio": "logical_bytes / archive_bytes (larger is better)",
                             "wall_time": "perf_counter around GNU time + CLI, including process startup",
                             "memory": "GNU time %M: per-process peak RSS in KiB, separately for each operation",
                             "cache": "not flushed; generated inputs and archives may be cached; no warmup",
                             "verification": "file paths, sizes and SHA-256 after every extraction, outside timing"},
                  "cases": []}
        for case in ("text", "repeated", "random", "large_file", "small_files"):
            result = run_case(executable, root, case, args.seed, PROFILES[args.profile], args.repeats)
            report["cases"].append(result)
            print(f"{case}: ratio={result['median']['compression_ratio']:.3f}, "
                  f"compress={result['median']['compress']['wall_seconds']:.3f}s, "
                  f"decompress={result['median']['decompress']['wall_seconds']:.3f}s", flush=True)
        with args.output.open("x") as output:
            json.dump(report, output, indent=2, ensure_ascii=False)
            output.write("\n")


if __name__ == "__main__":
    main()
