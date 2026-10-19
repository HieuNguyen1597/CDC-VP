"""Generate deterministic test-only reference vectors using the OpenSSL CLI."""
import argparse
import hashlib
from pathlib import Path
import random
import subprocess


def cases(bits):
    size = bits // 8
    for byte in (0, 255, 0xAA):
        yield bytes([byte]) * size, bytes([byte ^ (0x55 if byte == 0xAA else 0)]) * 16
    for bit in range(bits):
        key = bytearray(size)
        key[bit // 8] = 1 << (bit % 8)
        yield bytes(key), bytes(16)
    for bit in range(128):
        block = bytearray(16)
        block[bit // 8] = 1 << (bit % 8)
        yield bytes(size), bytes(block)
    rng = random.Random(0xAE500000 + bits)
    for _ in range(256):
        yield bytes(rng.randrange(256) for _ in range(size)), bytes(rng.randrange(256) for _ in range(16))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openssl', default='openssl')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    version = subprocess.check_output([args.openssl, 'version'], text=True).strip()
    rows = [f'# {version}', '# bits key input encrypt(input) decrypt(input); seed=0xAE500000+bits']
    for bits in (128, 256):
        for key, block in cases(bits):
            command = [args.openssl, 'enc', f'-aes-{bits}-ecb', '-nopad', '-nosalt', '-K', key.hex()]
            outputs = []
            for direction in ('-e', '-d'):
                result = subprocess.run(command + [direction], input=block, capture_output=True, check=True)
                if len(result.stdout) != 16:
                    raise RuntimeError('OpenSSL did not return exactly one block')
                outputs.append(result.stdout.hex())
            rows.append(f'{bits} {key.hex()} {block.hex()} {outputs[0]} {outputs[1]}')
    data = ('\n'.join(rows) + '\n').encode('ascii')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temp = args.output.with_suffix('.tmp')
    temp.write_bytes(data)
    temp.replace(args.output)
    print(f'Generated {len(rows) - 2} independent vectors; sha256={hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
