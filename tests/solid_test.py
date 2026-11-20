"""Solid boundary, empty-file, order and malformed table checks."""
import pathlib
import struct
import subprocess
import sys
import tempfile

exe = sys.argv[1]
def run(*args, ok=True):
    result = subprocess.run([exe, *map(str, args)], capture_output=True)
    assert (result.returncode == 0) == ok, result.stderr
    return result.stdout

with tempfile.TemporaryDirectory() as temp:
    root = pathlib.Path(temp)
    source = root / 'source'
    source.mkdir()
    expected = {}
    for i in range(4100):
        data = b'' if i < 4097 else bytes([i % 256]) * 600000
        name = f'f{i:05}'
        (source / name).write_bytes(data)
        expected[name] = data
    packed = root / 'archive'
    run('compress', packed, source)
    run('test', packed)
    listing = run('list', packed).decode().splitlines()
    assert [line.split('\t')[2].split('/')[-1] for line in listing[1:]] == list(expected)
    out = root / 'out'
    run('decompress', packed, out)
    assert {p.name: p.read_bytes() for p in (out / 'source').iterdir()} == expected
    # BWA5 solid streams retain their original checksum-free framing.
    legacy = root / 'legacy'
    legacy.write_bytes(b'BWA5' + packed.read_bytes()[4:-4])
    run('test', legacy)
    run('list', legacy)
    run('decompress', legacy, root / 'legacy-out')
    assert {p.name: p.read_bytes() for p in (root / 'legacy-out' / 'source').iterdir()} == expected
    data = bytearray(packed.read_bytes())
    group = 4 + 1 + 4 + len('source') + 16
    assert data[group] == 6
    for count in [0, 4097]:
        damaged = data[:]
        struct.pack_into('<I', damaged, group + 1, count)
        bad = root / 'bad'
        bad.write_bytes(damaged)
        for command in ['list', 'test']:
            run(command, bad, ok=False)
        run('decompress', bad, root / 'failed', ok=False)
        assert not (root / 'failed').exists()

    damaged = data[:]
    # First member: path length/path, 16-byte metadata, then size.
    path_length = struct.unpack_from('<I', data, group + 5)[0]
    struct.pack_into('<Q', damaged, group + 9 + path_length + 16, 1048577)
    bad.write_bytes(damaged)
    run('list', bad, ok=False)
    run('test', bad, ok=False)
    run('decompress', bad, root / 'failed', ok=False)
    assert not (root / 'failed').exists()
