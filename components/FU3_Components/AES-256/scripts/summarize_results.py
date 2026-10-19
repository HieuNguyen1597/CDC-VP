"""Summarize CTest XML, source hashes, independent vectors and AES gcov evidence."""
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from hds_verdicts import expected_tests, case_verdict, requirement_verdict, render_matrix


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    build = Path(sys.argv[1]).resolve()
    component = Path(__file__).resolve().parents[1]
    report = {'suites': {}, 'coverage': {}, 'source_sha256': {}}
    matrix_path = component / 'tests/hds-requirements.json'
    matrix = json.loads(matrix_path.read_text(encoding='utf-8'))
    report['hds_source'] = matrix['source']
    report['hds_clarity_counts'] = dict(Counter(r['clarity'] for r in matrix['requirements']))
    expected = expected_tests()
    has_failures = False
    for name in ('fu3-aes', 'fu3-aes-ubsan', 'fu3-aes-release'):
        folder = build / name
        root = ET.parse(folder / 'results.xml').getroot()
        cases = root.findall('.//testcase')
        by_name = {case.get('name'): case for case in cases}
        names = set(by_name)
        if len(cases) != len(expected) or names != set(expected):
            raise RuntimeError(f'{name}: unexpected test set')
        verdicts = {test: case_verdict(by_name[test], scope) for test, scope in expected.items()}
        failed = [test for test, verdict in verdicts.items() if not verdict.endswith('_PASS')]
        has_failures = has_failures or bool(failed)
        hds = {r['id']: requirement_verdict(r, verdicts) for r in matrix['requirements']}
        cache = {}
        for line in (folder / 'CMakeCache.txt').read_text().splitlines():
            if line.startswith(('CMAKE_BUILD_TYPE:', 'CMAKE_CXX_FLAGS:', 'SYSTEMC_LIBRARY:',
                                'SYSTEMC_INCLUDE_DIR:', 'FU3_AES_COVERAGE:')):
                key, value = line.split('=', 1)
                cache[key.split(':', 1)[0]] = value
        report['suites'][name] = {'passed': len(cases) - len(failed), 'tests': sorted(names), 'cache': cache,
                                 'test_verdicts': verdicts, 'verdict_counts': dict(Counter(verdicts.values())),
                                 'hds_requirements': hds, 'hds_verdict_counts': dict(Counter(hds.values())),
                                 'vectors_sha256': sha256(folder / 'openssl-vectors.txt')}
    for path in (build / 'fu3-aes' / 'coverage').glob('*.gcov.json.gz'):
        with gzip.open(path, 'rt') as file:
            data = json.load(file)
        for item in data['files']:
            path = Path(item['file'])
            if path.parent != component / 'src':
                continue
            lines = item['lines']
            branches = [b for line in lines for b in line.get('branches', [])]
            report['coverage'][path.name] = {
                'lines_hit': sum(line['count'] > 0 for line in lines), 'lines_total': len(lines),
                'uncovered_lines': [line['line_number'] for line in lines if line['count'] == 0],
                'branches_hit': sum(b['count'] > 0 for b in branches), 'branches_total': len(branches),
            }
    if set(report['coverage']) != {'aes_core.cpp', 'aes_controller.cpp'}:
        raise RuntimeError('missing AES source coverage')
    for folder in ('src', 'include', 'tests', 'scripts'):
        for path in sorted((component / folder).rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                report['source_sha256'][str(path.relative_to(component))] = sha256(path)
    report['source_sha256']['CMakeLists.txt'] = sha256(component / 'CMakeLists.txt')
    report['openssl'] = subprocess.check_output(['openssl', 'version'], text=True).strip()
    report['compiler'] = subprocess.check_output(['g++', '--version'], text=True).splitlines()[0]
    output = build / 'fu3-aes' / 'verification-summary.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    render_matrix(matrix, report, output.parent / 'hds-verification-matrix.md',
                  component / 'docs/spec-decisions.md')
    print(json.dumps({'suites': {k: v['verdict_counts'] for k, v in report['suites'].items()},
                      'hds': {k: v['hds_verdict_counts'] for k, v in report['suites'].items()},
                      'coverage': report['coverage'], 'report': str(output)}, indent=2))
    if has_failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
