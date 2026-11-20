"""BWA3 metadata, restrictive permissions, timestamp precision and validation."""
import os
import pathlib
import stat
import struct
import subprocess
import sys
import tempfile

exe = pathlib.Path(sys.argv[1]).resolve()


def run(*args, expected=0, umask=None):
    kwargs = {} if umask is None else {'umask': umask}
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, timeout=60, **kwargs)
    assert result.returncode == expected, (args, result.stderr)
    return result


def metadata(mode, seconds=1700000000, nanos=123456789):
    return struct.pack('<IqI', mode, seconds, nanos)


def record(kind, name, meta, body=b''):
    name = name.encode()
    return bytes([kind]) + struct.pack('<I', len(name)) + name + meta + body


def archive(records, count):
    return b'BWA3' + records + b'\0' + struct.pack('<QQ', count, 0)


with tempfile.TemporaryDirectory(prefix='bwa3-tests-') as temporary:
    root = pathlib.Path(temporary)
    source = root / 'source'
    source.mkdir()
    child = source / 'child'
    child.mkdir()
    empty = child / 'empty'
    empty.mkdir()
    data = child / 'file'
    data.write_bytes(b'metadata contents')
    blank = source / 'blank'
    blank.touch()
    paths = [source, child, empty, data, blank]
    modes = [0o500, 0o550, 0o500, 0o400, 0o640]
    # Compare against stat after setting: the source filesystem determines the
    # actual precision and supported negative Unix timestamps.
    stamps = [1700000000123456789, 1700000001987654321, 1700000002000000001,
              -123456789, 1700000003999999999]
    expected = {}
    try:
        for path, mode, stamp in zip(paths, modes, stamps):
            os.chmod(path, mode)
            os.utime(path, ns=(stamp, stamp))
            info = path.stat()
            expected[path.relative_to(source)] = (stat.S_IMODE(info.st_mode), info.st_mtime_ns)
        packed = root / 'metadata.bwa'
        run('compress', packed, source)
        encoded = packed.read_bytes()
        assert encoded[:5] == b'BWA6\x01'
        name_size = struct.unpack_from('<I', encoded, 5)[0]
        mode, seconds, nanos = struct.unpack_from('<IqI', encoded, 9 + name_size)
        assert (mode, seconds * 1000000000 + nanos) == expected[pathlib.Path('.')]
        assert 0 <= nanos < 1000000000
        before = packed.stat().st_mtime_ns
        run('list', packed)
        run('test', packed)
        assert packed.stat().st_mtime_ns == before
        output = root / 'output'
        run('decompress', packed, output, umask=0o777)
        try:
            for relative, (mode, stamp) in expected.items():
                restored = output / source.name / relative
                info = restored.stat()
                assert stat.S_IMODE(info.st_mode) == mode
                assert info.st_mtime_ns == stamp, (relative, info.st_mtime_ns, stamp)
            assert (output / 'source/child/file').read_bytes() == b'metadata contents'
        finally:
            for relative in [pathlib.Path('.'), pathlib.Path('child'), pathlib.Path('child/empty')]:
                os.chmod(output / 'source' / relative, 0o700)
    finally:
        for path in [source, child, empty]:
            os.chmod(path, 0o700)

    # Special mode bits are stripped at creation, including executable files.
    os.chmod(data, 0o6751)
    os.chmod(empty, 0o3700)
    special = root / 'special.bwa'
    run('compress', special, source)
    run('decompress', special, root / 'special-out')
    assert stat.S_IMODE((root / 'special-out/source/child/file').stat().st_mode) == 0o751
    assert stat.S_IMODE((root / 'special-out/source/child/empty').stat().st_mode) == 0o700

    # Handwritten archive can request mode 000, including on a parent directory.
    empty_stream = struct.pack('<Q', 0) + b'BWA1' + struct.pack('<IQ', 0, 0)
    restricted = root / 'restricted.bwa'
    records = (record(1, 'locked', metadata(0)) +
               record(1, 'locked/empty', metadata(0)) +
               record(2, 'locked/file', metadata(0), empty_stream))
    restricted.write_bytes(archive(records, 3))
    run('test', restricted)
    target = root / 'restricted-out'
    run('decompress', restricted, target)
    assert stat.S_IMODE((target / 'locked').stat().st_mode) == 0
    os.chmod(target / 'locked', 0o700)
    assert stat.S_IMODE((target / 'locked/empty').stat().st_mode) == 0
    assert stat.S_IMODE((target / 'locked/file').stat().st_mode) == 0
    os.chmod(target / 'locked/empty', 0o700)

    bad = root / 'bad.bwa'
    for meta in [metadata(0o4755), metadata(0o2755), metadata(0o1755),
                 metadata(0xFFFFFFFF), metadata(0o755, nanos=1000000000),
                 metadata(0o755)[:-1]]:
        bad.write_bytes(archive(record(1, 'bad', meta), 1))
        for command in ['list', 'test']:
            run(command, bad, expected=1)
        run('decompress', bad, root / 'failed', expected=1)
        assert not (root / 'failed').exists()
        assert not list(root.glob('.compressor-*'))
    # Late failure after creating nested entries must leave no staging output.
    bad.write_bytes(archive(records, 99))
    run('decompress', bad, root / 'failed', expected=1)
    assert not (root / 'failed').exists()
    assert not list(root.glob('.compressor-*'))
    bad.write_bytes(b'BWA6' + archive(b'', 0)[4:])
    run('test', bad, expected=1)

print('BWA3 metadata tests passed')
