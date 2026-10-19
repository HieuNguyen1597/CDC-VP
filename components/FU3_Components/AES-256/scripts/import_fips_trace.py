"""Import literal FIPS 197 Appendix C.1/C.3 trace values; never runs AES code.

Optional maintenance utility requiring pypdf. Normal builds use the committed
text fixture and need neither pypdf nor network access.
"""
import argparse
import hashlib
from pathlib import Path
import re
from pypdf import PdfReader

SOURCE_SHA256 = '251dfe0b5dc283abaf364adf586f7ec6dc4e495335d48dd5ee0fee6c5961da8a'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pdf', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if hashlib.sha256(args.pdf.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError('FIPS source differs from the reviewed publication')
    source = '\n'.join(page.extract_text() for page in PdfReader(args.pdf).pages)
    rows = ['# Literal FIPS 197 (2001) Appendix C.1/C.3, including the NIST archival cover.',
            '# Source: https://nvlpubs.nist.gov/nistpubs/fips/nist.fips.197.pdf',
            '# PDF SHA256: ' + SOURCE_SHA256,
            '# bits direction round phase expected_state_or_round_key']
    for bits, section in ((128, 'C.1 AES-128'), (256, 'C.3 AES-256')):
        start = source.rindex(section)
        end = source.index('EQUIVALENT INVERSE CIPHER', start)
        text = source[start:end]
        enc, dec = text.split('INVERSE CIPHER (DECRYPT):')
        for direction, text in (('enc', enc), ('dec', dec)):
            values = re.findall(r'round\[\s*(\d+)\]\.(\w+)\s+([0-9a-f]{32})', text)
            rounds = 10 if bits == 128 else 14
            if len(values) != 5 * rounds + 2:
                raise ValueError(f'incomplete trace {bits}/{direction}: {len(values)}')
            if max(int(value[0]) for value in values) != rounds:
                raise ValueError('incorrect final round')
            for round_number, phase, value in values:
                rows.append(f'{bits} {direction} {int(round_number)} {phase} {value}')
    args.output.write_text('\n'.join(rows) + '\n', encoding='ascii')
    print('Imported 248 literal round-state/key records from the pinned NIST PDF')


if __name__ == '__main__':
    main()
