# HDS verification matrix

Generated from `hds-requirements.json` and the three completed build reports.
Requirement clarity and verification status are separate. A clear requirement can remain
`SPEC_BLOCKED` or `SPEC_NOT_TESTED`; a passing policy/reference test cannot promote it.

**SPEC_PASS here is restricted to the stated core-vector/round-trace scope. It is not whole-IP HDS sign-off.**

Source: `FTEL_CDC_IP_AES_Specification (1).pdf`, physical PDF pages; SHA-256:
`b339ebbd0fdff57c6f483d062920f05b3a861e505df52b697de171c2c7229b45`.

The fixed FIPS 197 Appendix C vectors are specification-derived because HDS p.8 section 1.2
explicitly references FIPS 197. They are not vector results supplied inside the HDS itself.
OpenSSL-generated outputs are classified only as REFERENCE evidence.

| ID / function | HDS source | Clarity | Verification across Debug/UBSan/Release | Expected / limitation / blocker |
| --- | --- | --- | --- | --- |
| AES-HDS-001 — AES-128 encryption, 128-bit block | p.8 sections 1.2/2; p.11 section 6.3; FIPS197 C.1 | SPEC_CLEAR | SPEC_PASS | 00112233445566778899aabbccddeeff with key 000102030405060708090a0b0c0d0e0f -> 69c4e0d86a7b0430d8cdb78070b4c55a; Core output for the cited vector only; not CONFIG decode or intermediate-round verification |
| AES-HDS-002 — AES-128 decryption | p.8 sections 1.2/2; p.11 section 6.4; FIPS197 C.1 | SPEC_CLEAR | SPEC_PASS | C.1 ciphertext and key -> 00112233445566778899aabbccddeeff; Core output for the cited vector only |
| AES-HDS-003 — AES-256 encryption, 128-bit block | p.8 sections 1.2/2; p.11 section 6.3; FIPS197 C.3 | SPEC_CLEAR | SPEC_PASS | 00112233445566778899aabbccddeeff with key bytes 00..1f -> 8ea2b7ca516745bfeafc49904b496089; Core output for the cited vector only; not CONFIG decode or intermediate-round verification |
| AES-HDS-004 — AES-256 decryption | p.8 sections 1.2/2; p.11 section 6.4; FIPS197 C.3 | SPEC_CLEAR | SPEC_PASS | C.3 ciphertext and key -> 00112233445566778899aabbccddeeff; Core output for the cited vector only |
| AES-HDS-005 — AES-128 selects upper 128 bits of the core key | p.12 section 6.5; p.17 word ordering; FIPS197 C.1 | SPEC_CLEAR | SPEC_PASS | C.1 ciphertext remains unchanged for 256 lower-half fill patterns; Core API key selection, not CSR address/word packing |
| AES-HDS-006 — 10/14 rounds, final round omits MixColumns, forward/inverse transformations | HDS p.11 sections 6.3-6.4; FIPS197 Appendix C.1/C.3 | SPEC_CLEAR | SPEC_PASS | AES-128 10 rounds; AES-256 14; final forward round excludes MixColumns; Every published intermediate state and phase order in the cited vectors, both directions; exact 10/14 rounds and final-round omission. No claim about RTL physical S-box sharing. |
| AES-HDS-007 — Round key generation, Rcon, up to 15 round keys, both AES-256 key halves | HDS p.12 section 6.5; FIPS197 Appendix C.1/C.3 k_sch/ik_sch | SPEC_CLEAR | SPEC_PASS | FIPS key expansion with Rcon in GF(2^8); All 11 AES-128 / 15 AES-256 expanded round keys, forward and reverse order, for the two FIPS keys. Internal Rcon register and physical RTL key-memory architecture are not observed. |
| AES-HDS-008 — One clock domain, no internal clock gating or CDC | p.11 section 6.1 | SPEC_CLEAR | SPEC_NOT_TESTED | AXI adapter and core share i_axi_aclk; Structural review only; clock-stop VP tests are not HDS timing proof |
| AES-HDS-009 — Reset polarity and synchronous/asynchronous comments | p.11 section 6.2; p.18 AES-R01/R05 | SPEC_CONFLICT | SPEC_CONFLICT | Coherent synchronous active-low reset instead of opposite wrapper/core polarities; owner questions: HDS-Q01 |
| AES-HDS-010 — Byte/word CSR translation, alignment and aliases | p.10 ADDR_WIDTH; pp.14-17 local map; p.18 AES-R03 | SPEC_UNDEFINED | SPEC_UNDEFINED | Byte offset = index * 4; no high-address alias; owner questions: HDS-Q02 |
| AES-HDS-011 — NAME0/NAME1/VERSION contents and RO access | p.14 register table | SPEC_CLEAR | SPEC_BLOCKED | aes-space, spaces, version 0.60; owner questions: HDS-Q01, HDS-Q02; policy evidence only: registers, hds_fields |
| AES-HDS-012 — Explicit reserved ranges | pp.14-15 indices 03..07,0b..0f,18..1f,24..2f | SPEC_CLEAR | SPEC_BLOCKED | Reads zero; writes have no effect within the stated ranges; Do not extend this expectation automatically to all indices below 0x100; owner questions: HDS-Q01, HDS-Q02; policy evidence only: registers, hds_fields |
| AES-HDS-013 — CONFIG/KEY/BLOCK write-only readback | pp.15-16 register/bit tables | SPEC_CLEAR | SPEC_BLOCKED | Readback zero; owner questions: HDS-Q01, HDS-Q02; policy evidence only: registers, hds_fields |
| AES-HDS-014 — CONFIG direction/key length and reserved bits | p.16 CONFIG bits | SPEC_CLEAR | SPEC_BLOCKED | bit0: encrypt=1/decrypt=0; bit1: 256=1/128=0; bits31:2 ignored; owner questions: HDS-Q01, HDS-Q02, HDS-Q03; policy evidence only: registers, aes128, aes256, hds_fields |
| AES-HDS-015 — CTRL pulse, CONFIG mirror and reserved readback | p.12 section 6.6; p.16 CTRL bits | SPEC_CLEAR | SPEC_BLOCKED | INIT/NEXT one-cycle commands; bits3:2 mirror CONFIG; upper bits zero; owner questions: HDS-Q01, HDS-Q02; policy evidence only: commands, hds_fields |
| AES-HDS-016 — INIT schedules key; normal INIT then NEXT programming sequence | p.12 sections 6.5/6.6; p.17 section 9.1 | SPEC_CLEAR | SPEC_BLOCKED | Program config/key/block; INIT and poll READY; NEXT and poll VALID; owner questions: HDS-Q01, HDS-Q02, HDS-Q03; policy evidence only: aes128, aes256, hds_fields |
| AES-HDS-017 — STATUS bit meanings and reserved readback | p.12 section 6.6; p.16 STATUS bits | SPEC_CLEAR | SPEC_BLOCKED | READY bit0 reflects core ready; VALID bit1 indicates completed result; upper bits zero; owner questions: HDS-Q01, HDS-Q02; policy evidence only: status_flow, hds_fields |
| AES-HDS-018 — CSR KEY/BLOCK concatenation | p.17 key/block word ordering | SPEC_CLEAR | SPEC_BLOCKED | KEY0 core_key[255:224], KEY7 [31:0]; BLOCK0 [127:96] through BLOCK3 [31:0]; owner questions: HDS-Q01, HDS-Q02, HDS-Q03; policy evidence only: aes128, aes256, bus_lanes, hds_fields |
| AES-HDS-019 — Result word map: delivered versus intended behavior | pp.15-17; p.18 AES-R02 | SPEC_CONFLICT | SPEC_CONFLICT | Default returns all four MSW-first words, deviating from described delivered decode; observed mux option is not approved baseline; owner questions: HDS-Q03 |
| AES-HDS-020 — AXI handshake channels, ID capture/return, WLAST/RLAST | pp.12-14 section 7.1; p.17 section 9.2 | SPEC_CLEAR | SPEC_NOT_TESTED | VALID/READY handshakes; CSR write after AW and W accepted; captured ID on response; Not modeled by generic TLM socket; requires an AXI adapter/RTL testbench, not a SPEC_PASS from FU3 bus tests |
| AES-HDS-021 — Supported AXI INCR and sizes up to four bytes | pp.9,17-18 | SPEC_CLEAR | SPEC_NOT_TESTED | Adapter fires INCR transactions with transfer size <=4 bytes; TLM fabric decomposes transactions and loses raw AXI channel evidence; no protocol-level SPEC test |
| AES-HDS-022 — Unsupported AXI request response/completion | p.18 AES-R04 | SPEC_UNDEFINED | SPEC_UNDEFINED | Immediate TLM validation errors; does not model delivered possible stall; owner questions: HDS-Q04 |
| AES-HDS-023 — WSTRB merges enabled write lanes | p.10 STRB_WIDTH; p.17 section 9.2 | SPEC_CLEAR | SPEC_BLOCKED | Write byte lanes merged using WSTRB; TLM byte-enable repetition and partial read preservation are separate VP policies; owner questions: HDS-Q01, HDS-Q02; policy evidence only: strobes, bus_lanes, hds_fields |
| AES-HDS-024 — Default bus parameter widths | p.8 section 2; p.10 table 5-1 | SPEC_CLEAR | SPEC_NOT_TESTED | ADDR_WIDTH=32, DATA_WIDTH=32, STRB_WIDTH=4, ID_WIDTH=4; block=128; Generic TLM has no AWID/RID width; no full parameter conformance test |
| AES-HDS-025 — Non-default widths and parameter ranges | p.10 table 5-1; p.18 AES-R06 | SPEC_UNDEFINED | SPEC_UNDEFINED | Fixed 32-bit target only; owner questions: HDS-Q05 |
| AES-HDS-026 — Exact INIT/encrypt/decrypt latency and throughput | p.11 section 6.3; p.12 section 6.6 | SPEC_UNDEFINED | SPEC_UNDEFINED | Default 16/64 cycles; positive configurable values; owner questions: HDS-Q06 |
| AES-HDS-027 — Reset values, retention and ready after release | p.11 section 6.2; pp.14-16 lack reset-value column | SPEC_UNDEFINED | SPEC_UNDEFINED | Data/config/result zero; READY=1; VALID=0; all in-flight state cleared; owner questions: HDS-Q07 |
| AES-HDS-028 — Commands and writes while busy | pp.12,16-17 describe normal operation only | SPEC_UNDEFINED | SPEC_UNDEFINED | Reject key/block/config writes and nonzero commands while busy; owner questions: HDS-Q08 |
| AES-HDS-029 — Simultaneous INIT and NEXT | p.16 CTRL bits | SPEC_UNDEFINED | SPEC_UNDEFINED | INIT priority; owner questions: HDS-Q09 |
| AES-HDS-030 — NEXT before completed INIT | p.17 section 9.1 gives sequence but not violation outcome | SPEC_UNDEFINED | SPEC_UNDEFINED | Generic TLM error with no mutation; owner questions: HDS-Q10 |
| AES-HDS-031 — VALID clear/retention, old result visibility and atomicity | p.12 section 6.6; p.16 STATUS | SPEC_UNDEFINED | SPEC_UNDEFINED | INIT/NEXT clear VALID; reads retain it; old words held until atomic completion; owner questions: HDS-Q11 |
| AES-HDS-032 — Key/config updates and schedule reuse | pp.12,16-17 | SPEC_UNDEFINED | SPEC_UNDEFINED | Key write/length change invalidates schedule; direction-only change reuses it; owner questions: HDS-Q12 |
| AES-HDS-033 — Input/key/config sampling instant | pp.12,17 do not define sampling edges | SPEC_UNDEFINED | SPEC_UNDEFINED | INIT snapshots key; NEXT snapshots input/direction; owner questions: HDS-Q13 |
| AES-HDS-034 — AES-192 and block modes excluded | pp.8-9 section 3 | SPEC_CLEAR | SPEC_NOT_TESTED | No AES-192, CBC/CTR/GCM/XTS, automatic chaining or IV/counter interface; API/design review only; absence of an API is not a dynamic SPEC_PASS |
| AES-HDS-035 — IRQ/FIFO/UART/APB overview versus concrete feature scope | p.7 introduction; p.9 exclusions; pp.12-14 AXI/UART headings | SPEC_CONFLICT | SPEC_CONFLICT | AXI-oriented TLM endpoint, no IRQ/DMA/FIFO/UART/APB implementation; owner questions: HDS-Q14 |
| AES-HDS-036 — Document baseline and missing section references | cover V1.0; p.4 history V2.0; FU2/UART headings; p.17 missing section 8.1 reference | SPEC_CONFLICT | SPEC_CONFLICT | Use pinned PDF hash without treating proposed fixes as approved revisions; owner questions: HDS-Q15 |
| AES-HDS-037 — SoC base, aperture and unmapped/reserved indices | pp.14-17 only define local indices through 0x33 | SPEC_UNDEFINED | SPEC_UNDEFINED | Existing FU3 base 0x40000000/64KiB bus window; model 0x400 bytes; all unspecified slots inside treated reserved; owner questions: HDS-Q16 |
| AES-HDS-038 — No DMA handshake/key vault/explicit zeroization/security certification | p.9 section 3 | SPEC_CLEAR | SPEC_NOT_TESTED | Listed features and certification not supplied; Scope limitation, not a positive security certification or dynamic SPEC_PASS |

Owner questions and response fields: [hds-clarification-request.md](hds-clarification-request.md).
Machine evidence: `build/fu3-aes/verification-summary.json`.
TLM error codes, debug/DMI, concurrency/locking, annotated delay and reset-cancellation rules
are model contracts outside HDS; their results remain in the POLICY suite.
