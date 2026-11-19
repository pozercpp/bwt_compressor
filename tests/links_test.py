"""BWA4 links, special-file policy, virtual resolution and safe staging."""
import os
import pathlib
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

exe = pathlib.Path(sys.argv[1]).resolve()
faults = pathlib.Path(sys.argv[2]).resolve()


def run(*args, expected=0, **kwargs):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, timeout=30, **kwargs)
    assert result.returncode == expected, (args, result.returncode, result.stderr)
    return result


def string(value):
    value = value.encode() if isinstance(value, str) else value
    return struct.pack('<I', len(value)) + value


def meta(mode=0o700):
    return struct.pack('<IqI', mode, 1700000000, 123456789)


def record(kind, name, body):
    return bytes([kind]) + string(name) + body


def directory(name, mode=0o700):
    return record(1, name, meta(mode))


def file(name, data=b'hello', mode=0o600):
    stream = b'BWA1'
    if data:
        block = b'BWC1\0' + struct.pack('<III', len(data), 0, zlib.crc32(data)) + data
        stream += struct.pack('<I', len(block)) + block
    stream += struct.pack('<IQ', 0, len(data))
    return record(2, name, meta(mode) + struct.pack('<Q', len(data)) + stream)


def symlink(name, target):
    return record(3, name, meta()[4:] + string(target))


def hardlink(name, index):
    return record(4, name, struct.pack('<Q', index))


def fifo(name):
    return record(5, name, meta(0o640))


def archive(records, total=0):
    return b'BWA4' + b''.join(records) + b'\0' + struct.pack('<QQ', len(records), total)


with tempfile.TemporaryDirectory(prefix='bwa4-tests-') as tmp:
    root = pathlib.Path(tmp)
    src = root / 'tree'
    src.mkdir()
    (src / 'dir').mkdir()
    (src / 'dir/data').write_bytes(b'hello')
    os.chmod(src / 'dir/data', 0o400)
    os.link(src / 'dir/data', src / 'hard')
    (src / 'a-forward').symlink_to('z-chain')
    (src / 'z-chain').symlink_to('hard')
    (src / 'dir/relative').symlink_to('../hard')
    (src / 'directory-link').symlink_to('dir')
    stamp = 1700000000987654321
    os.utime(src / 'a-forward', ns=(stamp, stamp), follow_symlinks=False)
    stamp = (src / 'a-forward').lstat().st_mtime_ns
    packed = root / 'packed'
    run('compress', packed, src)
    original = packed.read_bytes()
    assert original[:4] == b'BWA4'
    assert original.count(b'BWC1') == 1
    listing = run('list', packed).stdout.decode()
    assert 'hardlink\t5\ttree/hard\ttree/dir/data\n' in listing
    assert 'symlink\t0\ttree/a-forward\tz-chain\n' in listing
    assert run('test', packed).stdout == b'Verified 10 bytes.\n'
    out = root / 'out'
    run('decompress', packed, out, umask=0o777)
    restored = out / 'tree'
    assert (restored / 'hard').stat().st_ino == (restored / 'dir/data').stat().st_ino
    assert stat.S_IMODE((restored / 'hard').stat().st_mode) == 0o400
    assert (restored / 'a-forward').read_bytes() == b'hello'
    assert (restored / 'a-forward').lstat().st_mtime_ns == stamp
    for name in ['a-forward', 'z-chain', 'dir/relative', 'directory-link']:
        assert os.readlink(restored / name) == os.readlink(src / name)
    run('decompress', packed, out, expected=1)
    assert packed.read_bytes() == original

    # Only selected aliases are included. A lone selected inode is a data record.
    lone = root / 'lone'
    run('compress', lone, src / 'hard')
    assert run('list', lone).stdout == b'file\t5\thard\n'
    # A relative symlink can refer to a separate selected input root.
    standalone_link = root / 'standalone'
    standalone_link.symlink_to('hard')
    together = root / 'together'
    run('compress', together, standalone_link, src / 'hard')
    run('test', together)

    # FIFOs are metadata-only and require permission for both write operations.
    pipe = src / 'pipe'
    os.mkfifo(pipe, 0o640)
    run('compress', root / 'denied', src, expected=1)
    assert not (root / 'denied').exists()
    with_fifo = root / 'fifo.bwa'
    run('compress', '--allow-fifo', with_fifo, src)
    run('list', with_fifo)
    run('test', with_fifo)
    run('decompress', with_fifo, root / 'fifo-denied', expected=1)
    assert not (root / 'fifo-denied').exists()
    fifo_out = root / 'fifo-out'
    run('decompress', '--allow-fifo', with_fifo, fifo_out, umask=0o777)
    assert stat.S_ISFIFO((fifo_out / 'tree/pipe').lstat().st_mode)
    assert stat.S_IMODE((fifo_out / 'tree/pipe').stat().st_mode) == stat.S_IMODE(pipe.stat().st_mode)
    assert (fifo_out / 'tree/pipe').stat().st_mtime_ns == pipe.stat().st_mtime_ns
    pipe.unlink()
    for args in [('test', '--allow-fifo', packed), ('list', '--allow-fifo', packed),
                 ('compress', '--allow-fifo', '--allow-fifo', root / 'x', src)]:
        run(*args, expected=2)

    # Sockets and devices are rejected even when special-file permission is set.
    with socket.socket(socket.AF_UNIX) as sock:
        sock.bind(str(src / 'socket'))
        run('compress', '--allow-fifo', root / 'socket.bwa', src, expected=1)
    (src / 'socket').unlink()
    run('compress', '--allow-fifo', root / 'device.bwa', '/dev/null', expected=1)

    sentinel = root / 'outside'
    sentinel.write_bytes(b'unchanged')
    sentinel_before = sentinel.stat()
    bad = root / 'bad'
    def reject(records, total=0):
        bad.write_bytes(archive(records, total))
        for command in ['list', 'test']:
            run(command, bad, expected=1)
        run('decompress', '--allow-fifo', bad, root / 'failed', expected=1)
        assert not (root / 'failed').exists()
        assert not list(root.glob('.compressor-*'))
        assert sentinel.read_bytes() == b'unchanged'
        assert sentinel.stat().st_mode == sentinel_before.st_mode
        assert sentinel.stat().st_mtime_ns == sentinel_before.st_mtime_ns

    for target in ['/tmp/outside', '../outside', 'missing', '', b'\xff', 'a\\b', 'C:/x', 'x\0y', './missing']:
        reject([symlink('link', target)])
    reject([symlink('self', 'self')])
    reject([symlink('a', 'b'), symlink('b', 'a')])
    reject([directory('d'), symlink('d/link', '../../outside')])
    reject([directory('d'), symlink('link', 'd'), file('link/file')], 5)
    reject([directory('d'), symlink('link', 'd'), directory('link/child')])
    reject([symlink('link', 'd'), directory('d'), file('link/file')], 5)
    reject([file('f'), symlink('link', 'f/../f')], 5)
    reject([fifo('pipe'), symlink('link', 'pipe')])
    reject([directory('d'), hardlink('h', 0)])
    reject([hardlink('h', 1), file('f')], 10)
    reject([file('f'), hardlink('h', 0), hardlink('h2', 1)], 15)
    reject([file('f'), symlink('s', 'f'), hardlink('h', 1)], 10)
    reject([file('f'), hardlink('h', 999999999)], 10)
    reject([file('f'), file('f')], 10)
    reject([record(6, 'device', b'')])
    reject([record(7, 'device', b'')])
    reject([record(255, 'unknown', b'')])
    reject([symlink('s', 'x')[:-1]])
    reject([hardlink('h', 0)[:-1]])
    reject([file('f'), hardlink('h', 0)], 5)  # logical total counts aliases

    # '..' must be evaluated after expansion, not simplified lexically.
    # d/l -> ../e/sub; d/l/../file resolves to e/file (not d/file).
    correct = [directory('d'), symlink('d/l', '../e/sub'), directory('e'),
               file('e/file'), directory('e/sub'), symlink('s', 'd/l/../file')]
    good = root / 'semantic'
    good.write_bytes(archive(correct, 5))
    run('test', good)
    run('decompress', good, root / 'semantic-out')
    assert (root / 'semantic-out/s').read_bytes() == b'hello'
    reject([directory('d'), symlink('d/l', '..'), symlink('s', 'd/l/../outside')])
    chain = [file('z')] + [symlink(f's{i}', f's{i+1}' if i < 40 else 'z') for i in range(41)]
    reject(chain, 5)

    # Exactly 40 symlinks are accepted, including the initially opened link.
    boundary = [file('z')] + [symlink(f's{i}', f's{i+1}' if i < 39 else 'z') for i in range(40)]
    good.write_bytes(archive(boundary, 5))
    run('test', good)
    run('decompress', good, root / 'boundary-out')
    assert (root / 'boundary-out/s0').read_bytes() == b'hello'
    # Symlink to the archive root and forward directory targets are valid.
    good.write_bytes(archive([symlink('root-link', '.'), symlink('dir-link', 'd'), directory('d')]))
    run('decompress', good, root / 'root-links')
    assert (root / 'root-links/root-link').resolve() == root / 'root-links'

    # Source links are validated, not traversed or silently skipped.
    for target in [str(sentinel), '../outside', 'absent', 'bad-source-link']:
        link = src / 'bad-source-link'
        link.symlink_to(target)
        run('compress', root / 'bad-source.bwa', src, expected=1)
        link.unlink()
    assert not (root / 'bad-source.bwa').exists()
    # Input parent components are not followed either.
    parent_link = root / 'parent-link'
    parent_link.symlink_to(src, target_is_directory=True)
    run('compress', root / 'parent-link.bwa', parent_link / 'dir/data', expected=1)

    # Fail after materializing symlinks, and after restricting a child directory.
    # The injected failures exercise cleanup beyond the validation stage.
    env = dict(os.environ, LD_PRELOAD=str(faults), COMPRESSOR_TEST_FAULT='1')
    for records, total in [([directory('d'), file('d/f'), symlink('fail-time', 'd')], 5),
                           ([directory('parent', 0o401), directory('parent/child', 0),
                             file('parent/child/f', mode=0), symlink('s', 'parent')], 5)]:
        good.write_bytes(archive(records, total))
        run('decompress', good, root / 'failed', expected=1, env=env)
        assert not (root / 'failed').exists()
        assert not list(root.glob('.compressor-*'))
    # A concurrent destination wins even after all links have been created.
    collision = root / 'collision'
    good.write_bytes(archive([directory('d', 0), symlink('s', 'd')]))
    env = dict(os.environ, LD_PRELOAD=str(faults), COMPRESSOR_TEST_COLLISION=str(collision))
    run('decompress', good, collision, expected=1, env=env)
    assert collision.is_dir() and not list(collision.iterdir())
    assert not list(root.glob('.compressor-*'))
    assert sentinel.read_bytes() == b'unchanged'

print('BWA4 link tests passed')
