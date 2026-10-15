# DDR3 Controller VP — Verification Report (Subtasks 4 and 5)

Audience: reviewers and the receiving FW/SW team. This report states what the regression proves, how to reproduce it, and what it does not cover.

The baseline is HAS V1.0 and MAS V0.1 (hashes in [baseline.md](baseline.md)), on CDC-VP branch `dev` (uncommitted working tree). Build: `/usr/bin/g++` with SystemC 2.3.4.

## 1. Requirement coverage

[traceability.md](traceability.md) is generated from the P0 audits by `docs/tools/gen_traceability.py`. It contains 399 requirements:

| Status | Count | Meaning |
|---|---|---|
| Done S1–S4 | 333 | Implemented, with test evidence named in the matrix |
| Limitation | 18 | Agreed VP limit, each with an L-ID in [limitations.md](limitations.md) |
| OUT | 5 | Excluded by the specification |
| n/a | 43 | ABSTRACTED in the audits (below the VP level: pin handshakes, internal pipelines, CDC) |

No requirement is left Planned or Partial. The undefined encodings that firmware reads are provisional, not missing: there are 15 Part A items in [spec_owner_questions.md](spec_owner_questions.md), all in `provisional_encodings.h`.

## 2. Regression

`make test` in the component runs 101 CTest cases with about 1 740 checks; `ctest -L ddr3` in a CDC-VP build runs the 95 that do not need the standalone install. A serial run takes a few seconds of wall time, most of it in the package consumer builds.

| Executable | Cases | Checks | Content |
|---|---|---|---|
| `test_core` | 7 | 351 | Profiles, capability words, speed-bin images vs MAS Tables 5-15..5-18, DFI bounds, config checks, time base, burst planner |
| `test_datapath_core` | 7 | 90 | SECDED (MAS T6-154 plus an independent bit-array encoder), mapping (G1–G4, G7, bijection over a whole geometry), frontend decode (E1, E2), atom plan (E3–E6), exclusive monitor (E8–E10), sparse store, bank timer (G9 plus refresh windows) |
| `test_tlm` | 9 | 265 | Binding, identity, CSR robustness, reset/quiesce contract, PHY handshake/faults, epoch scheduler |
| `test_csr` | 7 | 131 | All 210 CSR words, commits, mutex/snapshot, IRQ, policy, FE_PORT |
| `test_lifecycle` | 15 | 284 | Boot (three profiles, ECC on/off), init/PHY/training failures, runtime training, power, refresh, cancellation by every reset |
| `test_memory` | 18 | 236 | Scoreboard traffic (three profiles, ECC on/off), decode verdicts, ordering, exclusive (incl. races and power stop), admission/quiesce/publication with traffic, multi-port, 4 GiB capacity edges, timing, debug transport, platform bridge (AXI128/AXI64) |
| `test_ecc` | 14 | 259 | CE/UE/RMW (G6), scrub frontier, quarantine (G13 and all reasons), every fatal source from a clean controller, fatal containment incl. followers, resets with traffic |
| `test_verify` | 16 + 1 | 127 | Subtask 4 conformance cases, seeded soaks, idle efficiency, performance report; soak reproducibility |
| `test_regs_header.c` | 1 | — | The firmware header compiles as strict C99 |
| package consumer | 6 | — | Installs the package, then configures (clean build directory), builds and runs a consumer that uses only public headers, once per SystemC discovery method: `CMAKE_PREFIX_PATH`, `-DSYSTEMC_HOME`, environment `SYSTEMC_HOME`, a header/library-only prefix as `SYSTEMC_HOME` or on `CMAKE_PREFIX_PATH`, and explicit `SYSTEMC_INCLUDE_DIR`/`SYSTEMC_LIBRARY` (standalone build only). The fixture takes the SystemC that the producer build actually resolved, so the cases pass whichever method configured the producer: verified with producers configured by `SYSTEMC_HOME`, by `CMAKE_PREFIX_PATH` only, and by explicit header/library |

Inside CDC-VP, the DDR3 cases carry the labels `fx1_unit` and `ddr3`. The package consumer cases are standalone-only, so the IP contributes 95 cases there. Subtask 5 adds 10 platform unit cases (`fx1_ddr3_subsystem_*`, also labelled `ddr3`) and 17 CPU tests (`fx1_smoke_ddr3_*`); [acceptance_report.md](acceptance_report.md) describes them. Result at hand-off (subtask 5):

- `fx1_all` builds with 0 compiler warnings.
- `platforms/fx1_soc/ci/run_ci.sh`: PASS (configure, manifest, build, smoke, components, regression).
- `fx1_unit|fx1_smoke|fx1_regression`: 196/196, including the 95 IP cases, the 10 platform cases and the 17 CPU tests.
- Standalone `make test`: 101/101.

### 2.1 Independent oracles

- **Scoreboard.** The memory scoreboards compute AXI byte placement with their own implementation of the AXI4 beat walk (IHI0022 A3.4.1), not with the model's planner.
- **SECDED.** The encoder is cross-checked against a straightforward bit-array encoder and the MAS golden vector, on 2 000 random words with single- and double-bit error sweeps.
- **Golden values.** These are transcribed from MAS/HAS tables, via the audit worked examples (backend G1–G13, frontend E1–E15).

### 2.2 Seeded randomized soak

`fx1_ddr3_test_verify soak|soak_legacy|soak_noecc [seed]` (or env `DDR3_SEED`) prints the seed, profile and port count, then a digest of every response and the final simulated time.

Each iteration issues one to four concurrent random transactions on different ports and windows. Between iterations it applies random lifecycle events:

- port quiesce;
- global quiesce;
- snapshot;
- power-down or self-refresh round trip;
- a single-bit ECC fault.

At the end, every window is read back through another port.

- **Reproducibility:** `fx1_ddr3_verify_soak_reproducible` runs seed 7 twice and requires identical digests.
- **Seed sweep:** soak seeds 1–40, plus soak_legacy and soak_noecc seeds 1–15, all pass.
- **A sweep found a test bug:** a granule corrupted twice in one lane, a real UE. The soak now corrupts a granule at most once.

## 3. Mutation checks

Every behavioural fix since subtask 2 was mutation-checked: the fix was reverted alone and the matching regression rerun.

- **Subtask 3:** 29 mutants, 27 caught. The remaining two are equivalent: a defensive early guard covered by a later check, and a lock enqueue for masked atoms with no observable effect.
- **Subtask 3 review fixes:** 11 mutants, 9 caught. The remaining two are equivalent: overlapping containment guards, kept as defence in depth.
- **Subtask 4:** the clock-disable control (three mutants) and the scrub-order function (one mutant), all caught.
- **Subtask 4 review fixes:** each immediate fatal source disabled alone, or mapped to the wrong bit (five mutants); the scrub-fatal bit (one); the PHY rank-identity check (one). All caught. Restoring the previous package config makes four of the five consumer discovery cases fail. The reviewer's `subtask4_fault_coverage.py` now reports its mutant caught (`fx1_ddr3_ecc_fatal_sources` fails).
- **Subtask 5:** 9 DDR_SYSCTRL/subsystem mutants, 5 platform mutants against the CPU tests, 1 UART-hold mutant, all caught; two of them only after a test was added ([acceptance_report.md](acceptance_report.md) §4.1).
- **Subtask 5 review fixes (S5-R1..R3):** 10 mutants (wake back-off, storage listener and its three hook points, canonical identity in the monitor, range splitting), all caught; the two splitting mutants only after `fx1_ddr3_subsystem_canonical` was added.
- **Host-cost changes (VPL-56)** are behaviour-identical: the new SECDED syndrome agrees with the previous implementation on 5.66 M cases (random words and every single and double flip of 2 000 words), and the reference workload ends at the same simulated time to the picosecond.

Mutation runs are recorded in the review responses under `plan/`.

## 4. Efficiency

| Check | Result |
|---|---|
| Idle simulation: 100 ms (40 M controller cycles, ~12 800 tREFI) | 0 scheduled actions executed (`verify idle_events`) |
| Startup scrub of the reference geometry: 2 ranks × 4 Gb x8, ECC, 134 217 720 writes | One scheduled completion. About 0.67 s simulated, under 1 ms wall. No storage allocated |
| 8 MiB of ECC traffic on that geometry | 2 305 resident 4 KiB pages (2 048 data + companion records), process RSS 8.5 → 18 MiB, 161 ms wall (`verify perf`) |
| Full regression | about 1.5 s wall, serial |

These figures are measured, not thresholds. Wall times are from the development host and will vary with the host.

## 5. What is not verified here

- **Sanitizers.** `make sanitize` exists, but libasan and libubsan are not installed on the delivery host.
- **CPU timing fidelity**: the VP harts have no cache, so CPU-versus-DMA timing is pessimistic (L-19). Functional results on the CPU are in [acceptance_report.md](acceptance_report.md).
- **Signal-level AXI/DFI/CDC behaviour and cycle-exact timing** are outside the VP abstraction ([limitations.md](limitations.md) L-01, L-02). Such tests need RTL simulation.
- **Spec-owner answers.** Behaviour that depends on them (SOQ-01..38) follows the documented proposals and may change.

## 6. Reproduce

```bash
cd CDC-VP/components/FX1_Components/ddr3
make test                                         # build, 101 cases incl. six package consumers
./build/tests/fx1_ddr3_test_verify soak 1234      # one seeded soak, prints seed/digest
./build/tests/fx1_ddr3_test_verify perf           # efficiency report
/usr/bin/python3 docs/tools/gen_traceability.py   # regenerate the coverage matrix
```
