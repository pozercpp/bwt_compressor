#!/usr/bin/env python3
"""Rebuild the small, deterministic seed corpus without invoking an encoder."""
import argparse
import hashlib
import json
import pathlib
import struct
import zlib


def block(mode, source, payload=None, primary=0):
    return b'BWC1' + bytes([mode]) + struct.pack('<III', len(source), primary, zlib.crc32(source)) + (source if payload is None else payload)


def stream(encoded=None, size=0):
    return b'BWA1' + (struct.pack('<I', len(encoded)) + encoded if encoded else b'') + struct.pack('<IQ', 0, size)


def name(value):
    return struct.pack('<I', len(value)) + value


def seeds():
    legacy1 = bytes.fromhex('42574331012c01000058000000183dc3b3012aab0200')
    legacy2 = bytes.fromhex('4257433102000100001e0000007fe58530020202194546cda5d90bda30a77e835b29832d659fc96aa10b1e1cec2166d20b280ea14d9a235bd2a61fcde59a5197f6f61cfe448a2af4a763d1322f0c3ef0608406c4476332dd2f9af252cf74155a77463cef2c0d90c221b88325cc68bd6f10387aa9670f9af20f2a883759422a0d2b38c96b7da90e139ffaef7022f27200000000')
    arithmetic = bytes.fromhex('425743310300100000000000004034a6fe050000000142bc64f98000000000')
    # All 256 canonical codes have length 8, so the bitstream equals the RLE.
    # For A*4096: MTF is [65, 0*4095], RLE = [1,65, ULEB128(4095),0].
    huffman = block(4, b'A' * 4096, struct.pack('<I', 5) + b'\x08' * 256 + bytes([1, 65, 255, 31, 0]))
    blocks = {'raw': block(0, b'hello\x00world'), 'empty': block(0, b''),
              'legacy_mode1': legacy1, 'legacy_mode2': legacy2,
              'arithmetic_mode3': arithmetic, 'huffman_mode4': huffman}
    result = {f'bwc1/{key}': (value, True) for key, value in blocks.items()}
    result['bwc1/bad_crc'] = (block(0, b'hello')[:-1] + b'!', False)
    result['bwc1/truncated'] = (b'BWC1\x04', False)
    result['bwa1/empty'] = (stream(), True)
    for key, value in blocks.items():
        if key != 'empty':
            result[f'bwa1/{key}'] = (stream(value, struct.unpack_from('<I', value, 5)[0]), True)
    result['bwa1/truncated_footer'] = (stream()[:-1], False)
    result['bwa1/oversized_block'] = (b'BWA1' + struct.pack('<I', 0xffffffff), False)
    raw = blocks['raw']
    size = 11
    meta = struct.pack('<IqI', 0o755, 1700000000, 0)
    footer = b'\0' + struct.pack('<QQ', 2, size)
    for version in range(2, 7):
        records = b'\x01' + name(b'd') + (meta if version >= 3 else b'')
        records += b'\x02' + name(b'd/f') + (meta if version >= 3 else b'')
        records += struct.pack('<Q', size) + stream(raw, size)
        archive = f'BWA{version}'.encode() + records + footer
        if version == 6:
            archive += struct.pack('<I', zlib.crc32(archive))
        result[f'bwa2/v{version}_file'] = (archive, True)
    for key in ('legacy_mode1', 'legacy_mode2', 'arithmetic_mode3', 'huffman_mode4'):
        encoded = blocks[key]
        length = struct.unpack_from('<I', encoded, 5)[0]
        value = b'BWA2\x02' + name(b'f') + struct.pack('<Q', length) + stream(encoded, length)
        result[f'bwa2/{key}'] = (value + b'\0' + struct.pack('<QQ', 1, length), True)
    result['bwa2/empty'] = (b'BWA2\0' + struct.pack('<QQ', 0, 0), True)
    result['bwa2/traversal'] = (b'BWA2\x01' + name(b'../escape') + b'\0' + struct.pack('<QQ', 1, 0), False)
    result['bwa2/truncated_name'] = (b'BWA2\x01' + struct.pack('<I', 0xffffffff), False)
    # A genuine solid record exercises the newer format branch.
    solid = b'BWA5\x06' + struct.pack('<I', 1) + name(b'f') + meta + struct.pack('<Q', size) + stream(raw, size)
    solid += b'\0' + struct.pack('<QQ', 1, size)
    result['bwa2/v5_solid'] = (solid, True)
    solid6 = b'BWA6' + solid[4:]
    result['bwa2/v6_solid'] = (solid6 + struct.pack('<I', zlib.crc32(solid6)), True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent / 'corpus')
    args = parser.parse_args()
    manifest = {}
    for name_, (data, valid) in seeds().items():
        path = args.output / name_
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        manifest[name_] = {'valid': valid, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
