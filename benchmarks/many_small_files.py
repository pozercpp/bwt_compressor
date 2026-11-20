#!/usr/bin/env python3
"""Repeatable archive benchmark for many small files (Linux, Python 3)."""
import argparse
import os
import pathlib
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compressor", type=pathlib.Path, default=pathlib.Path("build/app/compressor"))
    parser.add_argument("--files", type=int, default=1000)
    parser.add_argument("--bytes", type=int, default=128, help="bytes per file")
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if args.files < 1 or args.bytes < 0 or args.repeats < 1:
        parser.error("files/repeats must be positive and bytes nonnegative")
    executable = args.compressor.resolve()
    with tempfile.TemporaryDirectory(prefix="compressor-small-files-") as temp:
        root = pathlib.Path(temp)
        source = root / "input"
        source.mkdir()
        payload = bytes(range(256))[:args.bytes] if args.bytes <= 256 else (bytes(range(256)) * ((args.bytes + 255) // 256))[:args.bytes]
        for i in range(args.files):
            (source / f"f{i:08d}").write_bytes(payload)
        archive = root / "result.bwa"
        logical = args.files * args.bytes
        print(f"files={args.files} bytes_per_file={args.bytes} logical_bytes={logical} repeats={args.repeats}")
        print("run\tseconds\tarchive_bytes\toverhead_bytes\toverhead_per_file\tmax_rss_kib")
        for run in range(1, args.repeats + 1):
            rss_file = root / "rss.txt"
            started = time.perf_counter()
            subprocess.run(["/usr/bin/time", "-f", "%M", "-o", str(rss_file), str(executable), "compress", str(archive), str(source)],
                           stdout=subprocess.DEVNULL, check=True)
            elapsed = time.perf_counter() - started
            archive_bytes = archive.stat().st_size
            overhead = archive_bytes - logical
            print(f"{run}\t{elapsed:.6f}\t{archive_bytes}\t{overhead}\t{overhead / args.files:.3f}\t{rss_file.read_text().strip()}")
            archive.unlink()


if __name__ == "__main__":
    main()
