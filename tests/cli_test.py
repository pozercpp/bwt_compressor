"""End-to-end CLI checks using isolated temporary files."""
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

executable = pathlib.Path(sys.argv[1]).resolve()


def run(*args, expected=0):
    result = subprocess.run([str(executable), *map(str, args)], capture_output=True)
    if result.returncode != expected:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result


with tempfile.TemporaryDirectory(prefix="compressor-cli-") as directory:
    root = pathlib.Path(directory)
    run("--help")
    run(expected=2)
    run("unknown", "a", "b", expected=2)
    source = root / "source with spaces.bin"
    archive = root / "archive.bwa"
    restored = root / "restored.bin"
    rng = random.Random(42)
    samples = [b"", "Hello, мир! $\n".encode(), bytes(range(256)),
               b"a" * (1024 * 1024), b"abc" * 800000,
               rng.randbytes(1024 * 1024 + 123)]
    for sample in samples:
        source.write_bytes(sample)
        run("compress", source, archive)
        run("decompress", archive, restored)
        assert restored.read_bytes() == sample
        assert source.read_bytes() == sample
        archive.unlink()
        restored.unlink()

    source.write_bytes(b"abc" * 800000)
    run("compress", source, archive)
    valid = archive.read_bytes()
    assert valid[:4] == b"BWA1"
    # The output must not overwrite an existing file or the input itself.
    run("compress", source, source, expected=1)
    assert source.read_bytes() == b"abc" * 800000
    run("compress", source, archive, expected=1)
    assert archive.read_bytes() == valid
    restored.write_bytes(b"keep me")
    run("decompress", archive, restored, expected=1)
    assert restored.read_bytes() == b"keep me"
    restored.unlink()
    run("compress", root / "missing", restored, expected=1)
    assert not restored.exists()
    run("compress", root, restored, expected=1)
    assert not restored.exists()

    bad = root / "bad.bwa"
    changed_footer = bytearray(valid)
    changed_footer[-1] ^= 1
    changed_crc = bytearray(valid)
    changed_crc[8 + 13] ^= 1
    first_size = struct.unpack_from("<I", valid, 4)[0]
    # This truncation follows a successfully decoded block, so cleanup is tested.
    truncated_after_block = valid[:8 + first_size]
    corruptions = [b"", b"BAD!", valid[:-1], valid + b"extra", changed_footer,
                   changed_crc, truncated_after_block,
                   b"BWA1" + struct.pack("<I", 0xFFFFFFFF)]
    for contents in corruptions:
        bad.write_bytes(contents)
        run("decompress", bad, restored, expected=1)
        assert not restored.exists(), "Incomplete output was left behind"

print("CLI tests passed")
