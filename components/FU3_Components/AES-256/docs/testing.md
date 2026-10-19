# Testing

Build commands are in the [README](../README.md#build-and-run).
The default suite contains 35 CTest cases; `FU3_AES_ORACLE_TESTS=ON` enables
four additional independent-reference tests for a total of **39**. Tests
print check counts; SystemC scenarios also print simulation time. Checks
remain enabled in Release. The MMIO executable has a 20-ms simulation
watchdog. CTest timeouts are 30 host seconds (60 for independent-reference tests).

Run the complete build/test matrix from the repository root in Linux/WSL:

```sh
bash components/FU3_Components/AES-256/scripts/run_full.sh
```

The script builds Debug with coverage, UBSan, and Release, all with
`-Wall -Wextra -Wpedantic -Werror`, runs all 39 tests in each configuration,
and writes JUnit XML and `build/fu3-aes/verification-summary.json`. It fails
if a build/test fails, a test is skipped, or an expected test is missing.
Prerequisites: SystemC 2.3.4/C++17, GCC/gcov, Python 3, OpenSSL CLI and
CMake/CTest >=3.21. Override the prefix/jobs with `SYSTEMC_HOME` and `JOBS`.
Results and the reset defect found during this audit are documented in the
[verification report](verification-report.md).

## Verdict rules and independent suites

The full suite has **7 SPEC + 28 POLICY + 4 REFERENCE tests**. Names start
with `fu3_aes_spec_`, `fu3_aes_policy_`, or `fu3_aes_reference_` respectively.
Only SPEC executables can produce `SPEC_PASS/FAIL`, and only when comparing
with literal expected values from a clear specification requirement.

`tests/test_hds_core.cpp` checks `encrypt128`, `decrypt128`, `encrypt256`,
`decrypt256` against FIPS 197 Appendix C.1/C.3 and `upper_key128` against the
same fixed C.1 result under changes to the unused key half. HDS p.8 section
1.2 explicitly references FIPS 197; the HDS itself supplies no known-answer
vector. These tests exercise only the core API, not CSR integration or RTL.

`tests/test_fips_trace.cpp` adds `trace128` and `trace256`. It compares the
actual transform's observed phase order, intermediate values, and round keys
with all 248 literal records for standard encryption/inverse cipher in
Appendix C.1/C.3. This checks 10/14 rounds, final-round omission of MixColumns,
SubBytes/ShiftRows/MixColumns/AddRoundKey and inverse results, plus all 11/15
round keys in forward/reverse order for the cited keys. Each test also checks
that an ordinary transform without an observer returns the identical output.

Fixture provenance: NIST archival PDF SHA-256
`251dfe0b5dc283abaf364adf586f7ec6dc4e495335d48dd5ee0fee6c5961da8a`.
`scripts/import_fips_trace.py` requires pypdf only for optional fixture
regeneration from that pinned PDF. It extracts literal source records and
never invokes the AES model or OpenSSL. The committed fixture is used in
ordinary builds. No claim is made about a physical RTL S-box mux, Rcon register
or key-memory implementation.

Existing scenarios mix explicit HDS fields with unresolved reset/address/result
and lifecycle choices. They remain POLICY tests, so none of their assertions
can silently contribute to SPEC_PASS. OpenSSL-generated expected values are
REFERENCE evidence only, including when exercised through MMIO.

`hds-requirements.json` maps each requirement to a source, expected behavior,
clarity class, test scope and/or owner question. `SPEC_UNDEFINED` and
`SPEC_CONFLICT` cannot be promoted by a passing test. Clear requirements
with open integration dependencies are `SPEC_BLOCKED`; requirements lacking
an isolated SPEC test are `SPEC_NOT_TESTED`. Crashes/timeout/setup exceptions
are `TEST_ERROR`/`TEST_NOT_RUN`, not fabricated SPEC mismatch verdicts.

```sh
ctest --test-dir build/fu3-aes -L '^spec$' --output-on-failure
ctest --test-dir build/fu3-aes -L '^vp_policy$' --output-on-failure
ctest --test-dir build/fu3-aes -L '^reference$' --output-on-failure
python3 components/FU3_Components/AES-256/scripts/test_hds_verdicts.py
```

The five Python reporting checks exercise wrong-category markers, crashes,
skips, undefined/conflicting requirements and attempts to use policy evidence
for SPEC verdicts. The full script runs them before the 39 SystemC CTest cases.

## Policy and reference regression inventory

All suffixes below use `fu3_aes_policy_` except `core_oracle`,
`mmio_oracle_128/256` and `sequence_oracle`, which use `fu3_aes_reference_`.

| Test suffix | Checked behavior | Source / policy |
| --- | --- | --- |
| registers | Identity/reset, complete reserved/RO slot sweep, WO readback, CONFIG masks/CTRL mirror | PDF pp.14-17; reset defaults are VP policy |
| aes128 | Known-answer encryption/decryption, repeat block, direction switch without INIT | PDF pp.8,11-12,17; FIPS 197 C.1 |
| aes256 | Known-answer encryption/decryption, repeat block, full result ordering | PDF pp.8,11-12,17; FIPS 197 C.3 |
| protocol | Null/command/address/length/streaming/byte-enable errors, debug access, strobed WO key merge, disabled lanes, incoming delay | PDF pp.10,17-18; TLM contract |
| commands | NEXT without INIT, command pulse, INIT priority, busy protection, key invalidation, key-size change | PDF pp.12,16-17 plus documented VP policies |
| reset | Reset during key expansion and block operation, clear/stale-result prevention, delayed-write cancellation across reset release, recovery | PDF p.11 and corrected reset profile |
| timing | Exact configured cycle counts, no early result, stopped core clock | Configurable VP timing, not an RTL cycle claim |
| result_mux | RESULT1/2 unavailable; RESULT0/3 low-word readback | PDF pp.15-16/AES-R02; RESULT0 truncation is an inference |
| strobes | All 16 byte-enable masks across all KEY/BLOCK words; CONFIG/CTRL low-lane semantics; repeating lengths 1/2/3; partial read preservation | Byte-strobe VP contract |
| status_flow | Previous result retained until atomic completion, VALID lifetime, RO/reserved/disabled writes while busy, re-INIT, hidden key/input reset | Register/state VP contract |
| access_matrix | Both read/write malformed lengths, widths, addresses, byte enables, null pointers; debug errors; no mutation; DMI denied | TLM contract |
| concurrency | Three callers on one target; serialized completion; clock-wait and mutex-wait cancellation; no stale queued key writes after reset | Target concurrency/reset contract, not fabric arbitration |
| reset_edges | Unsampled reset while clock stopped, reset-active read rejection, idle clear, delayed-call cancellation | Synchronous reset VP contract |
| reset_entry | Request enters during a 1-ns reset pulse between edges and delays 40 ns: rejected even after release | Regression for the audit defect |
| busy_matrix | Every KEY/BLOCK register, all CONFIG values and INIT/NEXT/both while INIT/NEXT are busy; disabled-lane no-ops; rejected writes cannot corrupt later output | VP busy-write policy |
| reset_boundaries | Both key sizes, INIT/NEXT, reset 1 ps before/after completion; no delayed reassertion, stale result or retained schedule; recovery | VP reset profile, not a resolution of AES-R01 |
| debug_invariants | Reject writes to every aligned debug slot; hidden WO key/input/schedule preserved; debug read sweeps/polling consume no time or command pulses | VP debug contract |
| hds_fields | Literal HDS field/index checks under the documented x4 address policy; all four CONFIG modes, fresh INIT directly in decrypt mode, WO/RO/reserved/pulse/mirror and word order | Conditional HDS behavior; remains POLICY because integration choices are unresolved |
| latency_1 / latency_2 / latency_17 / latency_257 | INIT/NEXT cycles 1/1, 2/3, 17/65, 257/513; clock periods 2, 7, 37, 10 ns; check just before/after completion | Configurable timing boundaries |
| core | Both directions rejected before initialization and after clearing either key size | Standalone core lifecycle |
| init_zero / block_zero | Constructor rejects zero cycle counts | Configuration validation |
| core_oracle | 1,158 independent key/input records, both directions, AES-128 lower key half ignored | Optional OpenSSL reference corpus |
| mmio_oracle_128 / mmio_oracle_256 | Same corpus through complete register/INIT/NEXT/poll/result flow, both directions | Optional OpenSSL reference corpus |
| sequence_oracle | 512 block transforms under reused zero-key schedules, all 128 walking-input bits, both key sizes/directions; 128 mixed rekey/mode/INIT flows | 640 additional independent OpenSSL comparisons |
| bus | CVA6 port -> real FU3 fabric -> AES at 0x40000000; INCR four-word block write, AES-256 encryption/decryption | Existing FU3 bus map and adapter contract; no CPU execution |
| bus_aes128 | Same real fabric flow with AES-128 | Existing FU3 bus contract |
| bus_lanes | Byte/halfword key writes, word result bursts, lane ordering, unaligned/out-of-aperture/cross-window rejection | Existing fabric width adapter and AES CSR contract |

```sh
ctest --test-dir build/fu3-aes --output-on-failure
ctest --test-dir build/fu3-aes -R '^fu3_aes_policy_aes256$' -V
ctest --test-dir build/fu3-aes -L integration --output-on-failure
```

Optional undefined-behavior sanitizer build (Linux/WSL):

```sh
cmake -S components/FU3_Components/AES-256 -B build/fu3-aes-ubsan \
  -DSYSTEMC_HOME=/opt/systemc-2.3.4 -DCMAKE_BUILD_TYPE=Debug \
  -DFU3_AES_BUILD_TESTS=ON -DFU3_AES_ORACLE_TESTS=ON \
  '-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wpedantic -Werror'
cmake --build build/fu3-aes-ubsan -j4
ctest --test-dir build/fu3-aes-ubsan --output-on-failure
```

## Independent reference data

`scripts/generate_vectors.py` invokes the installed OpenSSL executable in
ECB single-block mode without padding. For each key size the corpus contains
three fixed patterns, every individual key bit, every individual input bit,
and 256 pseudorandom records (seed `0xAE500000 + key_bits`). That produces
515 AES-128 records and 643 AES-256 records. Each record stores independent
`encrypt(input)` and `decrypt(input)` outputs. Decryption tests do not depend
on ciphertext produced by the model. Both core and MMIO paths check both
directions: 4,632 independent transform comparisons. `sequence_oracle` adds
640 reuse/rekey/direction comparisons, for **5,272 independent OpenSSL transform
comparisons per full suite**, plus 515 AES-128 checks with a changed unused key half.

Vectors are generated in each build directory, never using the AES model
as their reference. The original two FIPS known-answer tests remain enabled
even without OpenSSL. The generator checks command success and exact output
length; the C++ loader validates row format and both corpus counts.

## Evidence and limits

CTest transcripts are stored under each build's `Testing/Temporary/LastTest.log`.
The full script also writes `results.xml` in each build directory and
`build/fu3-aes/coverage/*.gcov.json.gz`. The JSON summary includes test names,
compiler, OpenSSL version, CMake flags, reference-corpus hash and source hashes.
Coverage includes only `src/aes_core.cpp` and `src/aes_controller.cpp`; it
does not imply PDF requirement coverage or RTL equivalence. The full script
clears only the two generated AES counter files before the Debug tests, so
its coverage reflects that run. Running CTest separately can accumulate counters.

These tests do not verify AXI pin timing, RTL defects beyond the optional
result mux, all possible keys/inputs or interleavings, multi-master fabric arbitration, or a
complete FU3 SoC boot. See [spec decisions](spec-decisions.md) before using
the VP software map against hardware.
