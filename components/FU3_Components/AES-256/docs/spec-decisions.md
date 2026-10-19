# Specification trace and VP decisions

Current requirement classifications and verdicts are in
[hds-verification-matrix.md](hds-verification-matrix.md). Decisions below
remain **unapproved VP policies** wherever the HDS is undefined/conflicting.
They do not provide expected values for SPEC_PASS/FAIL. The owner questions
are collected in [hds-clarification-request.md](hds-clarification-request.md).

Source: `FTEL_CDC_IP_AES_Specification (1).pdf`, 23 pages, supplied at
`D:/Intern_doc/Intern/VP/004_IP/003_AES/`. SHA-256:
`b339ebbd0fdff57c6f483d062920f05b3a861e505df52b697de171c2c7229b45`.
The cover says V1.0, while the revision history lists V2.0 (Apr 03, 2026).
Page numbers below are physical PDF page numbers. The original PDF is not
copied into this repository.

The document has template remnants (FU2/UART headings, verification and SDC
guidelines). Concrete AES sections and Appendix B on p.18 define the source
scope; template instructions are not treated as implementation requests.

| Topic | Source | Implementation / boundary |
| --- | --- | --- |
| AES-128/256, 128-bit blocks | pp.8,11-12 | Functional encryption, decryption and key expansion. AES-192 absent. |
| AXI vs APB | overview p.7 vs pp.8,12-14 | One TLM target representing the AXI CSR endpoint; no APB or UART interface. |
| Clock | p.11 section 6.1 | One clock input, all core progress on rising edges. |
| Reset | p.11 and AES-R01/R05 p.18 | VP uses coherent synchronous active-low reset for bus and core. Does not reproduce the unusable opposite-polarity wrapper connection. |
| CSR address map | pp.14-17, AES-R03 p.18 | Local index multiplied by four. No low-eight-bit byte-address aliasing. This is a proposed software map, not an RTL-confirmed translation. |
| Results | pp.15-17, AES-R02 p.18 | Full-block readback by default. Optional observed mux profile exposes missing words and inferred RESULT0 truncation. |
| Word order | p.17 | KEY0 is the most significant word; AES-128 uses KEY0..3. BLOCK0 and functional RESULT0 are MSW. |
| Data width / strobes | pp.10,17-18, AES-R06 | Fixed 32-bit target with enabled-byte merge. Bus fabric splits bursts; raw AXI AW/W/AR/B/R handshakes and IDs are outside this model. |
| Unsupported requests | pp.9,18, AES-R04 | Bounded validation errors instead of reproducing an RTL response hang. Waiting for a stopped clock still requires a platform watchdog. |
| Timing | pp.11-12 | Configurable positive INIT/NEXT cycle counts, default 16/64. Not RTL performance estimates. |
| Command/state corner cases | pp.12,16-17 describe normal sequence only | Busy writes rejected; INIT wins combined command; key write/key-length change invalidates schedule; direction can reuse it. VALID clears on accepted INIT/NEXT and persists on reads. |
| Reset cancellation | not specified for TLM | Sampled reset clears all model state and invalidates calls already waiting on delay/lock/clock. A request entering while reset is low remains invalid even if its annotated delay outlasts an unsampled reset pulse. |
| Reset values | incomplete in PDF | Control/data/results zero; READY=1 after reset; VALID=0. ID strings match p.14. |
| Unsupported features | pp.8-9 | No DMA request, IRQ, IV, chaining, key vault or certification. Introductory FIFO/interrupt prose is not implemented. |

The AES arithmetic follows [FIPS 197-upd1](https://doi.org/10.6028/NIST.FIPS.197-upd1).
Known-answer tests use the historical
[FIPS 197 Appendix C](https://nvlpubs.nist.gov/nistpubs/fips/nist.fips.197.pdf),
C.1 and C.3 (the algorithm is unchanged by the editorial update).
Passing these vectors does not establish RTL equivalence, exhaustive
cryptographic validation or full AXI protocol compliance.
