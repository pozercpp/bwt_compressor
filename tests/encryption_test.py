"""Independent libsodium framing oracle, authentication, CLI and terminal tests."""
import ctypes as C
import ctypes.util
import fcntl
import os
import pathlib
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

exe = str(pathlib.Path(sys.argv[1]).resolve())
sodium = C.CDLL(ctypes.util.find_library('sodium'))
assert sodium.sodium_init() >= 0
sodium.crypto_pwhash.argtypes = [C.c_void_p, C.c_ulonglong, C.c_void_p, C.c_ulonglong,
                               C.c_void_p, C.c_ulonglong, C.c_size_t, C.c_int]
sodium.crypto_secretstream_xchacha20poly1305_init_push.argtypes = [C.c_void_p] * 3
sodium.crypto_secretstream_xchacha20poly1305_init_pull.argtypes = [C.c_void_p] * 3
sodium.crypto_secretstream_xchacha20poly1305_push.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p,
    C.c_void_p, C.c_ulonglong, C.c_void_p, C.c_ulonglong, C.c_ubyte]
sodium.crypto_secretstream_xchacha20poly1305_pull.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p,
    C.c_void_p, C.c_void_p, C.c_ulonglong, C.c_void_p, C.c_ulonglong]
password = b'correct horse test password'


def key(header):
    result = C.create_string_buffer(32)
    ops, memory = struct.unpack_from('<QQ', header, 8)
    assert sodium.crypto_pwhash(result, 32, password, len(password), header[24:40], ops, memory, 2) == 0
    return result


def frames(data):
    parts, offset = [], 64
    while offset < len(data):
        size, = struct.unpack_from('<I', data, offset)
        parts.append(data[offset:offset + 4 + size])
        offset += 4 + size
    assert offset == len(data)
    return parts


def encrypt(messages):
    header = b'BWE1\1\1\0\0' + struct.pack('<QQ', 3, 64 * 1024 * 1024) + os.urandom(16)
    state = C.create_string_buffer(sodium.crypto_secretstream_xchacha20poly1305_statebytes())
    stream_header = C.create_string_buffer(24)
    assert sodium.crypto_secretstream_xchacha20poly1305_init_push(state, stream_header, key(header)) == 0
    header += stream_header.raw
    output = header
    for index, (message, tag) in enumerate(messages):
        size = len(message) + 17
        ad = header + struct.pack('<QI', index, size)
        cipher = C.create_string_buffer(size)
        assert sodium.crypto_secretstream_xchacha20poly1305_push(state, cipher, None, message,
            len(message), ad, len(ad), tag) == 0
        output += struct.pack('<I', size) + cipher.raw
    return output


def decrypt(data):
    header = data[:64]
    state = C.create_string_buffer(sodium.crypto_secretstream_xchacha20poly1305_statebytes())
    assert sodium.crypto_secretstream_xchacha20poly1305_init_pull(state, header[40:], key(header)) == 0
    messages = []
    for index, part in enumerate(frames(data)):
        ad = header + struct.pack('<Q', index) + part[:4]
        plain = C.create_string_buffer(len(part) - 4)
        count = C.c_ulonglong()
        tag = C.c_ubyte()
        assert sodium.crypto_secretstream_xchacha20poly1305_pull(state, plain, C.byref(count), C.byref(tag),
            part[4:], len(part) - 4, ad, len(ad)) == 0
        messages.append((plain.raw[:count.value], tag.value))
    assert messages[0] == (b'', 0) and messages[-1] == (b'', 3)
    assert all(tag == 0 and message for message, tag in messages[1:-1])
    return b''.join(message for message, tag in messages)


def run(*args, secret=password, ok=True, provide=True):
    command = [exe, *map(str, args)]
    if provide:
        command += ['--password-fd', '0']
    result = subprocess.run(command, input=secret + b'\n', capture_output=True,
                            timeout=60, start_new_session=True)
    assert (result.returncode == 0) == ok, (args, result.stderr)
    if not ok:
        assert b'Verified ' not in result.stdout and b'Statistics:' not in result.stderr
    return result


def terminal(args, answers, cancel=False):
    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    def setup():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    proc = subprocess.Popen([exe, *map(str, args)], stdin=slave, stdout=slave, stderr=slave,
                            preexec_fn=setup)
    transcript = b''
    try:
        for index, answer in enumerate(answers):
            prompt = b'Password: ' if index == 0 else b'Confirm password: '
            deadline = time.monotonic() + 30
            while prompt not in transcript:
                remaining = deadline - time.monotonic()
                assert remaining > 0 and select.select([master], [], [], remaining)[0], transcript
                transcript += os.read(master, 8192)
            assert not termios.tcgetattr(slave)[3] & termios.ECHO
            if cancel:
                proc.send_signal(signal.SIGINT)
                break
            os.write(master, answer + b'\n')
        proc.wait(timeout=60)
        while select.select([master], [], [], 0)[0]:
            transcript += os.read(master, 8192)
        assert termios.tcgetattr(slave) == original, 'Terminal settings not restored'
        for answer in answers:
            assert answer not in transcript, transcript
        return proc.returncode
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)
        os.close(slave)


with tempfile.TemporaryDirectory(prefix='compressor-encryption-') as temp:
    root = pathlib.Path(temp)
    source = root / 'source'
    source.mkdir()
    (source / 'big').write_bytes(os.urandom(2 * 1024 * 1024 + 100))
    (source / 'empty').touch()
    (source / 'small').write_bytes(b'hello')
    os.link(source / 'small', source / 'hard')
    (source / 'sym').symlink_to('small')
    packed = root / 'packed'
    run('compress', '--encrypt', packed, source)
    encrypted = packed.read_bytes()
    assert encrypted[:8] == b'BWE1\1\1\0\0'
    assert struct.unpack_from('<QQ', encrypted, 8) == (3, 256 * 1024 * 1024)
    assert b'source/big' not in encrypted
    plain = decrypt(encrypted)
    assert plain[:4] == b'BWA6'
    plain_path = root / 'plain'
    plain_path.write_bytes(plain)
    run('test', plain_path, provide=False)
    run('test', packed)
    listing = run('list', packed)
    assert b'source/big' in listing.stdout
    run('decompress', packed, root / 'out')
    restored = root / 'out' / 'source'
    assert (restored / 'big').read_bytes() == (source / 'big').read_bytes()
    assert (restored / 'small').stat().st_ino == (restored / 'hard').stat().st_ino
    assert os.readlink(restored / 'sym') == 'small'
    second = root / 'second'
    run('compress', '--encrypt', second, source)
    assert second.read_bytes() != encrypted
    for command in ['test', 'list', 'decompress']:
        args = [command, packed] + ([root / 'failed'] if command == 'decompress' else [])
        assert b'Wrong password or damaged archive' in run(*args, secret=b'wrong', ok=False).stderr
        assert not (root / 'failed').exists()
        assert not list(root.glob('.compressor-*'))
    # Initial frame also authenticates the entire header; invalid KDF bounds
    # must be rejected even when no password provider is available.
    bad = root / 'bad'
    for offset, value in [(8, 11), (16, 2**63)]:
        data = bytearray(encrypted)
        struct.pack_into('<Q', data, offset, value)
        bad.write_bytes(data)
        assert b'KDF parameters' in run('test', bad, provide=False, ok=False).stderr
    # Independent, cheaper KDF fixture, many authenticated data frames.
    valid = encrypt([(b'', 0)] + [(plain[i:i+1048576], 0) for i in range(0, len(plain), 1048576)] + [(b'', 3)])
    parts = frames(valid)
    altered = []
    for offset in [24, 40, 64 + 4, 64 + len(parts[0]) + 10, len(valid) - 1]:
        data = bytearray(valid)
        data[offset] ^= 1
        altered.append(bytes(data))
    altered += [valid[:-1], valid[:30], valid + b'extra', valid[:-len(parts[-1])]]
    altered += [valid[:64] + b''.join(parts[:1] + parts[2:]),
                valid[:64] + b''.join(parts[:2] + parts[1:]),
                valid[:64] + b''.join(parts[:1] + [parts[2], parts[1]] + parts[3:])]
    data = bytearray(valid)
    struct.pack_into('<I', data, 64, 2**32 - 1)
    altered.append(bytes(data))
    # Valid authentication but forbidden protocol structure/inner format.
    altered += [encrypt([(b'not empty', 0), (b'', 3)])]
    altered += [encrypt([(b'', 0)] + [(plain[i:i+1048576], 0) for i in range(0, len(plain), 1048576)] + [(b'x', 3)])]
    altered += [encrypt([(b'', 0), (b'BWA5', 0), (b'', 3)]),
                encrypt([(b'', 0), (b'', 0), (b'', 3)]),
                encrypt([(b'', 0), (b'BWA6', 2), (b'', 3)])]
    for contents in altered:
        bad.write_bytes(contents)
        for command in ['test', 'list', 'decompress']:
            args = [command, bad] + ([root / 'failed'] if command == 'decompress' else [])
            run(*args, ok=False)
        assert not (root / 'failed').exists()
        assert not list(root.glob('.compressor-*'))
    run('test', packed, provide=False, ok=False)
    run('test', packed, secret=b'', ok=False)
    run('test', packed, secret=b'x' * 1025, ok=False)
    run('test', packed, secret=password + b'\r')  # CRLF
    # Clear archives never ask for a password.
    run('list', plain_path, provide=False)
    for arguments in [('test', '--password-fd', '-1', packed),
                      ('test', '--encrypt', packed),
                      ('compress', '--password-fd', '0', root / 'unused', source)]:
        result = run(*arguments, provide=False, ok=False)
        assert result.returncode == 2
    # A real inherited descriptor (not stdin), preserving whitespace.
    read_fd, write_fd = os.pipe()
    try:
        os.write(write_fd, password + b'\n')
        os.close(write_fd)
        result = subprocess.run([exe, 'test', str(packed), '--password-fd', str(read_fd)],
                                pass_fds=(read_fd,), capture_output=True, timeout=60)
        assert result.returncode == 0, result.stderr
    finally:
        os.close(read_fd)
    empty_archive = root / 'empty-archive'
    run('compress', '--encrypt', empty_archive, source / 'empty', secret=b'x' * 1024)
    run('decompress', empty_archive, root / 'empty-out', secret=b'x' * 1024)
    assert (root / 'empty-out' / 'empty').read_bytes() == b''
    small = source / 'small'
    tty_archive = root / 'tty'
    assert terminal(['compress', '--encrypt', tty_archive, small], [password, password]) == 0
    assert terminal(['test', tty_archive], [password]) == 0
    assert terminal(['compress', '--encrypt', root / 'mismatch', small], [password, b'different password']) == 1
    assert terminal(['compress', '--encrypt', root / 'cancel', small], [password], cancel=True) == 1
    assert not (root / 'cancel').exists() and not (root / 'mismatch').exists()
    assert not list(root.glob('.compressor-*'))
