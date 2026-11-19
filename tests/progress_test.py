"""Statistics, legacy containers, redirection, errors and terminal rendering."""
import errno
import os
import pathlib
import pty
import re
import select
import time
import struct
import subprocess
import sys
import tempfile
import zlib

exe = str(pathlib.Path(sys.argv[1]).resolve())


def run(*args):
    return subprocess.run([exe, *map(str, args)], capture_output=True, timeout=60)


def stats(result, processed, input_size, output_size):
    assert result.returncode == 0, result.stderr
    text = result.stderr.decode()
    assert '\r' not in text and '\x1b' not in text
    assert len(text.splitlines()) == 1, text
    assert text.startswith('Statistics: ')
    assert f'processed {processed} B / {processed} B (100.0%)' in text
    assert f'input {input_size} B; result {output_size} B' in text
    assert re.search(r'time \d+\.\d+ s; average \d+\.\d+ MiB/s', text), text


def terminal(*args, stdout_pipe=False):
    master, slave = pty.openpty()
    process = None
    try:
        process = subprocess.Popen([exe, *map(str, args)], stdout=subprocess.PIPE if stdout_pipe else slave,
                                   stderr=slave)
        os.close(slave)
        slave = -1
        chunks = []
        deadline = time.monotonic() + 60
        while True:
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([master], [], [], remaining)[0], "PTY timeout"
            try:
                chunk = os.read(master, 8192)
                if not chunk:
                    break
                chunks.append(chunk)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
        assert process.wait(timeout=60) == 0
        return b''.join(chunks).decode()
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            if process.stdout is not None:
                process.stdout.close()
        os.close(master)
        if slave >= 0:
            os.close(slave)


with tempfile.TemporaryDirectory(prefix='compressor-progress-cli-') as temp:
    root = pathlib.Path(temp)
    source = root / 'source'
    source.write_bytes(b'abc' * 400000)
    packed = root / 'packed'
    stats(run('compress', packed, source), source.stat().st_size, source.stat().st_size, packed.stat().st_size)
    stats(run('decompress', packed, root / 'out'), source.stat().st_size, packed.stat().st_size, source.stat().st_size)
    assert run('list', packed).stderr == b''
    assert run('test', packed).stderr == b''
    # Independently constructed old formats, with raw BWC1 blocks.
    data = b'old format'
    block = b'BWC1\0' + struct.pack('<III', len(data), 0, zlib.crc32(data)) + data
    stream = b'BWA1' + struct.pack('<I', len(block)) + block + struct.pack('<IQ', 0, len(data))
    for version in (1, 2, 3):
        contents = stream if version == 1 else (
            f'BWA{version}'.encode() + b'\x02' + struct.pack('<I', 1) + b'f'
            + (struct.pack('<IqI', 0o600, 0, 0) if version == 3 else b'')
            + struct.pack('<Q', len(data)) + stream + b'\0' + struct.pack('<QQ', 1, len(data)))
        old = root / f'old-{version}'
        old.write_bytes(contents)
        stats(run('decompress', old, root / f'out-{version}'), len(data), len(contents), len(data))
    broken = root / 'broken'
    broken.write_bytes(packed.read_bytes()[:-1])
    failure = run('decompress', broken, root / 'failed')
    assert failure.returncode == 1 and b'Statistics:' not in failure.stderr
    assert b'\r' not in failure.stderr and not (root / 'failed').exists()
    empty = root / 'empty'
    empty.write_bytes(b'')
    empty_archive = root / 'empty-archive'
    stats(run('compress', empty_archive, empty), 0, 0, empty_archive.stat().st_size)
    stats(run('decompress', empty_archive, root / 'empty-out'), 0, empty_archive.stat().st_size, 0)

    text = terminal('compress', root / 'tty-archive', source)
    assert '\rProgress: ' in text and '(0.0%)' in text and 'Statistics:' in text
    elapsed = float(re.search(r'Statistics: .*?time ([\d.]+) s', text)[1])
    assert text.count('Progress: ') <= 1 + int((elapsed + 0.001) / 0.2)
    text = terminal('decompress', packed, root / 'tty-out')
    updates = [part for part in text.split('\r') if part.startswith('Progress: ')]
    assert updates and all('%' not in part for part in updates)
    # Redirecting stdout also disables the dynamic line, even with terminal stderr.
    text = terminal('compress', root / 'redirected', empty, stdout_pipe=True)
    assert 'Progress:' not in text and 'Statistics:' in text
print('Progress CLI tests passed')
