"""BWA2 integration and hostile metadata tests, using isolated directories."""
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

exe = pathlib.Path(sys.argv[1]).resolve()


def run(*args, expected=0):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, timeout=60)
    assert result.returncode == expected, (args, result.returncode, result.stderr)
    return result


def snapshot(root):
    return {str(p.relative_to(root)): None if p.is_dir() else p.read_bytes()
            for p in root.rglob('*')}


def record(kind, name, content=b''):
    return bytes([kind]) + struct.pack('<I', len(name)) + name + content


def nested(data=b''):
    stream = b'BWA1'
    if data:
        block = b'BWC1\0' + struct.pack('<III', len(data), 0, zlib.crc32(data)) + data
        stream += struct.pack('<I', len(block)) + block
    return stream + struct.pack('<IQ', 0, len(data))


def file_record(name, data=b''):
    return record(2, name, struct.pack('<Q', len(data)) + nested(data))


def container(records, count, total=0):
    return b'BWA2' + records + b'\0' + struct.pack('<QQ', count, total)


with tempfile.TemporaryDirectory(prefix='bwa2-tests-') as temporary:
    root = pathlib.Path(temporary)
    source = root / 'проект с пробелами'
    source.mkdir()
    (source / 'empty').mkdir()
    (source / 'nested').mkdir()
    (source / 'nested' / 'пустой.txt').write_bytes(b'')
    (source / 'nested' / 'binary.bin').write_bytes(bytes(range(256)) * 17)
    (source / 'large.bin').write_bytes(random.Random(42).randbytes(1024 * 1024 + 11))
    extra = root / 'extra.txt'
    extra.write_text('Hello, мир!\n')
    archive = root / 'all.bwa'
    destination = root / 'restored'
    run('compress', archive, source, extra)
    valid = archive.read_bytes()
    assert valid[:4] == b'BWA2'
    run('decompress', archive, destination)
    assert snapshot(destination / source.name) == snapshot(source)
    assert (destination / extra.name).read_bytes() == extra.read_bytes()
    # Existing output directories/files and symlinks are never overwritten.
    run('decompress', archive, destination, expected=1)
    assert snapshot(destination / source.name) == snapshot(source)
    run('compress', archive, extra, expected=1)
    assert archive.read_bytes() == valid
    shutil.rmtree(destination)
    destination.symlink_to(source, target_is_directory=True)
    run('decompress', archive, destination, expected=1)
    assert destination.is_symlink()
    destination.unlink()
    destination.symlink_to(root / 'nonexistent')
    run('decompress', archive, destination, expected=1)
    destination.unlink()

    # Deterministic enumeration, independent of input order.
    other = root / 'other.bwa'
    run('compress', other, extra, source)
    assert other.read_bytes() == valid

    # Output within the input directory is not included in its own archive.
    expected_tree = snapshot(source)
    inside = source / 'inside.bwa'
    run('compress', inside, source)
    run('decompress', inside, destination)
    assert snapshot(destination / source.name) == expected_tree
    inside.unlink()
    shutil.rmtree(destination)

    # Empty files/directories as standalone inputs.
    empty_archive = root / 'empty.bwa'
    run('compress', empty_archive, source / 'empty', source / 'nested' / 'пустой.txt')
    run('decompress', empty_archive, destination)
    assert snapshot(destination) == {'empty': None, 'пустой.txt': b''}
    shutil.rmtree(destination)

    # Same basenames, repeated roots, links and special files are rejected.
    conflicting = root / 'conflicts'
    conflicting.mkdir()
    (conflicting / extra.name).write_bytes(b'different')
    failed = root / 'failed.bwa'
    run('compress', failed, extra, conflicting / extra.name, expected=1)
    run('compress', failed, source, source, expected=1)
    link = source / 'link'
    link.symlink_to(extra)
    run('compress', failed, source, expected=1)
    run('compress', failed, link, expected=1)
    link.unlink()
    import os
    fifo = source / 'pipe'
    os.mkfifo(fifo)
    run('compress', failed, source, expected=1)
    fifo.unlink()
    assert not failed.exists()

    malformed = root / 'bad.bwa'
    sentinel = root / 'escape'
    sentinel.write_bytes(b'unchanged')
    def reject(data):
        malformed.write_bytes(data)
        run('decompress', malformed, destination, expected=1)
        assert not destination.exists() and not destination.is_symlink()
        assert sentinel.read_bytes() == b'unchanged'
        assert not list(root.glob('.compressor-*')), 'Staging output was not cleaned'

    for name in [b'../escape', b'/tmp/escape', b'C:/escape', b'..\\escape',
                 b'foo/../../escape', b'foo//bar', b'.', b'..', b'a/./b',
                 b'foo/', b'\0escape', b'\xff', b'\xc0\xaf', b'\xed\xa0\x80']:
        reject(container(file_record(name), 1))
    reject(container(record(1, b'a') + record(1, b'a'), 2))
    reject(container(file_record(b'a') + file_record(b'a/b'), 2))
    reject(container(file_record(b'missing/child'), 1))
    reject(container(record(1, b'a') + file_record(b'a'), 2))
    reject(container(bytes([3]), 1))  # no symlink record type
    reject(b'BWA2\x01' + struct.pack('<I', 0xffffffff))
    reject(container(record(2, b'x', struct.pack('<Q', 0) + nested(b'overflow')), 1))
    reject(container(record(2, b'x', struct.pack('<Q', 5) + nested()), 1))
    reject(container(file_record(b'x'), 2))  # wrong entry count
    reject(container(file_record(b'x'), 1, 1))  # wrong total size
    for size in [0, 4, 5, len(valid) // 2, len(valid) - 1]:
        reject(valid[:size])
    reject(valid + b'trailing')
    # A later record fails after files have already been extracted into staging.
    reject(container(file_record(b'good', b'ok') + file_record(b'../escape'), 2, 2))
    corrupted = bytearray(container(file_record(b'file', b'hello'), 1, 5))
    block_offset = corrupted.index(b'BWC1')
    corrupted[block_offset + 13] ^= 1
    reject(corrupted)
    # Legacy BWA1 remains readable into a file, using a fixture independent of encoder.
    legacy = root / 'legacy.bwa'
    legacy.write_bytes(nested(b'legacy data'))
    run('decompress', legacy, destination)
    assert destination.read_bytes() == b'legacy data'
    destination.unlink()
    assert not list(root.glob('.compressor-*'))

print('BWA2 tests passed')
