#!/usr/bin/env python3
"""Benchmark correctness tests: no performance thresholds."""
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
from unittest import mock

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "benchmarks" / "run.py"
spec = importlib.util.spec_from_file_location("benchmark", SCRIPT)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="benchmark-test-") as temp:
        root = pathlib.Path(temp)
        config = {"file_bytes": 1031, "large_bytes": 2053, "small_files": 3, "small_bytes": 37}
        for case in ("text", "repeated", "random", "large_file", "small_files"):
            first = benchmark.generate(root / (case + "1"), case, 42, config)
            second = benchmark.generate(root / (case + "2"), case, 42, config)
            check(first == second, "generator is not reproducible")
            count = 3 if case == "small_files" else 1
            size = 37 if case == "small_files" else (2053 if case == "large_file" else 1031)
            check(len(first) == count and all(item["bytes"] == size for item in first), "incorrect corpus size")
            for path in (root / (case + "1")).iterdir():
                check(path.stat().st_mtime_ns == benchmark.EPOCH * 10**9, "mtime is not fixed")
                check(path.stat().st_mode & 0o777 == 0o644, "mode is not fixed")
        golden = {
            "text": "3c59764fd249c260b49cc64d71228eff7dd5394bc18206b4ec81517c849473f5",
            "random": "822247ce969ee3dc7fb6ab81433389ee57e509f6d86093a9dea6c397275721a0",
        }
        for kind in ("random", "text"):
            a = b"".join(benchmark.chunks(kind, 42, 1000))
            b = b"".join(benchmark.chunks(kind, 43, 1000))
            check(a != b, "seed does not affect corpus")
            check(hashlib.sha256(a).hexdigest() == golden[kind], "v1 corpus changed")
        check(b"".join(benchmark.chunks("repeated", 42, 15)) == b"ABRACADABRA\x00ABR", "repetition changed")
        try:
            benchmark.measure(["/bin/false"], root)
        except RuntimeError:
            pass
        else:
            raise AssertionError("failed subprocess accepted")
        failure_root = root / "corrupted"
        failure_root.mkdir()
        original_measure = benchmark.measure

        def corrupt(command, directory):
            result = original_measure(command, directory)
            if command[1] == "decompress":
                (directory / "restored" / "input" / "file00000000").write_bytes(b"corrupted")
            return result

        with mock.patch.object(benchmark, "measure", side_effect=corrupt):
            try:
                benchmark.run_case(executable, failure_root, "text", 42, config, 1)
            except RuntimeError as error:
                check("Round-trip mismatch" in str(error), "unexpected verification error")
            else:
                raise AssertionError("corrupted extraction accepted")
        check(not (failure_root / "input").exists(), "source leaked after verification error")
        report = root / "report.json"
        command = [sys.executable, str(SCRIPT), "--compressor", str(executable),
                   "--profile", "smoke", "--repeats", "1", "--seed", "42",
                   "--work-dir", str(root), "--output", str(report)]
        subprocess.run(command, check=True, timeout=180)
        data = json.loads(report.read_text())
        check(data["seed"] == 42 and data["schema_version"] == 1, "missing version/seed")
        check(data["environment"]["executable_sha256"] == benchmark.digest(executable), "binary identity mismatch")
        check(data["environment"]["cpu_count"] and data["environment"]["platform"], "missing environment")
        check(len(data["cases"]) == 5, "missing cases")
        for case in data["cases"]:
            check(case["verified"] and len(case["runs"]) == 1, "missing verification/runs")
            run = case["runs"][0]
            check(case["logical_bytes"] == sum(item["bytes"] for item in case["files"]), "wrong input size")
            check(run["compression_ratio"] == case["logical_bytes"] / run["archive_bytes"], "wrong ratio")
            for operation in ("compress", "decompress"):
                check(run[operation]["wall_seconds"] >= 0, "invalid time")
                check(isinstance(run[operation]["peak_rss_kib"], int) and run[operation]["peak_rss_kib"] >= 0, "invalid RSS")
                check(case["median"][operation] == run[operation], "wrong median")
        before = report.read_bytes()
        check(subprocess.run(command, capture_output=True).returncode != 0, "existing report overwritten")
        check(report.read_bytes() == before, "existing report changed")
        check(not list(root.glob("compressor-benchmark-*")), "temporary files leaked")
        for option, value in (("--repeats", "0"), ("--seed", "-1")):
            invalid = command[:-2] + ["--output", str(root / "invalid.json"), option, value]
            check(subprocess.run(invalid, capture_output=True).returncode != 0, "invalid argument accepted")
    print("Benchmark correctness checks passed.")


if __name__ == "__main__":
    main()
