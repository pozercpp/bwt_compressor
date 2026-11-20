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


def inspect_without_writes(archive, expected_entries):
    # Detect creations/removals and changes to existing files, including staging.
    def state():
        return {str(p): (p.stat().st_size, p.stat().st_mtime_ns)
                for p in archive.parent.rglob('*')}
    before = state()
    listing = run('list', archive).stdout.decode().splitlines()
    actual = {}
    for line in listing:
        kind, size, name = line.split('\t', 2)
        actual[name] = (kind, int(size))
    assert len(listing) == len(actual)
    assert actual == expected_entries, (actual, expected_entries)
    total = sum(size for kind, size in expected_entries.values())
    assert run('test', archive).stdout == f'Verified {total} bytes.\n'.encode()
    assert state() == before, 'list/test changed the filesystem'


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
    (source / 'nested' / '資料' / 'пусто 📁').mkdir(parents=True)
    (source / 'nested' / 'пустой.txt').write_bytes(b'')
    (source / 'nested' / 'binary.bin').write_bytes(bytes(range(256)) * 17)
    (source / 'large.bin').write_bytes(random.Random(42).randbytes(1024 * 1024 + 11))
    extra = root / 'extra.txt'
    extra.write_text('Hello, мир!\n')
    archive = root / 'all.bwa'
    destination = root / 'restored'
    run('compress', archive, source, extra)
    expected_entries = {source.name: ('directory', 0), extra.name: ('file', extra.stat().st_size)}
    for path in source.rglob('*'):
        expected_entries[f'{source.name}/{path.relative_to(source)}'] = (
            'directory' if path.is_dir() else 'file', 0 if path.is_dir() else path.stat().st_size)
    inspect_without_writes(archive, expected_entries)
    valid = archive.read_bytes()
    assert valid[:4] == b'BWA6'
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

    # Several standalone files, including empty and Unicode names.
    unicode_file = root / 'данные 日本語 📄.txt'
    unicode_file.write_bytes('Unicode contents: привет 世界'.encode())
    files_archive = root / 'files.bwa'
    run('compress', files_archive, extra, unicode_file, source / 'nested' / 'пустой.txt')
    run('decompress', files_archive, destination)
    assert snapshot(destination) == {
        extra.name: extra.read_bytes(), unicode_file.name: unicode_file.read_bytes(),
        'пустой.txt': b'',
    }
    shutil.rmtree(destination)

    # An existing destination containing a symlink must remain untouched.
    destination.mkdir()
    (destination / source.name).symlink_to(source, target_is_directory=True)
    original_tree = snapshot(source)
    run('decompress', archive, destination, expected=1)
    assert (destination / source.name).is_symlink()
    assert snapshot(source) == original_tree
    shutil.rmtree(destination)

    # BWA2 crosses the maximum eight-block window and then writes another file.
    large = root / 'a-large.bin'
    block_size = 1024 * 1024
    large.write_bytes(b''.join(bytes([i]) * block_size for i in range(10)) + b'tail')
    large_archive = root / 'large.bwa'
    run('compress', large_archive, large, extra)
    inspect_without_writes(large_archive, {
        large.name: ('file', large.stat().st_size), extra.name: ('file', extra.stat().st_size)})
    run('decompress', large_archive, destination)
    assert snapshot(destination) == {large.name: large.read_bytes(), extra.name: extra.read_bytes()}
    shutil.rmtree(destination)

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
    inspect_without_writes(empty_archive, {'empty': ('directory', 0), 'пустой.txt': ('file', 0)})
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
    def reject(data, list_expected=1):
        malformed.write_bytes(data)
        before = set(root.rglob('*'))
        run('test', malformed, expected=1)
        run('list', malformed, expected=list_expected)
        assert set(root.rglob('*')) == before
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
    reject(container(file_record(b'a') + record(1, b'a'), 2))
    reject(container(file_record(b'a') + file_record(b'a'), 2))
    reject(container(record(1, b'a') + file_record(b'a/../../escape'), 2))
    reject(container(file_record(str(sentinel).encode()), 1))
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
    reject(corrupted, list_expected=0)
    # Payload, mode and BWT index validation belong to test, not listing.
    for offset in [4, 9, 17]:
        bad_content = bytearray(container(file_record(b'file', b'hello'), 1, 5))
        bad_content[bad_content.index(b'BWC1') + offset] ^= 128
        reject(bad_content, list_expected=0)
    reject(container(record(2, b'x', struct.pack('<Q', 0) + b'BAD!' + nested()[4:]), 1))
    reject(container(record(2, b'x', struct.pack('<Q', 1) + b'BWA1' + struct.pack('<I', 0xffffffff)), 1))
    malformed.write_bytes(container(b'', 0))
    inspect_without_writes(malformed, {})
    malformed.write_bytes(container(record(1, b'dir') + file_record(b'dir/file', b'old'), 2, 3))
    inspect_without_writes(malformed, {'dir': ('directory', 0), 'dir/file': ('file', 3)})
    run('decompress', malformed, destination)
    assert (destination / 'dir/file').read_bytes() == b'old'
    shutil.rmtree(destination)
    # Legacy BWA1 remains readable into a file, using a fixture independent of encoder.
    legacy = root / 'legacy.bwa'
    for chunks in [[], [b'legacy data'], [b'first', b'', bytes(range(256)), b'last']]:
        # Strip each fixture's signature/footer to assemble independent blocks.
        data = b''.join(chunks)
        legacy.write_bytes(b'BWA1' + b''.join(nested(chunk)[4:-12] for chunk in chunks)
                           + struct.pack('<IQ', 0, len(data)))
        inspect_without_writes(legacy, {'(unnamed)': ('file', len(data))})
        run('decompress', legacy, destination)
        assert destination.is_file() and destination.read_bytes() == data
        destination.unlink()
    assert not list(root.glob('.compressor-*'))

print('BWA2 tests passed')
