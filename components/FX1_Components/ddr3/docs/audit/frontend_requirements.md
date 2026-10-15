# DDR3 Controller VP: Frontend Requirements Audit (Phase P0)

## 1. Scope and sources

Scope: the frontend (AXI memory ports, address decode, ordering, write/read data movement, QoS capture, inter-port arbitration, exclusive monitor, response construction, port lifecycle, map publication). Only behavior visible at the IP boundary is broken out row by row. Internal microarchitecture is grouped into ABSTRACTED rows.

Precedence: HAS (normative) > MAS (supplementary) > RTL. MAS chapters 7–9 are missing from the file, so nothing here is inferred from them.

| Source | Sections | Lines actually read |
| :--- | :--- | :--- |
| HAS `FTEL_IP_DDR3_CONTROLLER_HAS_v1.0.md` | §2.2–2.4 (Tables 2-2, 2-3) | 770–845 |
| HAS | §6.1.1–6.1.15 (Tables 6-1 … 6-29) | 1220–2150 |
| HAS | §7.4.1–7.4.15 (Tables 7-11 … 7-25) | 3115–3545 |
| HAS (cross-ref only) | Table 1-3/1-4 (FE01, FE03, QoS/reserved-rate sideband) | 676–730 |
| HAS (cross-ref only) | §8 coherent commit, CSR map rows, Table 8-8, Table 8-10 | 3975–4025, 4113–4130, 4280–4306, 4318–4340 |
| MAS `FTEL_IP_DDR3_CONTROLLER_MAS_v0.1.md` | §6.1 intro, §6.1.1–6.1.15 (Tables 6-1 … 6-69) | 1925–3225 (no base64 lines in range) |

VP class legend: **MODEL** = implemented functionally. **APPROX** = implemented with abstracted timing or capacity. **ABSTRACTED** = intentionally not modeled because it is below the VP abstraction (the reason is given). **OUT** = excluded by the spec.

Test-ID prefixes: VP-AXI (protocol/response), VP-ORD (ordering), VP-MEM (data placement), VP-EXCL (exclusive), VP-QOS (QoS sideband/admission), VP-CFG (map/config), VP-LIFE (quiesce/reset), VP-DIAG (diagnostics/fault injection).

Abbreviations: A = AXI start address, B_r/S_r = region r base/size, beat_B = 2^SIZE, span_B = (LEN+1)×beat_B, G = 32-byte granule.

---

## 2. Requirement table

### 2.1 AXI profile (HAS §2.2–2.4)

| Req ID | Source | Requirement (one testable statement) | VP class | Proposed VP test ID | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-2.2-01 | HAS T2-2 INCR | An INCR burst with LEN 0–255 (1–256 beats) is accepted and completes OKAY when the size, 4-KB and region-span checks pass. | MODEL | VP-AXI-01 | Boundaries to test: LEN=0, LEN=255 (4096 B at 16-B beats). |
| FE-2.2-02 | HAS T2-2 WRAP; §6.1.9 | A WRAP burst of 2, 4, 8 or 16 beats with a size-aligned start is accepted, and its beats follow the AXI wrap progression inside the aligned container. | MODEL | VP-AXI-02, VP-MEM-03 | Container basis: see GAP-FE-02. |
| FE-2.2-03 | HAS T2-2 FIXED; MAS T6-22 fixed_ok | A single-beat FIXED burst is accepted. A FIXED burst with LEN>0 is accepted and answered SLVERR. | MODEL | VP-AXI-03 | Golden: MAS T6-23 row 7. |
| FE-2.2-04 | HAS T2-2 sizes, T2-3 | SIZE gives 1 B up to AXI_DATA_WIDTH/8 (16 B for AXI128, 8 B for AXI64). A larger SIZE is answered SLVERR. | MODEL | VP-AXI-04 | Golden: MAS T6-23 row 6 (32-B beat → SLVERR). |
| FE-2.2-05 | HAS T2-2 narrow/unaligned; §6.1.6 | Narrow and unaligned beats write only the bytes selected by lane and strobe. A read returns each byte on its AXI lane. | MODEL | VP-MEM-01, VP-MEM-02 | TLM data layout: see GAP-FE-15. |
| FE-2.2-06 | HAS T2-2 4-KB; FE04 | An INCR burst that crosses a 4-KB boundary is answered SLVERR. Every burst span must fit the selected region. WRAP is checked against its container. | MODEL | VP-AXI-05, VP-AXI-06 | Golden: MAS T6-23 rows 3 and 5. |
| FE-2.2-07 | HAS T2-2 outstanding; T2-3 | Each port holds up to 32 transaction contexts, subject to read-return, write-store and in-flight atom capacity. | APPROX | VP-AXI-20 | Modeled as counters. Exact boundary not asserted (see C9). |
| FE-2.2-08 | HAS T2-2 same-ID | Within a port, contexts with the same AXI ID follow one acceptance-order chain across reads and writes. AW and AR acceptance stay independent. A same-time, same-ID AW+AR places AW before AR. Different IDs progress independently. | MODEL | VP-ORD-01..04 | VP acceptance order: GAP-FE-06. |
| FE-2.2-09 | HAS T2-2 read-beat ordering | Beats of one read stay in AXI order. R data of different reads on one port is never interleaved. | MODEL | VP-ORD-05 | Trivial at TLM granularity, but per-port completion order is still tested (FE-6.1.8-01). |
| FE-2.2-10 | HAS T2-2 backpressure; §6.1.1 | AW/W/AR/B/R use independent VALID/READY, and a response payload is stable until accepted. | ABSTRACTED | – | No per-beat handshake in the TLM model. |
| FE-2.2-11 | HAS T2-2 exclusive | Exclusives work only on ports in the exclusive mask (default 0001, port 0 only). | MODEL | VP-EXCL-06 | VP parameter `excl_port_mask`, default 0x1. Details in §2.12. |
| FE-2.2-12 | HAS T2-2 QoS; FE05 | The 4-bit AxQOS is captured at address acceptance and mapped through a 16-entry per-port class table. | MODEL | VP-QOS-01..03 | Sideband per HAS T1-4 (not a CSR). |
| FE-2.2-13 | HAS T2-2; FE04 | AxCACHE and AxPROT are kept as metadata only. They do not change data, response, ordering, ECC policy or permission. | MODEL | VP-AXI-10 | Sweep all CACHE/PROT values: same data and response. |
| FE-2.2-14 | HAS T2-2 USER/REGION | AxUSER/WUSER/BUSER/RUSER and AxREGION are not part of the port. Decode uses only the committed region image. | OUT | VP-AXI-11 | VP ignores these fields if a generic extension carries them. |
| FE-2.2-15 | HAS T2-2 | AXI3 WID/locked mode, ACE coherency, AXI5 ATOP and an AXI4-Lite data-port mode are not supported. | OUT | – | Lock is the single AXI4 exclusive bit. |
| FE-2.2-16 | HAS T2-3; §7.4.1 | Address is 32 bit, ID 4 bit, RESP 2 bit, LEN 8 bit, data 128 bit (AXI128) or 64 bit (AXI64). | MODEL | VP-AXI-12 | Out-of-range extension values: GAP-FE-18. |
| FE-2.2-17 | HAS T2-3; FE01 (T1-3) | AXI_PORT_COUNT is 1–4 (default 4). Default profile is AXI128 at ratio 2. Unused port positions are tied off and their arbitration offer is disabled. | MODEL | VP-CFG-01 | VP: `n_ports` param; unbound sockets mean disabled ports. |
| FE-2.2-18 | HAS T2-3, §2.2 text | The memory service granule is fixed at 32 bytes with 32 byte-enable bits. | APPROX | VP-MEM-10 | Granule count drives the approximate DRAM-burst timing. |
| FE-2.2-19 | HAS §2.2 FE06 | Efficiency targets (FE06 workload definition) are measured on the steady-state cycle-level design. | ABSTRACTED | – | The VP is not cycle accurate. Performance numbers are not verified on the VP. |
| FE-2.3-01 | HAS §2.3 | An atom becomes eligible only with valid context, ordering permission, pending work and resources (a read needs a reserved return slot, a write needs resident data). A visible offer does not commit ownership. | ABSTRACTED | – | Internal pipeline. The ordering part is covered by FE-6.1.5-01. |
| FE-2.3-02 | HAS §2.3 | Shared arbitration uses class, rate policy and age. Backend ingress acceptance commits the atom. Bank and timing legality are checked later. | ABSTRACTED | – | No cycle scheduler. Bank/row timing is APPROX in the backend audit. |
| FE-2.4-01 | HAS §2.4 | Write beats are packed into 32-B segments with per-byte enables and kept until the backend fetches them. | ABSTRACTED | – | The VP applies the write data functionally at the commit point. Only the resulting memory image is observable. |
| FE-2.4-02 | HAS §2.4 | A read completion is matched to its saved slot and epoch. A stale or malformed return never completes another transaction. | ABSTRACTED | VP-DIAG-02 (optional) | The VP has no separate return channel (see FE-6.1.8-03). |

### 2.2 Port adapter and CDC (HAS §6.1.1–6.1.2)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.1-01 | HAS §6.1.1; §6.1.5 | Each port is an independent target. No ordering is implied across ports. | MODEL | VP-ORD-06 | Tests must not assume cross-port order. |
| FE-6.1.1-02 | HAS §6.1.1 | A request is accepted only when the context, data and read-return resources for its direction are available. Otherwise AW/AR stall. | APPROX | VP-AXI-20 | Modeled as a blocking wait in the target (GAP-FE-04). |
| FE-6.1.1-03 | HAS §6.1.1; §6.1.3; MAS T6-8 | An address or attribute error still takes a context, drains the W data and returns exactly one terminal response (one B, or LEN+1 R beats). | MODEL | VP-AXI-07 | |
| FE-6.1.1-04 | HAS §6.1.1 T6-2; §6.1.4 | The context tag returns when the terminal B/R is enqueued toward AXI, not at the master handshake. | APPROX | – | Capacity counter is released on response completion. |
| FE-6.1.1-05 | HAS §6.1.1 FE04 | Attribute values (CACHE, PROT, QOS) never bypass the address-map, burst-geometry, ordering or admission checks. | MODEL | VP-AXI-10 | |
| FE-6.1.1-06 | MAS T6-1, T6-10 | Throughput is one take every 2 cycles for a lone port; several fixed latencies apply. | ABSTRACTED | – | Cycle timing. The VP uses lumped port latency parameters. |
| FE-6.1.2-01 | HAS §6.1.2; §7.4.2; MAS §6.1.2 | Five asynchronous FIFOs (default depths AW 8, AR 8, B 8, W 16, R 16), Gray pointers, SYNC_STAGES=2, first-word fall-through, per-domain reset release. | ABSTRACTED | – | No CDC or metastability modeling (VP decision). |
| FE-6.1.2-02 | HAS §6.1.2; MAS T6-16 | During a stop (quiesce), new AW and AR are refused, but W owed to AWs accepted before the stop is still taken. AW/AR already inside the crossing are still accepted. | MODEL | VP-LIFE-01 | VP form: admission is decided at TLM arrival time (GAP-FE-05). |
| FE-6.1.2-03 | HAS §6.1.2/6.1.4/6.1.5 FE10 | Emergency reset resets masters, controller and PHY together. All contexts are cleared, no response is promised, issued writes are not rolled back, and full init, training and ECC scrub are required before admission reopens. | APPROX | VP-LIFE-08 | Handling of in-flight TLM calls: GAP-FE-21. |
| FE-6.1.2-04 | HAS §6.1.2 rules | Both clocks must keep running for reset release and drain. | ABSTRACTED | – | The VP has no clock gating of port domains. |

### 2.3 Address decode and region validation (HAS §6.1.3, §7.4.3)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.3-01 | HAS §6.1.3 T6-5; MAS FE-DEC-01 | Each port has 4 half-open regions. Region r is enabled iff S_r≠0, and hit_r = (A ≥ B_r) ∧ (A − B_r < S_r). | MODEL | VP-CFG-03 | Golden: HAS example, base 0x1000 size 0x1000 → 0x1FFC hit, 0x2000 miss. |
| FE-6.1.3-02 | HAS §6.1.3 | When enabled regions overlap, the lowest-index hit supplies the normalization and containment operands. | MODEL | VP-CFG-04 | Reachable only through the bootstrap map, because commit rejects same-port overlap. |
| FE-6.1.3-03 | HAS §6.1.3, T7-13; MAS §6.1.3 | The context carries the region-relative normalized address A − B_r, which becomes the user-granule address passed to the backend. | MODEL | VP-MEM-05 | Aliasing consequence: GAP-FE-01, C3. |
| FE-6.1.3-04 | HAS §6.1.3; MAS T6-22 size_ok | If 2^SIZE is greater than the bus width in bytes, the response is SLVERR. | MODEL | VP-AXI-04 | |
| FE-6.1.3-05 | MAS T6-22 burst_ok | BURST=0b11 (reserved) is answered SLVERR. | MODEL | VP-AXI-14 | Allowed by HAS ("burst encoding" check). |
| FE-6.1.3-06 | HAS §6.1.3; MAS wrap_ok | A WRAP with LEN not in {1,3,7,15} is answered SLVERR. | MODEL | VP-AXI-15 | |
| FE-6.1.3-07 | HAS §6.1.3; MAS wrap_algn | A WRAP whose start is not aligned to beat_B is answered SLVERR. | MODEL | VP-AXI-16 | Golden: MAS T6-23 row 4. |
| FE-6.1.3-08 | HAS §6.1.3; MAS fixed_ok | A FIXED burst with LEN≠0 is answered SLVERR. | MODEL | VP-AXI-03 | |
| FE-6.1.3-09 | HAS §6.1.3; MAS cross_4k_ok | An INCR burst with (size-aligned start offset in its 4-KB page) + span_B > 4096 is answered SLVERR. A burst ending exactly on the page's last byte is legal. | MODEL | VP-AXI-05 | MAS: the check uses the AXI (global) address (GAP-FE-02). |
| FE-6.1.3-10 | HAS §6.1.3; MAS span_contained | Every byte of the burst (one beat for FIXED, the wrap container for WRAP, start to last beat for INCR) must lie inside the selected region with no overflow past 2^32. Otherwise SLVERR. A start-address hit alone is not enough. | MODEL | VP-AXI-06 | A burst ending on the region's last byte is legal. MAS uses 33-bit arithmetic. |
| FE-6.1.3-11 | HAS §6.1.3 T6-6, §6.1.13; MAS FE-DEC-08 | Error precedence: an unmapped start gives DECERR whatever else is wrong. A mapped but malformed request gives SLVERR. Otherwise OKAY. Never both. | MODEL | VP-AXI-08 | Test: unmapped + BURST=0b11 → DECERR. |
| FE-6.1.3-12 | HAS §6.1.3, §6.1.7 FE11 | An errored write drains its W data, gets one B with the error and issues no backend atom, so memory is unchanged. | MODEL | VP-AXI-07, VP-MEM-08 | |
| FE-6.1.3-13 | HAS §6.1.3, §6.1.7 FE11 | An errored read returns LEN+1 R beats with zero data, the error on every beat, and RLAST on the final beat. | MODEL | VP-AXI-07 | |
| FE-6.1.3-14 | HAS §6.1.3, T7-13; MAS T6-24 | AW/AR ready is gated by tag availability, scoreboard space, burst geometry (MAS ar_unroll_busy: at most one read per port with atoms still to issue), AW write-tag space (WTAG_DEPTH 8) and the admission block (QoS not configured). | APPROX | VP-AXI-20 | Capacity counters only. ar_unroll_busy is not modeled (timing only). |
| FE-6.1.3-15 | HAS §6.1.3 FE11 | An invalid configuration commit keeps the previous active image. | MODEL | VP-CFG-07 | See FE-6.1.15-02. |

### 2.4 Tags, scoreboard, ordering (HAS §6.1.4–6.1.5, §7.4.4–7.4.5)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.4-01 | HAS §6.1.4 T6-7; MAS T6-26 | Tag pool: AW takes the lowest free tag, AR the lowest free tag other than the AW candidate. Allocation is visible the next cycle, and tags are port-local. | ABSTRACTED | – | Tag identity is not visible at the boundary. Capacity is in FE-2.2-07. |
| FE-6.1.4-02 | HAS §6.1.4, §6.1.7 T6-13 | Only the terminal B/R enqueue frees a tag. WLAST, atom retirement and read-slot release do not. | APPROX | VP-AXI-20 | Context counter is released at response completion. |
| FE-6.1.4-03 | HAS §6.1.4, T7-14 | The context keeps direction, ID, normalized address, LEN, SIZE, BURST, error status, QoS class, exclusive intent and atom count. | MODEL | – | The VP transaction record holds the same fields (trace/introspection). |
| FE-6.1.4-04 | HAS §6.1.4 | All tags free (every accepted transaction has its terminal response) is a precondition for quiesce acknowledgement. | MODEL | VP-LIFE-02 | |
| FE-6.1.5-01 | HAS §6.1.5 T6-8; MAS T6-7 | A context is order-eligible only when no older live context with the same ID exists in the port. A successor issues no memory access until its predecessor has enqueued its terminal response. | MODEL | VP-ORD-01 | Observable: a write then a read with the same ID (different addresses or the same address), where the read returns the new data. |
| FE-6.1.5-02 | HAS T2-2; MAS §6.1.5 | A same-ID chain spans both directions (AW then AR with the same ID: the AR waits on the AW). A same-cycle AW+AR with the same ID: AR waits on AW. | MODEL | VP-ORD-02 | |
| FE-6.1.5-03 | HAS §6.1.5; §6.1.8 | Errored and no-atom contexts keep their same-ID order position and complete locally only in their turn. | MODEL | VP-ORD-03 | Test: SLVERR write between two OKAY writes with the same ID. B order is kept. |
| FE-6.1.5-04 | HAS §6.1.5 rules | Completions with different IDs may be reordered where AXI allows. MAS: the B register loads the lowest due tag with order_ok. | APPROX | VP-ORD-04 | Tests accept any order across IDs for B. R is per-port in order (FE-6.1.8-01). |
| FE-6.1.5-05 | HAS §6.1.5 T6-9; T7-15 | Per-context in-flight atom window K=4 (INFLT_W=3). | APPROX | – | Folded into the approximate latency per transaction. |

### 2.5 Write data path (HAS §6.1.6–6.1.7, §7.4.6–7.4.7)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.6-01 | HAS §6.1.6 T6-10 | Beat n address follows the INCR, WRAP or FIXED progression (beat 0 at the raw start, later INCR beats aligned). Byte offset = A_beat mod 32. Enabled bytes = WSTRB ∧ beat lanes. | MODEL | VP-MEM-01..03 | Golden: HAS example, 4-B beat at offset 4 → BE[7:4]. |
| FE-6.1.6-02 | MAS §6.1.6, T6-8 | Strobes outside the beat lanes are ignored. | MODEL | VP-MEM-06 | HAS silent, so MAS is adopted. |
| FE-6.1.6-03 | MAS T6-31, T6-32 | Bytes with WSTRB=0 are not modified. A granule with no enabled byte gets no backend write (masked ordinal), but the B still completes. | MODEL | VP-MEM-07 | Golden: MAS T6-32. |
| FE-6.1.6-04 | HAS §6.1.6, §6.1.9; MAS T6-39 | A WRAP that re-enters its start granule makes two visits (split). The visits cover disjoint bytes and are issued in beat order. | MODEL (data) / APPROX (visit count) | VP-MEM-03, VP-MEM-10 | Golden: MAS T6-41. |
| FE-6.1.6-05 | HAS §6.1.6 T6-11, §6.1.13; MAS T6-8 | Early or missing WLAST: the burst still ends at the AWLEN count, all beats are consumed, the data is written, and B = SLVERR. | ABSTRACTED | VP-DIAG-03 (optional) | Cannot be expressed in TLM. An optional injection hook is in GAP-FE-20. |
| FE-6.1.6-06 | MAS T6-8 | A W stream with a different burst length than AWLEN is not supported. | OUT | – | |
| FE-6.1.7-01 | HAS §6.1.7, T7-17 | Write store has 32 slots per port (WBUF_DEPTH). A full store backpressures W, while AW may proceed up to the write-tag queue. | APPROX | – | Optional counter. No functional effect. |
| FE-6.1.7-02 | HAS §6.1.7; MAS §6.1.7 cleanup | A late exclusive veto discards the buffered granules and restores credit with no slot leak. The B waits until no resident data remains. | ABSTRACTED | – | Only visible as capacity. The functional effect is in FE-6.1.12-04. |
| FE-6.1.7-03 | HAS §6.1.7 T6-13 | Storage-slot free and tag free are separate events. | ABSTRACTED | – | |

### 2.6 Read data path (HAS §6.1.8, §7.4.8)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.8-01 | HAS §6.1.8 ("local acceptance order"); MAS T6-7 | R data of one port leaves in AR acceptance order across all IDs, one transaction at a time. Errored reads also take their place in this order. | MODEL | VP-ORD-05 | Test: multiple initiator threads on one port. Completion order = acceptance order. |
| FE-6.1.8-02 | HAS §6.1.8 rules; T7-18 | 16 return slots (RBUF_DEPTH). When all are full, new read issue is blocked. | APPROX | – | Counter. Affects latency only. |
| FE-6.1.8-03 | HAS §6.1.8 T6-14, FE11/FE12; MAS §6.1.8 | Epoch-protected returns: a duplicate, stale-epoch or free-slot return is discarded without an AXI response and raises a one-cycle diagnostic (proposed sticky BAD_RETURN plus last slot). | ABSTRACTED | VP-DIAG-02 (optional) | No separate backend return channel in the VP. MAS maps it to ERROR_STATUS[2] and BAD_RETURN_LAST, which belong to the CSR audit. |
| FE-6.1.8-04 | HAS §6.1.8 T6-15 | One 32-B granule holds 2 chunks (AXI128) or 4 chunks (AXI64). Each beat takes its bytes from the chunk at the beat address, and narrow beats follow the descriptor. | MODEL | VP-MEM-02 | |
| FE-6.1.8-05 | HAS §6.1.8 | A read with no backend data (decode error) waits for its same-ID turn and completes locally without a slot. | MODEL | VP-ORD-03 | |
| FE-6.1.8-06 | MAS §6.1.8 T6-37, T6-58 | A refused read atom during a backend quarantine is answered SLVERR with zero data for that granule, in order. | MODEL | VP-MEM-12 | Depends on the backend quarantine model (backend audit). |
| FE-6.1.8-07 | HAS §6.1.13; MAS §6.1.13 | RRESP of a beat = worst of the decode verdict and the granule return status. Backend errors follow MAS T6-58 (mapping error or poison → DECERR; quarantine, fault or ECC abort → SLVERR). | MODEL | VP-MEM-12 | Whether the status is sticky or per-beat across later beats: C5 / GAP-FE-14. |
| FE-6.1.8-08 | HAS §6.1.8 rules | Read command-to-enable delay (T + effective_enable_delay + 1), PHY return window (BE14) and read timeout. | ABSTRACTED | – | DFI-level. Belongs to the backend audit. |

### 2.7 Atom planning (HAS §6.1.9, §7.4.9)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.9-01 | HAS §6.1.9 T6-16; MAS T6-39 | n_gran = distinct_granules + split. Split happens when the burst is WRAP, the start is not granule aligned, and span_B ≥ 64. The atom count is fixed at accept. | APPROX | VP-MEM-10 | Drives the approximate DRAM-burst count. Unit-test it against MAS T6-41. |
| FE-6.1.9-02 | HAS §6.1.9 rules | WRAP: base = floor(start/span)×span, next = base + ((cur − base + beat_B) mod span). | MODEL | VP-MEM-03 | Which address the formula uses (global or normalized): GAP-FE-02. |
| FE-6.1.9-03 | HAS T7-19 text; MAS §6.1.9 | Visit bound is ceil(span_B/32)+1. The longest native INCR has 128 atoms, and a 256-B WRAP has at most 9. | APPROX | VP-MEM-10 | |
| FE-6.1.9-04 | HAS §6.1.9; T7-19, T7-11 | Atom record fields (W_atom 104/102 bits, gtag 7, granule address 27, …) and issue in beat order. | ABSTRACTED | – | Internal record. |

### 2.8 QoS capture and offer selection (HAS §6.1.10, §7.4.10, T1-4)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.10-01 | HAS §6.1.10 T6-19 | class = LUT[raw_qos] is captured at acceptance. Later LUT writes affect only newly accepted requests. | MODEL | VP-QOS-03 | Observable through trace or an introspection hook. Golden: QoS 9 → class 2. |
| FE-6.1.10-02 | HAS FE05 (§2.2, §6.1.10); MAS T6-8, FE-DEC-13 | At reset all 16 entries are NORMAL and QOS_CONFIGURED=0. No AW/AR is accepted on that port until the table is enabled. | MODEL | VP-QOS-01 | This includes DECERR requests, which also stall. Semantics: GAP-FE-07. |
| FE-6.1.10-03 | HAS T1-4, T7-20 | Per-port sideband programs the LUT: write enable 1, index 4, class 2, plus cfg write and cfg value (1 each). The configured flag is an output. | MODEL | VP-QOS-02 | Sideband, not CSR (VP decision; QOS_LUT is absent from the HAS CSR map). |
| FE-6.1.10-04 | MAS §6.1.10, T6-64 | A port soft reset resets that port's LUT to NORMAL and configured to 0. Software must reprogram before admission. | MODEL | VP-LIFE-06 | HAS silent, so MAS is adopted. |
| FE-6.1.10-05 | HAS §6.1.10; MAS emit select | Offer selection: eligibility mask plus a rotating pointer. A refused offer is held stable and uses no credit. | ABSTRACTED | – | Intra-port scheduling. The same-ID rule is kept by FE-6.1.5-01. |
| FE-6.1.10-06 | HAS §6.1.10, §6.1.11 FE05 | QoS, reserved-rate and aging create no fixed end-to-end latency guarantee. | ABSTRACTED | – | Nothing to test. |

### 2.9 Inter-port arbitration (HAS §6.1.11, §7.4.11)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.11-01 | HAS §6.1.11 T6-20/21; MAS §6.1.11 | Selection order: reserved floor (credit ≥ token_cost 1024), then the highest effective class (aging adds +1 once the age reaches ARB_AGE_THRESHOLD), then round robin. At most one take per cycle. | ABSTRACTED | – | No cycle arbiter. The VP may serve ports in arrival order, and class is recorded only. See GAP-FE-23. |
| FE-6.1.11-02 | HAS T1-4, T7-21; MAS T6-50 | Reserved-rate sideband: write enable, port index (2), rate (20), depth (20). MAS: a depth of 1–1023 is illegal. | APPROX | VP-QOS-05 | The VP stores and traces the values but does not shape timing. |
| FE-6.1.11-03 | HAS §6.1.11 | The exclusive-verdict stage may veto one selected exclusive write, which then completes locally with no backend take. | MODEL | VP-EXCL-03 | Functional effect only. |

### 2.10 Exclusive access (HAS §6.1.12, §7.4.12; decision: HAS 32-B granule)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.12-01 | HAS §6.1.12, T2-2 | An exclusive read on an enabled port creates or refreshes a reservation {port, AXI ID, 32-B granule, byte coverage}. | MODEL | VP-EXCL-01 | Granule = normalized address / 32. Multi-granule access: GAP-FE-03. |
| FE-6.1.12-02 | HAS §6.1.13 T6-24 (exclusive_success) | An exclusive read that creates a reservation returns EXOKAY on its R beats, unless DECERR or SLVERR applies. | MODEL | VP-EXCL-01 | See C12 for the shape-fail case. |
| FE-6.1.12-03 | HAS §6.1.12 T6-22 | An exclusive write whose port, ID, granule and byte enables match a live reservation updates memory and returns B=EXOKAY. | MODEL | VP-EXCL-02 | The byte-enable basis is defined in GAP-FE-24. |
| FE-6.1.12-04 | HAS §6.1.12, §6.1.6, T2-2 | An exclusive write miss consumes its W data, writes nothing to memory, and returns B=OKAY after WLAST and in its same-ID turn. | MODEL | VP-EXCL-03 | Golden: HAS example ID 3 / ID 4 at granule 0x40. |
| FE-6.1.12-05 | HAS §6.1.12; MAS T6-56 | A normal write (from any port) that touches a reserved granule clears the matching reservations. | MODEL | VP-EXCL-04 | MAS: any taken write atom with blk_hit. Suppressed (errored) writes and fully-unstrobed granules issue no atom and do not clear (MAS). |
| FE-6.1.12-06 | MAS T6-55 | A passing exclusive write also clears the reservation, so a second exclusive write without a new exclusive read fails (OKAY). | MODEL | VP-EXCL-05 | HAS silent, so MAS is adopted. |
| FE-6.1.12-07 | HAS §6.1.12; MAS T6-56 | On a port outside the mask, an access with the exclusive bit is treated as ordinary: no reservation, never EXOKAY, the write updates memory and returns OKAY, the read returns OKAY. | MODEL | VP-EXCL-06 | |
| FE-6.1.12-08 | HAS §6.1.12 rules; MAS §6.1.12 table | There are 4 entries with one entry per owner {port, ID}. Allocation order: the owner's own entry, else the lowest invalid entry, else the victim (round robin mod 4). Entries are never chosen by address. | MODEL | VP-EXCL-07 | Golden: MAS T6-54. HAS gives only the count, so the policy is adopted from MAS. |
| FE-6.1.12-09 | HAS T6-22 | An exclusive write with a different ID, different granule or different byte coverage than the reservation fails (OKAY, no update). | MODEL | VP-EXCL-08 | |
| FE-6.1.12-10 | HAS §6.1.12, §6.1.14; MAS §6.1.12 | A quiesce request on port p (also MAS: a power-stop ack) invalidates p's reservations only. Reservations owned by p hold off p's quiesce ack. | MODEL | VP-EXCL-09, VP-LIFE-04 | Sequence: exclusive read, quiesce, release, exclusive write → OKAY. |
| FE-6.1.12-11 | HAS §6.1.13; MAS §6.1.3 | An exclusive request with a DECERR or SLVERR verdict returns that error, creates no reservation and clears none. | MODEL | VP-EXCL-10 | |
| FE-6.1.12-12 | HAS §6.1.12 rules | Exclusive semantics stop at 32-B granularity. No ACE coherency, no ATOP. | OUT | – | |
| FE-6.1.12-13 | HAS §6.1.12; MAS T6-52 | There is a single registered verdict stage, and the exclusive write is judged at its first emitted atom (whole take or whole fail). | ABSTRACTED | – | VP verdict point = when the write's W data is complete and its same-ID turn arrives. |

### 2.11 Response construction (HAS §6.1.13, §7.4.13)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.13-01 | HAS §6.1.13 T6-24; MAS FE-RSP-07 | Status merge: DECERR > SLVERR > EXOKAY > OKAY. A recorded error is never lowered by a later success. | MODEL | VP-AXI-09 | Golden: HAS example, a packing SLVERR survives a good completion. |
| FE-6.1.13-02 | HAS §6.1.13 T6-25 | A normal B is issued only after WLAST, all granules drained, and the backend's response-safe completion, so the data is observable when B arrives. | MODEL | VP-MEM-09 | Test: read from another port after B returns the new data. |
| FE-6.1.13-03 | HAS §6.1.13; MAS FE-RSP-06 | Exactly one B per accepted AW and exactly LEN+1 R beats per accepted AR, errors included. | MODEL | VP-AXI-07 | |
| FE-6.1.13-04 | HAS §6.1.13, T7-23 | BID and RID equal the AxID of the originating context. | MODEL | VP-AXI-13 | |
| FE-6.1.13-05 | HAS T7-23 (late service error); MAS T6-58 | A late backend error (ECC, parity or service) is merged into the response: SLVERR, or DECERR for a mapping error or poisoned location. | MODEL | VP-MEM-12 | Depends on the backend ECC model. |
| FE-6.1.13-06 | MAS §6.1.13 | A locally completed write cannot answer EXOKAY, and the backend never produces EXOKAY. | MODEL | VP-EXCL-03 | |
| FE-6.1.13-07 | HAS §6.1.13 rules | No bounded response latency under backpressure. | ABSTRACTED | – | |

### 2.12 Port lifecycle (HAS §6.1.14, §7.4.14)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.14-01 | HAS §6.1.14 | A quiesce request blocks new AW/AR. Accepted W data, backend work and B/R delivery continue to their terminal events. | MODEL | VP-LIFE-01 | |
| FE-6.1.14-02 | HAS §6.1.14 T6-26; MAS T6-61 | quiesce_ack = request ∧ all ownership terms clear (no live tag, no store data, no read slot, no held offer, no pending response, no atom credit, no owned reservation) ∧ backend_drained. | MODEL | VP-LIFE-02 | Golden: HAS example (one live read means no ack). |
| FE-6.1.14-03 | HAS §6.1.14; MAS T6-66 | A soft reset starts only after the quiesce ack. A request without the ack waits and does not start. | MODEL | VP-LIFE-03 | Command-level behavior: GAP-FE-13. |
| FE-6.1.14-04 | HAS §6.1.14 T6-27; MAS T6-66 | The reset is held for at least 4 × SYNC_STAGES = 8 core cycles. Dropping the request early does not shorten the hold. | APPROX | VP-LIFE-05 | VP: 8 × core clock period parameter. |
| FE-6.1.14-05 | MAS T6-19, T6-66 | No AW, W or AR is accepted during the hold. A request presented during the hold is accepted after release, and is then subject to the QoS gate. | MODEL | VP-LIFE-06 | |
| FE-6.1.14-06 | HAS §6.1.14 | Lifecycle is per port. It does not flush shared backend state, other ports, or other ports' reservations. | MODEL | VP-LIFE-07 | |
| FE-6.1.14-07 | HAS FE10 (§6.1.14, T7-24) | The orderly quiesce waits indefinitely. Timeout status is always 0 and is never a successful ack. | MODEL | VP-LIFE-09 | MAS's FE_QUIESCE_TIMEOUT is not adopted (C6). |
| FE-6.1.14-08 | HAS §6.1.14; MAS T6-64 | Releasing the quiesce reopens admission (the QoS gate still applies). | MODEL | VP-LIFE-10 | Release op encoding: GAP-FE-13. |
| FE-6.1.14-09 | HAS T8-8 (cross-ref) | FE_PORT_STATE reflects CSR quiesce, effective quiesce, ack, reset active, timeout, reset pending and backend drained for each port. | MODEL | VP-CFG-15 | Field positions are in HAS T8-8. The CSR audit owns the details. |

### 2.13 Map publication (HAS §6.1.15, §7.4.15, T8-10)

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-6.1.15-01 | HAS §6.1.15; MAS T6-68 step 1 | Shadow writes never change the effective map. | MODEL | VP-CFG-05 | |
| FE-6.1.15-02 | HAS §6.1.15 rules, T8-10; MAS T6-68 | A commit is rejected, and the prior active image is kept with error and blocker status, when any of these hold: same-port overlap among enabled regions, BASE+SIZE > 2^32, BASE not a multiple of 32, SIZE not a multiple of 32 (MAS), or region end beyond USABLE_GRANULES (MAS). | MODEL | VP-CFG-06, VP-CFG-07 | MAS blocker codes: 0x0B (crossing not ready), 0x0C (alignment, overflow or overlap), 0x0D (beyond USABLE_GRANULES). See C8 and GAP-FE-10/11. |
| FE-6.1.15-03 | HAS §6.1.15; MAS T6-68 | An accepted commit raises the frontend quiesce on all ports. No new AW/AR enters, and accepted work drains. Install happens only when every port has acked (MAS also requires global quiesce). | MODEL | VP-CFG-08 | Waits indefinitely if a port cannot drain (GAP-FE-12). |
| FE-6.1.15-04 | HAS §6.1.15 T6-28 | Install is atomic over the full 4×4 image. Decode sees the old complete image or the new complete image, never a mix. | MODEL | VP-CFG-09 | Golden: HAS example (one changed size). |
| FE-6.1.15-05 | HAS §6.1.15 rules | Accepted contexts keep their captured normalized address and status across an install. | MODEL | VP-CFG-10 | Naturally true if quiesce precedes install. Test with a held write. |
| FE-6.1.15-06 | HAS §6.1.15 T6-29; MAS FE-TOP-12 | effective_map = installed ? active : bootstrap. After a core reset, installed=0, so the bootstrap map is used even if software published before; software must publish again. | MODEL | VP-CFG-11 | CSR readback difference: GAP-FE-09. |
| FE-6.1.15-07 | MAS §6.1.15; HAS T8-10 "same-port overlap" | Regions of different ports may overlap. | MODEL | VP-CFG-12 | |
| FE-6.1.15-08 | HAS T8-10; MAS T6-68 step 6 | After install, FE_REGION_ACTIVE shows the new image, FE_REGION_STATUS done[1]=1 and active valid[16]=1, and the publication quiesce is released. While the candidate is retained, busy[0]=1. On failure, error[2]=1 and blocker[15:8] is set. | MODEL | VP-CFG-13 | Field positions are from HAS T8-10. |
| FE-6.1.15-09 | HAS T7-25, T8-10 | The global image is 1024 bits (4 ports × 4 regions × BASE/SIZE). CSR word index i = 2*(4p+r)+k, with k=0 for BASE and k=1 for SIZE. | MODEL | VP-CFG-14 | |
| FE-6.1.15-10 | HAS T7-25, §6.1.15 rules | Commit cancelled is a defined event, but its cancellation timing is integration behavior. | MODEL (partial) | – | GAP-FE-10. |
| FE-6.1.15-11 | HAS T8-10 | Core offline clears the active mirror and requires a fresh commit. | MODEL | VP-LIFE-11 | Consistent with FE-6.1.15-06. |

### 2.14 Interfaces (HAS §7.4) not already covered

| Req ID | Source | Requirement | VP class | Test | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| FE-7.4-01 | HAS T7-11 | AXI payload widths W_ax=61, W_w=145/73, W_b=6, W_r=135/71. | ABSTRACTED | – | TLM extension carries the fields, not packed vectors. |
| FE-7.4-02 | HAS T7-14…T7-23 | Internal bundles (W_ctx 80/78, W_atom 104/102, W_plan 117/114, retirement, completion, fetch, read return 271, credit ledger). | ABSTRACTED | – | Internal microarchitecture. |
| FE-7.4-03 | HAS T7-24 | Per-port signals: quiesce request and ack, soft-reset request, one reset-active qualifier, timeout status (always 0), per-port backend drained. | MODEL | VP-LIFE-* | sc_signal or callback ports on the VP top. |
| FE-7.4-04 | HAS T7-25 | Map endpoint events: commit, ready, accepted, done, cancelled, busy, plus status (1 success + 8 blocker) and publication quiesce. | MODEL | VP-CFG-13 | Exposed through FE_REGION_STATUS. |
| FE-7.4-05 | HAS T7-22 | The exclusive failure identity is port + local tag + AXI ID. Reservation invalidation is per port. | ABSTRACTED | – | Internal. The functional effect is in §2.10. |

---

## 3. HAS vs MAS conflicts

| # | Item | HAS says (cite) | MAS says (cite) | Resolution |
| :--- | :--- | :--- | :--- | :--- |
| C1 | Exclusive reservation size | 32-byte granule. The match needs same port, ID, granule and byte enable (§6.1.12, T6-22, T2-2). | EXCL_GRANULE_B=128. The window covers up to 4 granules, from excl_g0 with a 128-bit mask and shape_ok (T6-52, T6-53). | **HAS wins** (VP decision): one 32-B granule. Multi-granule exclusives → GAP-FE-03. |
| C2 | Reservation clearing | "A normal write to the same granule invalidates matching reservations" (§6.1.12). Nothing on passing exclusive writes, same-owner re-reads, or shape failures. | Any taken write atom with blk_hit clears it, including a passing exclusive write. A same-owner re-read elsewhere reuses the entry. A shape-fail read clears the owner's entry. An invalidate fires on quiesce or power-stop (§6.1.12 table, T6-56). | HAS silent on these details, so **MAS is adopted [MAS-adopted]**. The granule test uses 32 B. |
| C3 | Granule address semantics | T6-17: "physical granule address". T7-13: "region-relative normalized address". §7.5: "user-granule address". | "region-normalized, not physical" (§6.1.9 atom form). | HAS is internally inconsistent. **Adopt the normalized user-granule address** (HAS T7-13/§7.5 + MAS). Backend topology maps it to a physical address. Aliasing → GAP-FE-01. |
| C4 | Decode map after reset | Bootstrap map is effective until the first install (§6.1.15 T6-29). | §6.1.3: "After reset all sizes are 0, so every request is DECERR until the first region commit". §6.1.15 agrees with HAS (straps). | **HAS wins**: the bootstrap map is effective. The MAS §6.1.3 sentence holds only for all-zero straps. |
| C5 | RRESP across beats with a backend error | One status ledger per context, and a later status cannot hide an earlier error (§6.1.13). | RRESP = worse of (intent, status of *this* beat), i.e. per beat (§6.1.13 response code). | HAS wins in principle, but it does not say "per beat". **Proposed**: sticky-forward (GAP-FE-14). Needs architect confirmation. |
| C6 | Quiesce timeout | FE10: indefinite wait, timeout status is zero (§6.1.14, T7-24). FE_QUIESCE_TIMEOUT is not in the HAS CSR map. | Programmable FE_QUIESCE_TIMEOUT counter, default 0 = off (§6.1.14). | **HAS wins**: no timeout register, and the status reads 0. |
| C7 | Does quiesce block decode? | Quiesce "blocks new AW and AR acceptance" (§6.1.14). Ready includes the "active quiesce block" (§6.1.3). | FE-DEC-13: quiesce does not block decode. The stop acts at the AXI side of the CDC, AW/AR already in the FIFOs are accepted, and i_quiesce_block = NOT qos_configured. | No conflict at the AXI boundary. **VP**: block arrivals after the quiesce request time. Arrivals already admitted complete (GAP-FE-05). |
| C8 | Commit overlap and alignment checks | §6.1.15: rejects "overlapping enabled shadow regions", overflow and non-granule-aligned **bases**. T8-10: "same-port overlap". | Only same-port overlap. Regions of different ports may overlap. BASE **and SIZE** must be multiples of 32, plus a USABLE_GRANULES check (T6-68). | **HAS T8-10 + MAS**: same-port overlap only. The SIZE alignment and USABLE_GRANULES checks are **MAS-adopted** (HAS silent). |
| C9 | AR context capacity | 32 contexts per port. The AR candidate excludes the AW candidate; the example covers only the case where both request (§6.1.4, T2-2). | aw_tag is computed every cycle, so an AR needs 2 free tags and 2 free scoreboard entries. ar_unroll_busy allows only one read with atoms pending (§6.1.3–6.1.5). | **HAS wins**: the VP models 32 shared contexts. Tests must not assert the exact 31/32 boundary or the unroll limit (timing only). |
| C10 | Tag return point | At terminal response *enqueue* into the core-to-AXI queue (§6.1.1, §6.1.4). | "at its B handshake / RLAST handshake" (§6.1.4, T6-59), on the core side into the CDC FIFO. | Equivalent. VP releases the context at response completion. |
| C11 | QoS "configured" semantics | FE05: block admission "until firmware writes all 16 entries and enables the completed table" (§2.2, §6.1.10). Sideband (T1-4). | configured_q follows the QOS_CFG write. No hardware check that all entries were written. CSR copies QOS_LUT[p] reset 0x55555555 and QOS_CFG[p] (§6.1.10, T6-64). | **HAS (sideband)** per the VP decision. The all-16 rule is treated as a firmware obligation (GAP-FE-07). MAS CSR names are not adopted. |
| C12 | EXOKAY on an exclusive read that creates no reservation | exclusive_success = "an enabled exclusive access met its reservation condition" (T6-24). | RRESP intent is EXOKAY whenever AxLOCK ∧ EXCL_ENABLE ∧ mask[port], even when shape_ok=0 creates no entry. | **HAS wins**: EXOKAY only if the reservation was created. Otherwise OKAY. |
| C13 | Exclusive byte-enable basis | "same_byte_enable" (T6-22). | Atom byte_enable comes from the address plan (first_be/last_be), not WSTRB (T6-40, T6-42). | HAS is ambiguous, so **adopt MAS**: coverage is derived from address, SIZE and LEN. WSTRB is not part of the match (GAP-FE-24). |
| C14 | Soft reset effect on QoS | Silent. | The LUT resets to NORMAL and configured to 0. Software reprograms (T6-64). | **MAS-adopted**. |
| C15 | FE_PORT_CMD op encodings | Field op[2:0], with no encodings given (T8-8). | op 1 = quiesce, op 3 = soft reset (T6-64). | **MAS-adopted** for 1 and 3. Release and other ops → GAP-FE-13. |
| C16 | Failed commit bus response | Prior image is kept and the status reports it (FE11, T8-10). A §8 busy collision returns an access error. | "A failed check answers SLVERR" on the AXI4-Lite write (T6-68). | HAS silent on the validation failure response, so **MAS-adopted**: SLVERR plus status error and blocker. |
| C17 | WRAP container and 4-KB basis | Formula uses "start_address" (§6.1.9), unqualified. | The 4-KB check uses the AXI address. The planner uses the normalized address (§6.1.3, T6-39). | Undefined when B_r is not span- or 4-KB-aligned → GAP-FE-02. |

---

## 4. Gaps / open issues

Each item below is undefined by both documents and affects FW-visible or test-visible behavior. Every "VP default" is a **PROPOSAL** for review, not a spec fact.

- **GAP-FE-01: Normalized-address aliasing.** The backend sees only the region-relative user granule (A − B_r)/32 (27 bits). There is no region index or offset field. So region 0 and region 1 of one port, and regions of different ports, all map onto user offset 0 and alias each other. *Proposal*: the VP backing store is indexed by A − B_r and models the aliasing faithfully. Add VP-MEM-05: write through port0/region0, then read the same offset through region1 or port1 and see the same data. Raise to the architect: confirm the aliasing is intended, or define a per-region DRAM offset.
- **GAP-FE-02: WRAP container and 4-KB basis when the region base is not aligned.** Region bases need only 32-B alignment. A WRAP of up to 256 B, or the 4-KB check, gives different results on the global address and on the normalized address. *Proposal*: run the legality checks (4-KB, wrap_algn, WRAP container) on the **global** AXI address (AXI semantics, and MAS states this for 4 KB). Place data at progression_address − B_r. Raise an SC_WARNING at commit or bootstrap when an enabled B_r is not 4-KB aligned. Tests use 4-KB-aligned bases, except one dedicated divergence test.
- **GAP-FE-03: Exclusive accesses not inside one 32-B granule.** This covers size > 32 B, a span crossing a granule, a non-power-of-2 length, or an unaligned access. *Proposal*: such an exclusive read creates no reservation, clears the owner's existing entry (MAS shape-fail rule) and returns OKAY (C12). Such an exclusive write fails: OKAY and no memory update. An exclusive write with all WSTRB=0 inside a matching reservation returns EXOKAY with no data change.
- **GAP-FE-04: TLM behavior while admission is closed.** Admission is closed when QoS is not configured, during quiesce, during publication quiesce, during the soft-reset hold, or when contexts are full. *Proposal*: `b_transport` blocks on an admission event with no timeout and never returns an error (AXI stall semantics). An optional `admission_watchdog` parameter issues SC_WARNING after a configured time. This includes DECERR requests (C7, FE-6.1.10-02).
- **GAP-FE-05: Admission instant.** "Already in the CDC FIFO at stop" has no TLM meaning. *Proposal*: a transaction is admitted iff admission is open when it enters the target (sim time, then call order). A quiesce or commit request takes effect at its own sim time. Anything admitted earlier drains to completion.
- **GAP-FE-06: Acceptance order for concurrent initiators.** *Proposal*: acceptance order is the order of target entry (time, then delta, then call order). If an AW and an AR with the same ID enter in the same delta, the AW is ordered first (HAS T2-2).
- **GAP-FE-07: QOS_CONFIGURED details.** It is unclear whether hardware checks that all 16 entries were written, whether writing cfg=0 re-blocks admission, and what a LUT write does while configured=1. *Proposal*: configured follows the cfg value (MAS), and cfg=0 re-blocks new admission only. There is no all-16 hardware check, but the VP raises SC_WARNING if configured is set before all 16 indices were written since reset. A LUT write applies only to later acceptances. Class encoding is **inferred** from MAS 0x55555555 = all NORMAL: BULK=0, NORMAL=1, LATENCY=2, HRT=3. This is unconfirmed.
- **GAP-FE-08: Who programs QoS for platform port 0.** No CSR exists, and FE05 blocks all traffic until the table is configured, so CPU boot would hang. *Proposal*: add VP parameters `qos_lut_init[p]` and `qos_configured_at_reset[p]` (default false, per FE05). The platform sets port 0 to true. Conformance tests keep the default.
- **GAP-FE-09: Bootstrap map vs FE_REGION_ACTIVE readback.** The VP decision says the bootstrap map is "loaded as initial active image". HAS gives an FE_REGION_ACTIVE reset value of 0x00000000 and active valid[16]=0 until the first install, while the effective map is the bootstrap. *Proposal*: decode uses the bootstrap map. FE_REGION_ACTIVE reads 0 and STATUS[16]=0 until the first successful commit. The decision-log wording needs to change. The bootstrap image is not validated: overlap is resolved by lowest index, with SC_WARNING on overlap or misalignment.
- **GAP-FE-10: Blocker codes and cancellation.** Only 0x0B, 0x0C and 0x0D are defined (MAS). The codes for busy collision, cancellation and endpoint failure, and the conditions for cancellation, are undefined. *Proposal*: a commit while busy returns AXI4-Lite SLVERR and leaves the status unchanged (HAS §8 "busy collision returns an access error"). A core reset while a commit is pending → cancelled, prior image kept, blocker 0xFF (placeholder). No other cancel source exists in the VP.
- **GAP-FE-11: USABLE_GRANULES check basis.** "Region end within USABLE_GRANULES" could mean BASE+SIZE (global) or SIZE (normalized). *Proposal*: check SIZE/32 ≤ USABLE_GRANULES, which matches normalized addressing. Because USABLE_GRANULES is 0 after reset (MAS), every enabled-region commit fails until a topology commit, so tests must commit the topology first.
- **GAP-FE-12: A commit that cannot drain.** For example, a master never sends its owed W. *Proposal*: wait indefinitely (FE10 spirit), with FE_REGION_STATUS.busy=1. There is no automatic cancel.
- **GAP-FE-13: FE_PORT_CMD semantics.** The release-quiesce op, ops 0/2/4–7, the response when soft reset is requested without an ack, and the blocker values are all undefined (MAS ch. 8 missing). *Proposal*: op 2 = release quiesce, as a **placeholder**. An illegal op returns SLVERR with error=1. A soft-reset command without an ack waits for the ack (MAS block behavior), and the AXI4-Lite response is withheld until the endpoint result arrives (HAS §8). Flag this as a firmware-deadlock risk.
- **GAP-FE-14: RRESP stickiness across beats** (C5). *Proposal*: sticky-forward. Once a granule's error status is presented, that beat and all later beats of the same read carry at least that severity. Earlier beats keep their own status.
- **GAP-FE-15: TLM data layout for bursts.** *Proposal*: use the bus-lane layout. `data_length` = (LEN+1)×beat_bytes(bus). Beat n occupies [n×beat_bytes, (n+1)×beat_bytes) with each byte at lane (A_beat mod beat_bytes). `byte_enable` = WSTRB per beat (same length). For reads, unused lanes return 0. This carries narrow, unaligned and WRAP data exactly.
- **GAP-FE-16: Generic 32-bit bus adapter (port 0).** *Proposal*: ID=0 fixed, which serializes all platform traffic per FE-6.1.5-01. Use INCR with SIZE = min(log2 of the access width, 2) for the 32-bit bus. Split at 4-KB boundaries and at 256 beats. QoS is a parameter, lock=0, and CACHE/PROT are passed through. Response mapping: OKAY → TLM_OK, SLVERR → TLM_GENERIC_ERROR_RESPONSE, DECERR → TLM_ADDRESS_ERROR_RESPONSE.
- **GAP-FE-17: DMI and debug transport.** *Proposal*: `transport_dbg` follows the effective map (an unmapped access transfers 0 bytes). It bypasses admission, ordering, timing and the exclusive monitor, and changes no reservations. DMI is off in conformance mode. In platform mode DMI is granted per enabled region and invalidated on map install, port soft reset, core reset, and whenever port admission closes.
- **GAP-FE-18: Out-of-range extension values.** ID > 15, address ≥ 2^32, LEN > 255, or a `data_length` that disagrees with LEN/SIZE → SC_REPORT_ERROR (a model-usage error, not an AXI response). BURST=0b11 → SLVERR (spec'd).
- **GAP-FE-19: Generic payload without an AXI extension on a direct port.** *Proposal*: SC_REPORT_ERROR unless `allow_plain_gp=true`. When allowed, treat it as GP-adapter defaults (GAP-FE-16).
- **GAP-FE-20: Early or missing WLAST cannot be expressed in TLM.** *Proposal*: a test-only extension flag `wlast_error`. When set, B=SLVERR and the data is written per AWLEN (MAS T6-8).
- **GAP-FE-21: In-flight TLM calls at emergency or core reset** (FE10: "no response promised"). *Proposal*: blocked or in-flight calls return TLM_GENERIC_ERROR_RESPONSE with AXI resp SLVERR plus SC_WARNING "aborted by reset". Writes are applied atomically per transaction (all or nothing). Tests must not rely on this.
- **GAP-FE-22: Frontend admission before DRAM init, training or scrub completes.** §6.1 frontend gates do not include init (FE03/FE10 gate the backend). *Proposal*: the frontend accepts once QoS is configured. Errored requests complete locally. Legal requests stall in the backend until init, training and scrub are done.
- **GAP-FE-23: Use of class and reserved-rate in VP timing.** *Proposal*: the VP ignores floor and aging. The captured class may optionally order the backend queue (`use_qos_priority`, default false). No test asserts cross-port service order.
- **GAP-FE-24: Exclusive byte-coverage basis** (C13). *Proposal*: the read's coverage and the write's coverage are both derived from address, SIZE and LEN (planner first_be/last_be). The write matches if its coverage equals the reservation's. WSTRB is ignored for the match and applied to the data.

---

## 5. Worked examples for golden tests

All values are copied from the documents unless marked *derived*.

**E1. MAS Table 6-23 decode**: region 0 = [0x8000_0000, size 1 GiB), region 1 = [0xC000_0000, size 256 bytes), AXI128.

| Request | Region | Failing check | Verdict | Normalized address |
| :--- | :--- | :--- | :--- | :--- |
| INCR 0x8000_0100, len 15, 16-byte beats | 0 | - | OKAY | 0x100 |
| INCR 0x7000_0000, len 0, 16-byte beats | - | - | DECERR | - |
| INCR 0x8000_0FC0, len 7, 16-byte beats | 0 | cross_4k_ok | SLVERR | 0xFC0 |
| WRAP 0x8000_0008, len 3, 16-byte beats | 0 | wrap_algn | SLVERR | 0x8 |
| INCR 0xC000_00F0, len 1, 16-byte beats | 1 | span_contained | SLVERR | 0xF0 |
| INCR 0x8000_0000, len 0, 32-byte beat | 0 | size_ok | SLVERR | 0x0 |
| FIXED 0x8000_0040, len 3, 16-byte beats | 0 | fixed_ok | SLVERR | 0x40 |

VP-visible checks: the response codes, R beat count = len+1 with zero data for errors, and no memory change for errored writes. The normalized address is visible only through VP-MEM placement (for example, the OKAY row writes user offset 0x100).

**E2. HAS §6.1.3**: base 0x1000, size 0x1000. Address 0x1FFC is mapped and 0x2000 is not. A 16-beat 8-byte INCR has span_B = 128 bytes.

**E3. MAS Table 6-41 atom plan** (normalized address input). Used by the VP-MEM-10 granule-count unit test.

| Burst | span_b | base_addr | last_byte | distinct | split | atom_count | Case | first_be | last_be |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| INCR 0x104, 4-byte beats, len 15 | 64 | 0x104 | 0x143 | 3 | no | 3 | 4 | [31:4] | [3:0] |
| WRAP 0x130, 8-byte beats, len 7 | 64 | 0x100 | 0x13F | 2 | yes | 3 | 1 | [31:16] | [15:0] |
| WRAP 0x120, 8-byte beats, len 7 | 64 | 0x100 | 0x13F | 2 | no | 2 | 2 | all | all |
| INCR 0x000, 16-byte beats, len 255 | 4096 | 0x0 | 0xFFF | 128 | no | 128 | 4 | [31:0] | [31:0] |
| WRAP 0x110, 16-byte beats, len 15 | 256 | 0x100 | 0x1FF | 8 | yes | 9 | 1 | [31:16] | [15:0] |
| INCR 0x108, 4-byte beats, len 1 | 8 | 0x108 | 0x10F | 1 | no | 1 | 3 | [15:8] | [15:8] |

**E4. WRAP beat walk** (*derived* from the HAS §6.1.9 formula, for the T6-41 row "WRAP 0x130, 8-byte beats, len 7"): container 0x100–0x13F, beat addresses 0x130, 0x138, 0x100, 0x108, 0x110, 0x118, 0x120, 0x128. Golden data test: write pattern beat n = n, then read back with an INCR at 0x100. Expected bytes per 8-B word at 0x100…0x138 = beats 2,3,4,5,6,7,0,1.

**E5. MAS Table 6-32 packing**: INCR at 0x10, 16-byte beats, len 3.

| Beat | Address | Granule bytes | Granule | WSTRB | Action |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 0 | 0x10 | [31:16] | 0 | 0xFFFF | store granule 0 |
| 1 | 0x20 | [15:0] | 1 | 0xFFFF | merge |
| 2 | 0x30 | [31:16] | 1 | 0x00FF | store granule 1 |
| 3 | 0x40 | [15:0] | 2 | 0x0000 | masked ordinal 2 (no write) |

VP check: bytes 0x10–0x37 are updated, 0x38–0x3F and 0x40–0x4F are unchanged, and B=OKAY.

**E6. HAS §6.1.6**: a 4-byte beat starting 4 bytes into a granule has offset 4, gran_idx_rel 0, and BE bits 4–7.

**E7. MAS Table 6-36**: a 128-B read (4 granules, 2 beats each, slots 0–3) with returns in slot order 2, 0, 3, 1. R beats 0–7 still leave in order, and RLAST is on beat 7. In the VP this is golden only if a backend return-reordering stub exists. Otherwise the check is "data in address order".

**E8. Exclusive walk (MAS Table 6-55, port 0)**: XR taken and entry 0 written. XW1 to the same bytes → owner match → taken → **EXOKAY**, and entry 0 is cleared. XW2 → no match → **OKAY**, no memory update.

**E9. HAS §6.1.12**: a reservation for ID 3 at granule 0x40. An exclusive write by ID 3 → EXOKAY. A write by ID 4 at 0x40 misses → OKAY, local completion, no update.

**E10. MAS Table 6-54, allocation with 4 entries** (32-B granule per HAS; the "first granule" values are reused as granule indices):

| Read | Owner {port, ID} | First granule | Entry used | victim_q after |
| :--- | :--- | :--- | :--- | :--- |
| XR0 | {0, 0} | 0x100 | 0 | 0 |
| XR1 | {0, 1} | 0x100 | 1 | 0 |
| XR2 | {0, 0} | 0x200 | 0 (reuse) | 0 |
| XR3 | {0, 2} | 0x300 | 2 | 0 |
| XR4 | {0, 3} | 0x400 | 3 | 0 |
| XR5 | {0, 4} | 0x500 | 0 (replace) | 1 |

VP check: after XR5, an exclusive write by {0,0} at 0x200 → OKAY (evicted), and {0,1} at 0x100 → EXOKAY.

**E11. HAS §6.1.13**: a SLVERR recorded during write packing is not replaced by a later successful backend completion, so the final B is SLVERR.

**E12. HAS §6.1.10**: the LUT maps QoS 9 → class 2. A request accepted then keeps class 2 after software changes the table.

**E13. Soft reset (HAS §6.1.14, MAS T6-64/T6-65)**: with SYNC_STAGES=2 the minimum hold is 8 core cycles. MAS events: the request is high from cycles 0–2 with no ack, so no start. The ack goes high at 3 and the reset starts. At cycle 4 active_q=1 and both port resets are asserted. The request is dropped at 5. At 11 hold_cnt_q=7 and the reset releases. At 12 active_q=0 (8 cycles in reset).

Software sequence (MAS T6-64): FE_PORT_CMD op 1 (quiesce) → wait for success → FE_PORT_CMD op 3 (soft reset) → reprogram the QoS LUT, then the configured flag (sideband in the VP) → release the quiesce. VP check: an AXI request between the soft reset and the QoS reprogram stays blocked.

**E14. Map commit (MAS T6-68 + HAS §6.1.15)**: write the shadow (32 words, i = 2*(4p+r)+k), then FE_REGION_COMMIT[0]=1. The validation blockers are 0x0B, 0x0C and 0x0D; a failure returns SLVERR and keeps the prior image. On accept: region quiesce on all ports, drain, install when all ports ack, FE_REGION_ACTIVE updated, STATUS[16]=1, quiesce released. HAS example: a four-region candidate with one changed size is installed as four base/size pairs on one event. After a core reset, decode uses the bootstrap image even if software had published earlier.

**E15. HAS §6.1.14 quiesce**: a port with an empty write store but one live read does not ack until the read's granules are consumed, its response crossing is drained, and the terminal response is done. VP check: hold the R response (initiator delays), and quiesce_ack stays 0 until it returns.

Non-golden (ABSTRACTED, listed only for traceability): MAS T6-26 (tag allocation), T6-29 (atom window), T6-47/T6-49 (token bucket, floor, round robin), T6-48 (credit ledger), T6-17/T6-62 (CDC stop timing), HAS §6.1.8 epoch 12/11 example.
