"""Keep HDS clarity, executable outcomes and VP policy evidence separate."""
from os.path import relpath

SPEC_CASES = ('encrypt128', 'decrypt128', 'encrypt256', 'decrypt256', 'upper_key128', 'trace128', 'trace256')
POLICY_CASES = ('registers', 'aes128', 'aes256', 'protocol', 'commands', 'reset', 'timing',
                'result_mux', 'strobes', 'status_flow', 'access_matrix', 'concurrency',
                'reset_edges', 'reset_entry', 'latency_1', 'latency_2', 'latency_17',
                'latency_257', 'core', 'init_zero', 'block_zero', 'bus', 'bus_aes128', 'bus_lanes',
                'busy_matrix', 'reset_boundaries', 'debug_invariants', 'hds_fields')
REFERENCE_CASES = ('core_oracle', 'mmio_oracle_128', 'mmio_oracle_256', 'sequence_oracle')


def expected_tests():
    return {f'fu3_aes_{group}_{case}': scope
            for group, scope, cases in [('spec', 'SPEC', SPEC_CASES),
                                        ('policy', 'POLICY', POLICY_CASES),
                                        ('reference', 'REFERENCE', REFERENCE_CASES)]
            for case in cases}


def case_verdict(case, scope):
    if case is None or case.find('skipped') is not None or case.get('status') not in (None, 'run'):
        return 'TEST_NOT_RUN'
    output = case.findtext('system-out', '') + '\n' + case.findtext('system-err', '')
    markers = {line.split()[0] for line in output.splitlines() if line.strip()}
    failed = case.find('failure') is not None or case.find('error') is not None
    if 'TEST_ERROR' in markers:
        return 'TEST_ERROR'
    if not failed and f'{scope}_PASS' in markers and f'{scope}_FAIL' not in markers:
        return f'{scope}_PASS'
    if failed and f'{scope}_FAIL' in markers and f'{scope}_PASS' not in markers:
        return f'{scope}_FAIL'
    # A crash, timeout, missing/wrong-category marker or contradictory result
    # is infrastructure/test evidence failure, never a fabricated SPEC_FAIL.
    return 'TEST_ERROR'


def requirement_verdict(requirement, verdicts):
    clarity = requirement['clarity']
    if clarity != 'SPEC_CLEAR':
        return clarity
    if requirement.get('blocked_by'):
        return 'SPEC_BLOCKED'
    tests = requirement.get('spec_tests', [])
    if not tests:
        return 'SPEC_NOT_TESTED'
    if any(test not in SPEC_CASES for test in tests):
        raise ValueError('Non-SPEC test cannot provide a SPEC verdict')
    outcomes = [verdicts.get('fu3_aes_spec_' + test, 'TEST_NOT_RUN') for test in tests]
    if 'SPEC_FAIL' in outcomes:
        return 'SPEC_FAIL'
    if all(outcome == 'SPEC_PASS' for outcome in outcomes):
        return 'SPEC_PASS'
    return 'SPEC_NOT_TESTED'


def render_matrix(matrix, report, destination, specification_notes):
    rows = [
        '# HDS verification matrix', '',
        'Generated from `tests/hds-requirements.json` and the three completed build reports.',
        'Requirement clarity and verification status are separate. A clear requirement can remain',
        '`SPEC_BLOCKED` or `SPEC_NOT_TESTED`; a passing policy/reference test cannot promote it.', '',
        '**SPEC_PASS here is restricted to the stated core-vector/round-trace scope. It is not whole-IP HDS sign-off.**', '',
        'Source: `' + matrix['source']['file'] + '`, physical PDF pages; SHA-256:',
        '`' + matrix['source']['sha256'] + '`.', '',
        'The fixed FIPS 197 Appendix C vectors are specification-derived because HDS p.8 section 1.2',
        'explicitly references FIPS 197. They are not vector results supplied inside the HDS itself.',
        'OpenSSL-generated outputs are classified only as REFERENCE evidence.', '',
        '| ID / function | HDS source | Clarity | Verification across Debug/UBSan/Release | Expected / limitation / blocker |',
        '| --- | --- | --- | --- | --- |',
    ]
    for req in matrix['requirements']:
        outcomes = {suite['hds_requirements'][req['id']] for suite in report['suites'].values()}
        verdict = next(iter(outcomes)) if len(outcomes) == 1 else ', '.join(sorted(outcomes))
        detail = '; '.join(str(req[key]) for key in ('expected', 'scope', 'vp_policy') if key in req)
        links = req.get('issues', []) + req.get('blocked_by', [])
        if links:
            detail += '; owner questions: ' + ', '.join(links)
        if req.get('policy_tests'):
            detail += '; policy evidence only: ' + ', '.join(req['policy_tests'])
        rows.append('| ' + ' | '.join([req['id'] + ' — ' + req['function'], req['source'],
                                      req['clarity'], verdict, detail.replace('|', '/')]) + ' |')
    notes_link = relpath(specification_notes, destination.parent).replace('\\', '/')
    rows += ['', 'HDS issues and model conventions: [spec-decisions.md](' + notes_link + ').',
             'Machine evidence: `build/fu3-aes/verification-summary.json`.',
             'TLM error codes, debug/DMI, concurrency/locking, annotated delay and reset-cancellation rules',
             'are model contracts outside HDS; their results remain in the POLICY suite.', '']
    destination.write_text('\n'.join(rows), encoding='utf-8')
