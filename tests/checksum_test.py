"""Independent zlib oracle, metadata/order coverage and atomic rejection of BWA6."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

exe = sys.argv[1]

def run(*args, ok=True):
    result = subprocess.run([exe, *map(str, args)], capture_output=True, timeout=60)
    assert (result.returncode == 0) == ok, (args, result.stderr)
    return result

def seal(data):
    return data + struct.pack('<I', zlib.crc32(data))

def name(value):
    return struct.pack('<I', len(value)) + value

def record(path, data):
    block = b'BWC1\0' + struct.pack('<III', len(data), 0, zlib.crc32(data)) + data
    stream = b'BWA1' + struct.pack('<I', len(block)) + block + struct.pack('<IQ', 0, len(data))
    return b'\2' + name(path) + struct.pack('<IqIQ', 0o600, 0, 0, len(data)) + stream

with tempfile.TemporaryDirectory() as temp:
    root = pathlib.Path(temp)
    packed = root / 'archive'
    first, second = record(b'a', b'hello'), record(b'b', b'world')
    footer = b'\0' + struct.pack('<QQ', 2, 10)
    body = b'BWA6' + first + second + footer
    valid = seal(body)
    packed.write_bytes(valid)
    for command in ['list', 'test']:
        run(command, packed)
    run('decompress', packed, root / 'out')
    assert (root / 'out' / 'a').read_bytes() == b'hello'

    # All replacements are structurally valid and have valid per-block CRCs.
    mutations = [
        body.replace(b'\1\0\0\0a', b'\1\0\0\0c', 1),
        b'BWA6' + second + first + footer,
        b'BWA6' + record(b'a', b'HELLO') + second + footer,
        b'BWA6' + record(b'a', b'hell') + record(b'b', b'world!') + footer,
        body[:10] + bytes([body[10] ^ 1]) + body[11:],  # permissions
        # Different types, same entry count and logical size.
        b'BWA6\1' + name(b'a') + struct.pack('<IqI', 0o700, 0, 0)
        + record(b'b', b'helloworld') + footer,
    ]
    for data in mutations:
        packed.write_bytes(seal(data))
        run('test', packed)  # Semantic validators alone must accept these changes.
        packed.write_bytes(data + valid[-4:])
        assert b'Archive checksum mismatch' in run('test', packed, ok=False).stderr
    damaged = [data + valid[-4:] for data in mutations]
    damaged += [valid[:-n] for n in range(1, 6)]
    damaged += [valid + b'extra', valid[:-1] + bytes([valid[-1] ^ 1])]
    for data in damaged:
        packed.write_bytes(data)
        for command in ['list', 'test']:
            run(command, packed, ok=False)
        run('decompress', packed, root / 'failed', ok=False)
        assert not (root / 'failed').exists()
        assert not list(root.glob('.compressor-*'))

    # Without a key, recomputing CRC accepts an intentional alteration.
    packed.write_bytes(seal(mutations[0]))
    run('test', packed)
    # Explicit BWA5 fixture: unchanged semantics, no checksum trailer.
    packed.write_bytes(b'BWA5' + body[4:])
    run('list', packed)
    run('test', packed)
    run('decompress', packed, root / 'legacy')

    # Validate writer against an independent implementation, across many blocks.
    source = root / 'source'
    source.write_bytes(bytes(range(256)) * 13000)
    packed.unlink()
    run('compress', packed, source)
    data = packed.read_bytes()
    assert data[:4] == b'BWA6'
    assert data == seal(data[:-4])
    run('test', packed)
    # Empty container is also protected.
    packed.write_bytes(seal(b'BWA6\0' + struct.pack('<QQ', 0, 0)))
    run('test', packed)
    run('decompress', packed, root / 'empty')
