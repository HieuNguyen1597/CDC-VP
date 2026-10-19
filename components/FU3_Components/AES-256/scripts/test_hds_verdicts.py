"""Negative reporting checks: policy/reference evidence must never become SPEC_PASS."""
import unittest
import xml.etree.ElementTree as ET
from hds_verdicts import case_verdict, requirement_verdict


class VerdictTests(unittest.TestCase):
    def case(self, text, failure=False, skipped=False):
        case = ET.Element('testcase', status='run')
        ET.SubElement(case, 'system-out').text = text
        if failure:
            ET.SubElement(case, 'failure')
        if skipped:
            ET.SubElement(case, 'skipped')
        return case

    def test_spec_mismatch_and_harness_error_are_distinct(self):
        self.assertEqual(case_verdict(self.case('SPEC_FAIL encrypt128', failure=True), 'SPEC'), 'SPEC_FAIL')
        self.assertEqual(case_verdict(self.case('TEST_ERROR timeout', failure=True), 'SPEC'), 'TEST_ERROR')
        self.assertEqual(case_verdict(self.case('SPEC_PASS encrypt128', failure=True), 'SPEC'), 'TEST_ERROR')

    def test_non_spec_marker_cannot_pass_spec(self):
        for marker in ('POLICY_PASS', 'REFERENCE_PASS', 'PASS'):
            self.assertEqual(case_verdict(self.case(marker + ' encrypt128'), 'SPEC'), 'TEST_ERROR')

    def test_no_passing_skips(self):
        self.assertEqual(case_verdict(self.case('SPEC_PASS encrypt128', skipped=True), 'SPEC'), 'TEST_NOT_RUN')
        self.assertEqual(case_verdict(None, 'SPEC'), 'TEST_NOT_RUN')

    def test_unclear_and_blocked_requirements_stay_open(self):
        evidence = {'fu3_aes_spec_encrypt128': 'SPEC_PASS'}
        for clarity in ('SPEC_UNDEFINED', 'SPEC_CONFLICT'):
            self.assertEqual(requirement_verdict({'clarity': clarity, 'spec_tests': ['encrypt128']}, evidence), clarity)
        self.assertEqual(requirement_verdict({'clarity': 'SPEC_CLEAR', 'spec_tests': ['encrypt128'],
                                             'blocked_by': ['HDS-Q01']}, evidence), 'SPEC_BLOCKED')

    def test_policy_only_and_missing_evidence_cannot_promote_requirement(self):
        evidence = {'fu3_aes_policy_aes128': 'POLICY_PASS'}
        self.assertEqual(requirement_verdict({'clarity': 'SPEC_CLEAR', 'policy_tests': ['aes128']}, evidence), 'SPEC_NOT_TESTED')
        self.assertEqual(requirement_verdict({'clarity': 'SPEC_CLEAR', 'spec_tests': ['encrypt128']}, evidence), 'SPEC_NOT_TESTED')
        with self.assertRaises(ValueError):
            requirement_verdict({'clarity': 'SPEC_CLEAR', 'spec_tests': ['aes128']}, evidence)


if __name__ == '__main__':
    unittest.main()
