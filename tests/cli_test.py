"""End-to-end CLI checks using isolated temporary files."""
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

executable = pathlib.Path(sys.argv[1]).resolve()
backend = pathlib.Path(sys.argv[2]).resolve()


def run(*args, expected=0):
    result = subprocess.run([str(executable), *map(str, args)], capture_output=True, timeout=60)
    if result.returncode != expected:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result


def legacy_create(source, destination):
    subprocess.run([str(backend), "compress", str(source), str(destination), "2"],
                   check=True, capture_output=True, timeout=60)


with tempfile.TemporaryDirectory(prefix="compressor-cli-") as directory:
    root = pathlib.Path(directory)
    assert b"--threads" not in run("--help").stdout
    help_text = run('--help').stdout
    assert b'does NOT verify contents or CRC' in help_text
    assert b'fully decode' in help_text
    for command in ('list', 'test'):
        run(command, expected=2)
        run(command, 'missing.bwa', 'extra', expected=2)
        run(command, '--unknown', expected=2)
        run(command, 'missing.bwa', expected=1)
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
        legacy_create(source, archive)
        assert run('list', archive).stdout == f'file\t{len(sample)}\t(unnamed)\n'.encode()
        run('test', archive)
        assert not restored.exists()
        run("decompress", archive, restored)
        assert restored.read_bytes() == sample
        assert source.read_bytes() == sample
        archive.unlink()
        restored.unlink()

    # More blocks than the maximum in-flight window, with unequal costs.
    block_size = 1024 * 1024
    mixed = b"".join(bytes([i]) * block_size if i % 3 else rng.randbytes(block_size)
                     for i in range(10)) + b"last partial block"
    source.write_bytes(mixed)
    archives = []
    for threads in (1, 2, 4):
        path = root / f"parallel-{threads}.bwa"
        subprocess.run([str(backend), "compress", str(source), str(path), str(threads)],
                       check=True, capture_output=True, timeout=60)
        archives.append(path.read_bytes())
        subprocess.run([str(backend), "decompress", str(path), str(restored), str(threads)],
                       check=True, capture_output=True, timeout=60)
        assert restored.read_bytes() == mixed
        restored.unlink()
    assert archives[0] == archives[1] == archives[2], "Block order or format changed"
    # Error in a later worker, with already written and queued blocks.
    late_bad = bytearray(archives[0])
    offset = 4
    for _ in range(5):
        offset += 4 + struct.unpack_from("<I", late_bad, offset)[0]
    late_bad[offset + 4 + 13] ^= 1
    late_archive = root / "late-error.bwa"
    late_archive.write_bytes(late_bad)
    run('test', late_archive, expected=1)
    run('list', late_archive)  # A late CRC error must not imply listing checks CRC.
    run("decompress", late_archive, restored, expected=1)
    assert not restored.exists()
    for count in ("1", "4", "0", "5", "-1", "abc", "2x", "", "9999999999999999999"):
        run("compress", source, restored, "--threads", count, expected=2)
        assert not restored.exists()
    run("compress", source, restored, "--unknown", 2, expected=2)

    source.write_bytes(b"abc" * 800000)
    legacy_create(source, archive)
    valid = archive.read_bytes()
    assert valid[:4] == b"BWA1"
    # The output must not overwrite an existing file or the input itself.
    run("compress", source, source, expected=1)
    assert source.read_bytes() == b"abc" * 800000
    run("compress", archive, source, expected=1)
    assert archive.read_bytes() == valid
    restored.write_bytes(b"keep me")
    run("decompress", archive, restored, expected=1)
    assert restored.read_bytes() == b"keep me"
    restored.unlink()
    run("compress", restored, root / "missing", expected=1)
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
        run('test', bad, expected=1)
        run('list', bad, expected=0 if contents == changed_crc else 1)
        run("decompress", bad, restored, expected=1)
        assert not restored.exists(), "Incomplete output was left behind"

print("CLI tests passed")
