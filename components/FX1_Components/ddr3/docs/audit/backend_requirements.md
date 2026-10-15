# DDR3 Controller VP — Backend Requirements Audit (P0)

**Scope.** Backend of the FTEL DDR3 controller: ECC storage/capacity (HAS §2.5–2.6), backend functional blocks (HAS §6.2.1–6.2.20), backend block interfaces (HAS §7.5). The CSR fields and IRQ bits that these blocks drive are cited only where they are the observable effect of a backend requirement. Frontend, AXI port, CSR decode and maintenance sequencing are covered by other audits.

**Precedence.** HAS v1.0 is normative. MAS v0.1 is supplementary (HAS > MAS > RTL). MAS chapters 7–9 are referenced by MAS but missing. Nothing here infers their content.

**Sources actually read** (line numbers in the source `.md` files):

| Doc | Lines | Content |
|---|---|---|
| HAS | 830–870 | §2.3–2.7 (scope §2.5–2.6) |
| HAS | 2140–2880 | §6.2.1–6.2.20, start of §7 |
| HAS | 3530–3960 | §7.5.1–7.5.20 |
| HAS (support) | 4100–4220, 4225–4250, 4280–4420, 4470–4500, 4536–4560, 4950–4962, 5055–5140 | Table 8-5 address map; Tables 8-6, 8-11..8-14, 8-16, 8-18; §9 scrub count, BE03 restatement |
| MAS | 1640–1925 | §5.4–5.8 (devices, speed bins, reset images, Table 5-19/5-20 legality, Table 5-21) |
| MAS | 3223–5300 | §6.2–6.2.20 (base64 image lines skipped) |

**VP class legend.** MODEL = bit/value-exact functional behaviour. APPROX = modelled, but timing or values are approximate (event-timed). ABSTRACTED = not modelled, with the reason. OUT = outside the VP.

**Golden-check status.** I recomputed every MAS mapping and SECDED vector in §3 and §6 with an independent script (`/usr/bin/python3`), and all of them matched. Values marked *derived* are my own calculations from the quoted formulas. They are not quoted from the documents.

---

## 1. Requirement table

### 1.1 Capacity, mapping, ECC storage (HAS §2.5, §6.2.2, §7.5.2)

| Req ID | Source | Requirement (one testable statement) | VP class | Proposed VP test ID | Notes |
|---|---|---|---|---|---|
| BE-2.5-01 | HAS §2.5, Table 2-4 | A user granule is 32 B, holding four 64-bit codewords with one ECC byte each. Eight data granules share one hidden 32-B companion granule made of eight 4-B records. | MODEL | VP-ECC-01 | Record layout in §3.4 (MAS §6.2.17). |
| BE-2.5-02 | HAS §2.5 eq., §6.2.2 Table 6-31 | With ECC enabled, the visible capacity is `U = min(8×floor(P/9), 2^27)` granules. | MODEL | VP-MEM-01 | Example: P=9 gives U=8. |
| BE-2.5-03 | HAS §2.5, §7.5.2 | The user address space is at most 4 GiB = 2^27 granules (G_u = 27 bits). | MODEL | VP-MEM-02 | |
| BE-6.2.2-01 | HAS §6.2.2 Table 6-30; MAS §6.2.2 | `P = A × 8 × 2^R_row × (Wp/8)` and `C = log2(Wp/8)`. Legal Wp values are 256/512/1024/2048 words, so C = 5..8. | MODEL | VP-MEM-03 | One word is 32 bit (x32 bus), so Wp/8 is granules per row. |
| BE-6.2.2-02 | HAS Table 8-11; MAS Table 5-21 | The geometry is taken from TOPO_ACTIVE: [0] = active ranks − 1, [8:4] = row bits, [10:9] = page code, [12] = map profile. | MODEL | VP-CFG-30 | Page-code and profile encodings: GAP-BE-06/07. ECC enable bit is missing: GAP-BE-05. |
| BE-6.2.2-03 | HAS Table 6-31; MAS §6.2.2 | With ECC off: visible capacity `U0 = min(P, 2^27)`, ECC_BASE = 0, and companion coordinates/slot are forced to 0. | MODEL | VP-MEM-04 | HAS §6.2.2 last rule; MAS BE-MAP-06. |
| BE-6.2.2-04 | HAS Table 6-31 | With ECC on: `Q = floor(P/9)`, `U = min(8Q, 2^27)`, `base = U`. | MODEL | VP-MEM-05 | |
| BE-6.2.2-05 | HAS Table 6-32; MAS §6.2.2 | Companion granule `e = base + floor(d/8)`. Record slot `s = d mod 8` (= d[2:0]). | MODEL | VP-ECC-02 | |
| BE-6.2.2-06 | HAS §6.2.2; MAS §6.2.2 decode | In the linear profile, `d = [row | rank | bank | column]`. The exact decode is in §3.2. | MODEL | VP-MEM-06 | Golden: MAS Table 6-78/6-79. |
| BE-6.2.2-07 | HAS §6.2.2; MAS §6.2.2 decode | In the interleaved profile, `d = [row | rank | column | bank]`. The exact decode is in §3.2. | MODEL | VP-MEM-07 | Golden: MAS Table 6-78/6-79. |
| BE-6.2.2-08 | HAS Table 6-32 ex.; MAS decode | With 2 active ranks, rank = g[C+3] in both profiles. With 1 rank, the rank field is absent (rank = 0) and `row = g >> (C+3)`. | MODEL | VP-MEM-08 | Ranks alternate every 2^(C+3) granules (32 KiB for C=7). |
| BE-6.2.2-09 | HAS §6.2.2; MAS BE-MAP-07..09 | The companion granule is decoded with the same function and profile as data. The decode is one-to-one, and no data coordinate equals an ECC coordinate. | MODEL | VP-MEM-09 | Property test over a full small geometry. |
| BE-6.2.2-10 | HAS Table 6-33; MAS Table 6-80, BE-MAP-19/20 | A request with d ≥ U (hidden region or above) is poisoned with DECERR. It issues no DRAM command: a read returns zero data, a write changes no memory. | MODEL | VP-MEM-10 | Ordinary AXI can never reach hidden storage (HAS §6.2.2, BE03). |
| BE-6.2.2-11 | HAS Table 6-33; MAS Table 6-80 | Decoded row ≥ 2^R_row gives DECERR and poison (backstop). | MODEL | VP-MEM-11 | Unreachable with a legal geometry. Test with an assertion only. |
| BE-6.2.2-12 | HAS Table 6-33; MAS Table 6-80 | An impossible geometry makes every request SLVERR + poison and raises an error event. `e ≥ P` (range_fault) gives SLVERR + poison + error event. | MODEL | VP-FAULT-30 | Status priority: geom_impossible > out_of_capacity/row_overflow > range_fault. IRQ link: GAP-BE-28. |
| BE-6.2.2-13 | HAS Table 8-11, RS08; MAS BE-MAP-17 | USABLE_GRANULES, PHYSICAL_GRANULES and ECC_BASE are published as zero-extended 64-bit LO/HI values. They read 0 when the geometry is illegal or invalid. | MODEL | VP-CFG-31 | HI word = bits [63:32] = 0 in this profile. |
| BE-6.2.2-14 | MAS Table 5-20 (HAS silent) | TOPO_COMMIT succeeds only if: quiesced; 1 ≤ ranks ≤ MAX_RANKS; 12 ≤ row bits ≤ ROW_BITS_MAX; page code legal; profile in MAP_PROFILE_MASK; P within the capacity limit; with ECC, the build has ECC and floor(P/9) > 0. On failure: blocker 0x11 and the active geometry is kept. | MODEL | VP-CFG-32 | The capacity limit conflicts between HAS and MAS: C-05. Blocker location HAS Table 8-13. |
| BE-6.2.2-15 | HAS §6.2.2; MAS BE-MAP-15/16 | A scrub lookup of granule g returns the same data/companion coordinates as a mission decode of g. It returns an error and zero result if ECC is off, the geometry is illegal, or g ≥ U. | MODEL | VP-SCRUB-11 | |
| BE-6.2.2-16 | MAS §6.2.2 (HAS silent) | Request kind is: write → WR_DATA; read with ECC active and not poisoned → RD_ECC; otherwise RD_DATA. | MODEL | VP-ECC-03 | Drives whether a read is ECC-checked. |
| BE-6.2.2-17 | HAS §6.1/§6.2.2 install rule; MAS Table 6-81 | A new geometry applies only under global quiesce. Requests already accepted keep their captured mapping and status. | APPROX | VP-CFG-33 | The VP needs no pipeline. It must reject TOPO_COMMIT unless quiesced. |
| BE-6.2.2-18 | HAS §6.2.2 rule BE11, §6.2.16 | Rank count, FE/BE width, ratio and phase map are validated together. An inconsistent profile cannot be published. | MODEL | VP-PROFILE-01 | MAS Table 5-19 rules 1, 2, 8 are build-time checks. |
| BE-6.2.2-19 | MAS §6.2.2 | A geometry smaller than the device addresses a sub-array. A larger geometry aliases on hardware. | ABSTRACTED | — | Sparse VP storage keyed by physical granule does not alias. Document it in the VP limitations. |

### 1.2 Ingress, request ownership, generation, ordering (HAS §6.2.1–6.2.5, §7.5.1–7.5.5)

| Req ID | Source | Requirement | VP class | Test ID | Notes |
|---|---|---|---|---|---|
| BE-6.2.1-01 | HAS §6.2.1 | Atoms leave ingress in strict arrival order, at most one per controller cycle. Backpressure prevents overflow. | APPROX | VP-MEM-30 | The VP keeps the order. FIFO depth (D_IN default 8) is a TLM backpressure parameter only. |
| BE-6.2.1-02 | MAS §6.2.1 ING-RD-02, ING-QA-04..06 | While quarantine is active, a read atom at the ingress head gets an SLVERR zero-data answer and is never mapped. Writes still flow in order. | MODEL | VP-FAULT-03 | HAS §6.2.11/BE15 states that read admission closes. |
| BE-6.2.1-03 | MAS §6.2.1, Table 6-73 | Admission (ingress ready) is 0 when any of these holds: fatal hold, sticky ECC stop, sequence fault, DFI protocol fault, geometry/timing/parameter image invalid, or mission not ready (includes scrub incomplete with ECC on). | MODEL | VP-FAULT-12 | Atoms already inside drain. |
| BE-6.2.1-04 | HAS §7.5.1 | Ingress occupancy is 0..D_IN, and empty is reported. | ABSTRACTED | — | Cycle-level FIFO occupancy is not meaningful in the VP. BACKEND_OCCUPANCY words are APPROX (CSR audit). |
| BE-6.2.3-01 | HAS §6.2.3, §7.5.3 | At most 16 requests are resident. Installation allocates {index(4b), generation(4b)} together with the payload. | ABSTRACTED | — | Not observable, except through the quarantine first-cause identity (BE-6.2.11-06). The VP keeps a 16-slot ID allocator only for fault injection. |
| BE-6.2.3-02 | HAS §6.2.3 | Exactly one terminal response is issued per request. It carries the identity from installation and is held until the consumer accepts. | MODEL | VP-MEM-31 | |
| BE-6.2.3-03 | HAS §6.2.3 | No fixed end-to-end response deadline exists. | APPROX | — | VP latency comes from the bank/timing model (§1.3). |
| BE-6.2.4-01 | HAS §6.2.4 eq., Table 6-34; MAS BE-REQ-01 | An event mutates slot i only if `event_valid ∧ resident_valid[i] ∧ event_gen = resident_gen[i]`. Any other event is discarded and a stale diagnostic is raised. | MODEL | VP-FAULT-20 | Fault injection: replay an old-generation completion. |
| BE-6.2.4-02 | HAS §6.2.4 | The generation is 4 bits and increments modulo 16 on reuse. | MODEL | VP-FAULT-21 | MAS: increment at install. Generation is not reset on free. |
| BE-6.2.4-03 | HAS RS01; MAS §6.2.3, Table 6-134 | A slot is not reused while a prior owner can still complete. In particular, index i is not freed while a read token of i is queued (rd_index_busy). | MODEL | VP-FAULT-21 | A late return of an old generation never reaches a new occupant. |
| BE-6.2.5-01 | HAS §6.2.5 eq.; MAS BE-REQ-07 | Two non-poisoned requests to the same physical granule where at least one is a write complete in install order. A read issued after a write observes the written data. | MODEL | VP-MEM-32 | Golden: MAS Table 6-89. |
| BE-6.2.5-02 | HAS §6.2.5; MAS Table 6-91 | Reads of different transactions to the same granule are not ordered relative to each other. | MODEL | VP-MEM-32 | The VP may serialize them. Values are unaffected. |
| BE-6.2.5-03 | HAS RS02; MAS §6.2.5 seq check | Same-gtag atoms become ready in seq_idx order. A modular sequence violation `(seq(N) − seq(O)) mod 2^w ∈ {0} ∪ [2^(w−1), …)` blocks install and sets a sticky seq fault that closes admission. | ABSTRACTED | — | The frontend guarantees the order. Model the fault only if the frontend audit requires injection. |

### 1.3 Bank state, timing legality, scheduling (HAS §6.2.6–6.2.9, §7.5.6–7.5.9)

| Req ID | Source | Requirement | VP class | Test ID | Notes |
|---|---|---|---|---|---|
| BE-6.2.6-01 | HAS §6.2.6 eq.; MAS Table 6-107 | Bank state is tracked per (rank, bank), with MAX_RANKS×8 banks. `hit = open ∧ open_row = req_row`. A closed bank needs ACT. An open bank with a different row needs PRE then ACT. | APPROX | VP-MEM-40 | Drives granule latency. |
| BE-6.2.6-02 | MAS Table 6-93 | ACT opens a row. PRE closes the bank. PREA closes the whole rank. REF requires the bank closed and sets refresh-blocked until refresh done. | APPROX | VP-MEM-41 | |
| BE-6.2.6-03 | MAS §6.2.6, BNK-PW-02 | Entering precharge power-down or self-refresh closes all rows of the rank. Active power-down keeps the rows. Exits do not change rows. | APPROX | VP-POWER-01 | Maintenance issues PREA before PDE/SRE. |
| BE-6.2.6-04 | HAS §6.2.6 | Rank 0 maps to CS0 and rank 1 to CS1. One-rank and two-rank profiles use the same rank index convention. | ABSTRACTED | — | No pins in the VP. The rank index is kept in the VP trace. |
| BE-6.2.6-05 | MAS §6.2.6 | Bank state of inactive ranks is cleared, and only active ranks are refreshed or initialized. | APPROX | VP-POWER-02 | |
| BE-6.2.7-01 | HAS §6.2.7 eq. T_next; MAS DTE-TB | The timing timebase counts tCK and advances by FREQ_RATIO per core cycle, modulo 2^32. Comparisons are wrap-safe (`signed(t − dl) ≥ 0`). | APPROX | VP-MEM-42 | The VP derives a tCK count from sc_time and uses the same wrap-safe compare. |
| BE-6.2.7-02 | HAS §6.2.7 `legal` eq.; MAS Table 6-99 | A command is legal only if structural, bank, rank (tRRD, tFAW 4-slot, tRFC), channel (tCCD, tRTW, WR→RD, rank switch) and refresh terms all hold. Only accepted launches update deadlines. | APPROX | VP-MEM-43 | Use the deadline model at granule granularity. Golden: MAS Table 6-102. |
| BE-6.2.7-03 | MAS Table 6-98 | Derived delays (exact formulas in §3.6) are used for latency. BL8 only. No auto-precharge (open-page policy). | APPROX | VP-MEM-44 | |
| BE-6.2.7-04 | HAS §6.2.7 BE13 (timing); MAS §5.5 | A time requirement converts as `ceil(required_time / clock_period)`. n tCK last `ceil(n/R)` controller cycles. A value that cannot be represented is rejected. | MODEL | VP-CFG-10 | Applies when the 12 MAS-only words are derived from ns at build time. |
| BE-6.2.7-05 | HAS Table 8-11/8-12; MAS Table 5-20 | TIMING_COMMIT validates the full 19-word bank against the exact bounds in §3.7. Failure: SLVERR, blocker, previous active image kept. Success: the active mirrors A_* = shadow. | MODEL | VP-CFG-01..08 | Values in tCK counts. Blocker 0x04 (MAS). |
| BE-6.2.7-06 | MAS DTE-CS-01/02 (HAS silent) | Timing changes take effect only when no deadline is live (during init, or in self-refresh with the clock stopped). | APPROX | VP-CFG-09 | VP: the commit requires quiesce and applies only to later commands. See GAP-BE-23. |
| BE-6.2.7-07 | HAS §6.2.7 | Legality reports nine independent reason bits. | ABSTRACTED | — | Not CSR-visible, and the encoding is not defined. |
| BE-6.2.7-08 | MAS Table 6-99 tRFC row; HAS Table 8-12 | After REF, every command to the rank except PDE/PDX waits tRFC. Refresh is due every tREFI per active rank. | APPROX | VP-POWER-03 | Debt/postponement (REFRESH_CFG 1..8) belongs to the maintenance audit. |
| BE-6.2.7-09 | MAS Table 6-99, 6-100 | After PDX the rank waits tXP (tXPDLL for RD/WR after a DLL-off precharge power-down). After SRX it waits tXS (tXSDLL for RD/WR). | APPROX | VP-POWER-04 | Golden: SRX at 1000 → ACT at 1216, RD/WR at 1512. |
| BE-6.2.8-01 | HAS §6.2.8 RS03 | Supported ECC operation kinds are protected read, full write, partial-write RMW and startup scrub. Any other kind gives one SLVERR terminal and no launch. | ABSTRACTED | — | AXI cannot generate other kinds. |
| BE-6.2.9-01 | HAS §6.2.9, RS04; MAS §6.2.9 | Scheduling uses class, age, row hit, rotation, and rescue after STARVE_LIMIT. | ABSTRACTED | — | There is no OoO scheduler in the VP. Requests are serviced in arrival order with bank-state latency. |
| BE-6.2.9-02 | HAS §6.2.9 eq.; MAS §6.2.9 | Write-drain hysteresis: enter when occupancy ≥ enter, exit when occupancy ≤ exit or when the budget is reached. | APPROX | VP-MEM-50 | Only SCHED_STATUS[0] and STAGED_WRITE_OCC are visible. Cool-down: C-10. |
| BE-6.2.9-03 | HAS Table 8-14; MAS Table 5-20 | WRITE_DRAIN_CFG legal values: 1 ≤ enter ≤ REQ_DEPTH(16), 0 ≤ exit < enter, max ≥ 1. STARVE_LIMIT is 1..2^31−1. An illegal write gives SLVERR and keeps the old value. Reset values: 0x00100408 and 1024. | MODEL | VP-CFG-20/21 | Policy image = WRITE_DRAIN_CFG + STARVE_LIMIT + READ_TIMEOUT (HAS Table 8-14 note). |

### 1.4 Dispatch, read metadata, timeout, quarantine (HAS §6.2.10–6.2.11, §7.5.10–7.5.11)

| Req ID | Source | Requirement | VP class | Test ID | Notes |
|---|---|---|---|---|---|
| BE-6.2.10-01 | HAS §6.2.10 | Physical acceptance and request acceptance are separate events. Parentless maintenance or scrub commands have only physical acceptance. | ABSTRACTED | — | Internal. |
| BE-6.2.10-02 | MAS §6.2.10 read return; HAS RS05 | Every non-OKAY read return (DECERR, SLVERR) carries all-zero data. | MODEL | VP-MEM-33 | |
| BE-6.2.10-03 | MAS §6.2.10 | A write completion reports the worst status of DECERR (mapping), SLVERR, and write-fault SLVERR. A later fault never downgrades it. There is one completion per write atom. | MODEL | VP-FAULT-13 | |
| BE-6.2.10-04 | MAS Table 6-124/6-129 | An injected DFI write-result error gives B = SLVERR. On an ECC parent it is an ECC write fault. | MODEL | VP-FAULT-14 | MAS ERROR_STATUS@0x404C is not in the HAS map: C-17. |
| BE-6.2.10-05 | HAS §6.2.10; MAS §6.2.10 faults | Fatal events latch a sticky FATAL_STATUS bit (no software clear), set IRQ_RAW[11] and close admission until cold reset. | MODEL | VP-FAULT-12 | MAS bits [3..7] known. Others: GAP-BE-11. |
| BE-6.2.10-06 | HAS §6.2.10 | A failure after an irreversible write triggers fatal containment and the granule lock stays held until cold reset. Issued writes are never rolled back. | MODEL | VP-FAULT-10 | Same rule as BE02. |
| BE-6.2.11-01 | HAS §6.2.11; HAS §7.5.11 | A read token {id(8), slot(4), epoch(8)} is reserved before issue. The response carries {port(2), slot(4), epoch(8), status(2), data(256)}. | MODEL | VP-MEM-34 | Slot and epoch are pass-through identity in a TLM extension. |
| BE-6.2.11-02 | HAS Table 6-40 eq. | `return_match = return_valid ∧ ¬quarantine ∧ token_count>0 ∧ ¬malformed ∧ ¬empty_token ∧ ¬phy_error`, and the identity comes from the head token. A return that fails the match never completes a younger request. | MODEL | VP-FAULT-01 | Injection hooks are needed for malformed, empty_token, phy_error, tokenless and extra returns. |
| BE-6.2.11-03 | MAS §6.2.11 push/pop; HAS §6.2.11 | `deadline = now(tCK) at issue + READ_TIMEOUT`. If the head deadline is reached (modular) without a good pop of the head, quarantine starts with reason 0 and IRQ_RAW[2] is set. A good pop in the same cycle wins over the timeout. | MODEL | VP-FAULT-02 | Unit: GAP-BE-13. The VP return delay must stay below the timeout: GAP-BE-26. |
| BE-6.2.11-04 | MAS Table 6-135 | Quarantine reasons, in priority order 4, 3, 1, 2, 0: 4 = generation mismatch at pop, 3 = context/kind mismatch, 1 = malformed (status 10), 2 = tokenless/unexpected, 0 = head timeout. Reasons 1–4 set IRQ_RAW[3]. | MODEL | VP-FAULT-04 | Reason codes for HAS empty_token/phy_error: GAP-BE-12. |
| BE-6.2.11-05 | MAS §6.2.11, Table 6-136; HAS BE15 | Quarantine is sticky until cold reset. The token FIFO is cleared. New and outstanding reads (issued or not) end with SLVERR + zero data. RD_ECC/RMW parents end with SLVERR and no write. Writes needing no old-data read, maintenance and scrub continue. Late returns are discarded. | MODEL | VP-FAULT-05 | MAS bound: ≤ 2×R_P+2 cycles, which is APPROX in the VP. |
| BE-6.2.11-06 | HAS RS10; MAS Table 6-137 | The first cause {index, generation, kind, reason, deadline} is latched once and never overwritten until cold reset. The layout is in §3.9. | MODEL | VP-FAULT-06 | Index/generation values depend on the allocator (APPROX). The field layout is exact. Kind codes: GAP-BE-14. |
| BE-6.2.11-07 | HAS Table 8-14; MAS §6.2.11/§6.2.12 | READ_TIMEOUT: nonzero, < 2^(BE_TIME_W−1) = 2^31, and ≥ `5×FREQ_RATIO + trddata_en + tphy_rdlat + 4 + DFI_ASSEMBLY_MARGIN`. Checked on write and on DFI_TIMING_COMMIT. Reset value 1024. | MODEL | VP-CFG-22 | The lower bound is MAS-only: C-19. |
| BE-6.2.11-08 | HAS BE15 (§6.2.15) | Leaving quarantine requires a coordinated controller+PHY reset and full initialization. Clearing only the adapter tokens does not restore readiness. | MODEL | VP-FAULT-07 | |
| BE-6.2.11-09 | HAS §6.2.11 | 16 read slots, 8-bit epoch, metadata FIFO depth default 16 (legal 4–64). | ABSTRACTED | — | Capacity only. |

### 1.5 Memory interface, serializer, boundary (HAS §6.2.12–6.2.16, §7.5.12–7.5.16)

| Req ID | Source | Requirement | VP class | Test ID | Notes |
|---|---|---|---|---|---|
| BE-6.2.12-01 | HAS BE12 | Native: ratio 2, 128-bit words, 16-bit mask, X = 2 transfers per granule, lower 128 bits first. Legacy: ratio 1, 64-bit words, X = 4. | ABSTRACTED | — | No DFI modelling. Ratio only scales tCK↔core-cycle conversion (VP-PROFILE-02). |
| BE-6.2.14-01 | HAS §6.2.14 eqs. | `word_k`, `mask_k` and `physical_mask_k = ~mask_k` are defined as in §3.10. | ABSTRACTED | — | Pin level. The functional equivalent is BE-6.2.14-02. |
| BE-6.2.14-02 | HAS §6.2.14, §2.4 | With ECC off, a partial write updates exactly the bytes whose byte-enable is 1. Other bytes keep their stored value. | MODEL | VP-MEM-35 | |
| BE-6.2.15-01 | HAS BE14/BE15 | Malformed, unexpected, tokenless, extra or timed-out PHY returns close read admission and latch quarantine. A legal low-valid gap inside the return window is allowed. | MODEL | VP-FAULT-01/04 | Injection only. Gaps are not modelled. |
| BE-6.2.16-01 | HAS BE17 | A profile (ratio/rank) change is allowed only after admission stop and full drain. Publishing clears readiness and requires full init and training. | MODEL | VP-PROFILE-03 | In the VP the ratio is a build parameter, so only the rank/geometry change path applies. |
| BE-6.2.13-01 | HAS §6.2.13 | A reserved opcode produces a diagnostic and no command. | ABSTRACTED | — | Unreachable. |

### 1.6 ECC codec, events, lock, RMW (HAS §6.2.17–6.2.19, §7.5.17–7.5.19)

| Req ID | Source | Requirement | VP class | Test ID | Notes |
|---|---|---|---|---|---|
| BE-6.2.17-01 | HAS §6.2.17 Table 6-43 | N_cw = 4 and W_protected = 288. Each 64-bit lane has an 8-bit check field. | MODEL | VP-ECC-01 | |
| BE-6.2.17-02 | MAS §6.2.17 (HAS silent on code) | Each lane uses an extended Hamming (72,64) code with even parity: positions, check bits, syndrome and mismatch exactly as in §3.3. | MODEL | VP-ECC-04 | Bit-exact golden: MAS Table 6-154. |
| BE-6.2.17-03 | MAS Table 6-153 | Decode classes: CLEAN, CORRECTED_DATA (syndrome ≠ 0 with mismatch; the data bit is flipped if the position is a data position), CORRECTED_PARITY (syndrome 0 with mismatch), UNCORRECTABLE (syndrome > 71, or syndrome ≠ 0 without mismatch). | MODEL | VP-ECC-05 | An error in check bits 0–6 is classed CORRECTED_DATA, not CORRECTED_PARITY. |
| BE-6.2.17-04 | MAS §6.2.17 code box | Record = {E3, E2, E1, E0}, where Ei = check byte of lane i. Slot s occupies bits [32s+31:32s] of the companion granule. | MODEL | VP-ECC-06 | Lane↔bit mapping: GAP-BE-04. |
| BE-6.2.17-05 | HAS BE01, RS05; MAS Table 6-124 | A read with a correctable error returns corrected data with OKAY. A read with an uncorrectable error returns SLVERR with zero data, sets the UE event, and sets IRQ_RAW[1]. | MODEL | VP-ECC-07 | |
| BE-6.2.17-06 | HAS RS05; MAS §6.2.20 | Corrected data is not written back. There is no patrol or repair, so a CE persists and is counted again on every read. | MODEL | VP-ECC-08 | |
| BE-6.2.17-07 | HAS §6.2.17, §6.2.2 | With ECC disabled the codec and companion are bypassed and there is no ECC event. | MODEL | VP-ECC-09 | |
| BE-6.2.17-08 | MAS §6.2.17 events, ECC-EV-01..03 | At most one ECC event per parent, emitted with the accepted terminal, and only when a decode was needed and some lane is CE or UE. The event carries {UE flag, ce_codewords 0..4, physical data granule, lane byte mask, syndrome}. A full write decodes nothing and emits no event. | MODEL | VP-ECC-20 | HAS §7.5.19 bundle widths. |
| BE-6.2.17-09 | MAS Table 6-156; HAS Table 8-16 | `CE_COUNT += ce_codewords`, saturating at 0xFFFF_FFFF. `UE_COUNT += 1` per UE event, saturating. Both are cleared only by reset. | MODEL | VP-ECC-21 | HAS: these values are read through acknowledged snapshots (cross-ref CSR audit). |
| BE-6.2.17-10 | MAS Table 6-156; HAS Table 8-16/8-18 | IRQ_RAW[0] is set when CE_COUNT reaches the non-zero CE_THRESHOLD (reset 1). | MODEL | VP-ECC-22 | Value 0: C-16. "Reaches" semantics: GAP-BE-16. |
| BE-6.2.17-11 | HAS Table 8-16 | ECC_LAST_ADDR_LO/HI = physical granule of the last error. ECC_LAST_INFO = type/codeword/syndrome. All ECC snapshot words read 0 when ECC is disabled. | MODEL (addr) / GAP (info) | VP-ECC-23 | GAP-BE-01/02/03. |
| BE-6.2.17-12 | HAS Table 8-16 | ECC_CAP_STATUS: [0] ECC option, [1] RMW busy, [8:2] lock occupancy, [12:9] = 4, [20:13] = 8. | MODEL ([0],[12:9],[20:13]) / APPROX ([1],[8:2]) | VP-ECC-24 | |
| BE-6.2.18-01 | HAS §6.2.18 eq.; MAS §6.2.18 | A lock is per physical granule. A conflicting owner waits. Release takes effect only when `release_valid ∧ release_done ∧ owner_match`. A stale or duplicate release changes nothing. | MODEL | VP-ECC-30 | Functional serialization of ECC parents on one granule. |
| BE-6.2.18-02 | HAS BE02 (§6.2.18) | A successful operation or a pre-launch abort releases the lock exactly once. After a post-launch fatal error the lock stays held, and stale releases are rejected until cold reset + init. | MODEL | VP-FAULT-10 | |
| BE-6.2.18-03 | MAS §6.2.18 | A release with a stale id moves the parent to FATAL_HOLD. | MODEL | VP-FAULT-11 | Injection. |
| BE-6.2.18-04 | HAS §7.5.18 | The lock table is bounded and its occupancy is 7 bits. | ABSTRACTED | — | Only ECC_CAP_STATUS[8:2] is visible, and it is APPROX. |
| BE-6.2.19-01 | HAS §6.2.19; MAS Table 6-160 | Partial write sequence: lock, RD data, RD ECC granule, check, merge, WR data (256 bits, all 32 BE), WR record (4 B of slot s), terminal, release. | MODEL (data) / APPROX (time) | VP-ECC-10 | Two reads and two writes, which affects latency. |
| BE-6.2.19-02 | HAS §2.6, §6.2.19; MAS Table 6-160 | Full write (all 32 BE set): no old-data read, but the ECC granule is read so that the other 7 records are preserved. Then WR data and WR record. | MODEL | VP-ECC-11 | "Profile-controlled" bypass: GAP-BE-17. |
| BE-6.2.19-03 | MAS Table 6-160 | Checked read: lock, RD data, RD ECC, check, terminal (corrected data + OKAY, or zero + SLVERR), release. | MODEL | VP-ECC-12 | |
| BE-6.2.19-04 | HAS Table 6-45 eq.; MAS merge | `merged[b] = BE[b] ? new[b] : corrected_old[b]` for b = 0..31. The record is then re-encoded from the merged data. | MODEL | VP-ECC-13 | |
| BE-6.2.19-05 | HAS BE02; MAS ECC-CT-01 | A UE in a needed lane (lane BE ≠ 0xFF) ends the parent with SLVERR (B) and no data or record write. DRAM and the old record are unchanged. Exactly one UE event. Lock released. | MODEL | VP-ECC-14 | HAS §6.2.19 paragraph says "fatal": C-08. |
| BE-6.2.19-06 | MAS ECC-CT-02, Table 6-155 | A UE only in fully overwritten lanes does not abort. The write heals the lane, and that lane counts toward ce_codewords. | MODEL | VP-ECC-15 | HAS silent, MAS adopted. |
| BE-6.2.19-07 | MAS ECC-SQ-03 | If the ECC-granule read of a full write fails (non-OKAY notify or walk abort), the parent ends with SLVERR and no DRAM write. | MODEL | VP-FAULT-15 | |
| BE-6.2.19-08 | MAS §6.2.19 | The record write uses byte enables for the 4 bytes of slot s only. The 7 sibling records are unchanged. | MODEL | VP-ECC-16 | Property test across slots 0..7. |
| BE-6.2.19-09 | MAS Table 6-136, ECC-EV-02 | A quarantine walk abort ends the parent with SLVERR, no write and no ECC event. | MODEL | VP-FAULT-05 | |

### 1.7 Startup ECC scrub (HAS §6.2.20, §7.5.20, §9 BE03)

| Req ID | Source | Requirement | VP class | Test ID | Notes |
|---|---|---|---|---|---|
| BE-6.2.20-01 | MAS §6.2.20 start; HAS Table 7-45 gates | Scrub starts when all hold: ECC active, geometry valid, PHY ready, DRAM initialized, timing active, no training, no failure, all ranks ACTIVE. | MODEL | VP-SCRUB-01 | |
| BE-6.2.20-02 | HAS BE03, §6.2.20 eqs.; MAS walk | Groups of 8 data granules from g = 0 to U−1. Slots 0..7 write data granule g+k with zero data and all BE. Slot 8 writes companion `base + (g>>3)` with zero and all BE. Then `g ← g+8`. | MODEL | VP-SCRUB-02 | Golden: MAS Table 6-164. |
| BE-6.2.20-03 | HAS §9 Table 9-3 | `N_total = U + U/8` writes (`N_data = U`, `N_ecc = U/8`). | MODEL | VP-SCRUB-03 | U is always a multiple of 8. |
| BE-6.2.20-04 | HAS BE03, Table 9-5 | The walk advances only on an accepted write. ACT/PRE do not advance the address. | APPROX | VP-SCRUB-04 | The VP computes the duration analytically (write count × modelled per-write cost + refresh). |
| BE-6.2.20-05 | HAS §6.2.20 final_drain; MAS MNT-SC-04 | Completion is reported only after the last group is accepted and the DFI is idle (DRAIN state). | APPROX | VP-SCRUB-05 | |
| BE-6.2.20-06 | HAS §9 example, BE03; MAS MNT-RD-02 | With ECC on, mission admission stays closed until scrub completes. With ECC off, scrub_complete is treated as true. | MODEL | VP-SCRUB-06 | |
| BE-6.2.20-07 | HAS RS05, BE03; MAS MNT-SC-02 | Any readiness loss (reset, reinit, frequency change, init failure, ordinary prerequisite loss) returns the walk to WAIT. On re-arm the walk restarts at granule 0, with no resume checkpoint. | MODEL | VP-SCRUB-07 | Abort/restart must be observable (progress returns to 0). C-14. |
| BE-6.2.20-08 | MAS MNT-SC-03 | A power-down or self-refresh round trip is not a readiness loss, and a completed scrub stays complete. | MODEL | VP-SCRUB-08 | HAS silent, MAS adopted. |
| BE-6.2.20-09 | HAS BE03 | A mapping fault, i_failure or containment fault latches a sticky scrub fatal, and mission readiness stays 0. | MODEL | VP-SCRUB-09 | MAS default build has no mapping-fault path: C-13. |
| BE-6.2.20-10 | MAS §6.2.20 | After scrub, every user granule reads back as zero data with CLEAN decode, because zero data with a zero record is a valid codeword. | MODEL | VP-SCRUB-10 | `enc(0) = 0x00` (verified). |
| BE-6.2.20-11 | HAS Table 7-45 | Scrub status exposes busy, current address, complete, fatal and error. | MODEL | VP-SCRUB-11 | CSR location: GAP-BE-19. |
| BE-6.2.20-12 | HAS §6.2.20; MAS DSC-CB-07 | Scrub is parentless (no request-table entry), runs on lane 0 only, and runs only while admission is closed. | ABSTRACTED | — | Internal. |
| BE-6.2.20-13 | derived from HAS §6.2.2 | The `P − 9Q` physical granules above `9Q − 1` are never written or read. | MODEL | VP-SCRUB-12 | The VP must not allocate them, and a backdoor read of them returns "uninitialized". |

### 1.8 Interface widths (HAS §7.5) — VP payload/extension contract

| Req ID | Source | Requirement | VP class | Test ID | Notes |
|---|---|---|---|---|---|
| BE-7.5.1-01 | HAS §7.5.1 | W_atom = 104 (AXI128) = 7 origin-tag + 27 granule + 8 seq + 8 atom-count + 1 dir + 32 BE + 1 first + 1 last + 2 class + 4 read-slot + 8 epoch + 4 origin-ID + 1 exclusive. | APPROX | VP-MEM-36 | The TLM extension carries every field that affects observable behaviour (granule, BE, dir, slot, epoch, port, class). |
| BE-7.5.2-01 | HAS §7.5.2 | G_u = 27, G_p = 28, capacity counts 29 bits. W_geom = 21 = 2 ranks + 5 row + 12 page + 1 profile + 1 ECC policy. | MODEL | VP-CFG-34 | Shows that an ECC-policy bit exists in the committed geometry (GAP-BE-05). |
| BE-7.5.17-01 | HAS §7.5.17 | Codec status = 2 bits per lane plus aggregate CE/UE flags. Symbolic names are boundary-defined. | ABSTRACTED | — | Not CSR-visible beyond the events. |
| BE-7.5.19-01 | HAS §7.5.19 | ECC event = 1 + 1 + 4 + G_p + 32 + 8 + 1 bits: UE flag, corrected-codeword count, physical data-granule address, affected-byte lane mask, syndrome, operation identity. | MODEL | VP-ECC-20 | Only one 8-bit syndrome per event across four lanes: GAP-BE-03. |
| BE-7.5.20-01 | HAS §7.5.20 | Scrub map result = 84 coordinate bits with separate valid and error. W_scrubop = 89. | ABSTRACTED | — | |

---

## 2. Test ID index (proposed)

- **VP-MEM-01..11**: capacity and mapping.
- **VP-MEM-30..36**: order, terminal, dependency, zero data, identity pass-through, masked write, payload extension.
- **VP-MEM-40..44, 50**: bank/timing latency and drain status.
- **VP-ECC-01..24**: codec, record, events, counters, CAP.
- **VP-ECC-30**: lock.
- **VP-SCRUB-01..12**: startup scrub.
- **VP-FAULT-01..07**: read return and quarantine.
- **VP-FAULT-10..15**: fatal, containment and write faults.
- **VP-FAULT-20..21**: generation.
- **VP-FAULT-30**: geometry and configuration errors.
- **VP-POWER-01..04**: rows and timing across PD/SR/REF.
- **VP-PROFILE-01..03**: profile consistency and switching.
- **VP-CFG-01..10, 20..22, 30..34**: commit and policy legality.

---

## 3. Exact formulas (implement and golden-test)

### 3.1 Capacity (HAS §2.5, §6.2.2 Tables 6-30/6-31; MAS §6.2.2)
```
granules_per_row = Wp / 8                 C = log2(Wp / 8)        -- C = 5..8 for Wp in {256,512,1024,2048}
P   = A × 8 × 2^R_row × (Wp / 8)          -- A = active ranks (TOPO[0]+1), R_row = TOPO[8:4]
U0  = min(P, floor(4 GiB / 32 B)) = min(P, 2^27)                       -- ECC off
Q   = floor(P / 9)
U   = min(8Q, 2^27)                                                    -- ECC on
base = U   (ECC on);  ECC_BASE = 0 (ECC off)                           -- HAS Table 8-11 "zero without ECC"
```
MAS form: `usable = ecc_enable ? 8 × floor(P/9) : P`. It equals the HAS form only under the MAS rule P ≤ 2^27. Implement the HAS `min(...)` form (C-05).

### 3.2 Address decode (MAS §6.2.2 "Address decode", verbatim semantics; HAS §6.2.2 field order)
```
-- one decode function, used for the data granule d and for the ECC granule
r = (active_ranks = 2)
linear:       column = g mod 2^C;          bank = (g >> C) mod 8
interleaved:  column = (g >> 3) mod 2^C;   bank = g mod 8
rank = r ? (g >> (C + 3)) mod 2 : 0
row  = g >> (C + 3 + r)
ecc_granule = ecc_base + (d >> 3);  record slot = d[2:0]
out_of_capacity = (d >= usable)            -> DECERR, poison
row_overflow    = (data row >= 2^row_bits) -> DECERR, poison (backstop)
range_fault     = (ecc_granule >= P)       -> SLVERR, poison, error event (backstop)
geom_impossible                            -> SLVERR, poison, error event (first-match priority)
poison: every coordinate, physical_granule and ECC field forced to 0
```

### 3.3 SECDED (72,64) per 64-bit lane (MAS §6.2.17 "Code and record"; HAS defines no H-matrix)
```
codeword positions 1 .. 71:
  check bits 0 .. 6 at positions 1, 2, 4, 8, 16, 32, 64
  data bits 0 .. 63 at the other positions 3, 5, 6, 7, 9, ... 71, in order
  check bit i = XOR of every position (other than its own) whose index has bit i set
  check bit 7 = overall parity of the 71 positions
syndrome = XOR of the indices of all positions that are 1         (7 bits)
mismatch = XOR of all 72 bits
```
Check byte packing: bit i of the 8-bit check field = check bit i. I verified this against MAS's vector `0x0123456789ABCDEF → 0x9C`.

Classification (MAS Table 6-153), evaluated in this order:

| Syndrome | Mismatch | Result |
|---|---|---|
| 0 | no | CLEAN |
| > 71 | any | UNCORRECTABLE |
| ≠ 0 | yes | CORRECTED_DATA (flip the data bit only if the position is a data position) |
| 0 | yes | CORRECTED_PARITY |
| ≠ 0 | no | UNCORRECTABLE |

### 3.4 Record and companion (MAS §6.2.17; HAS Table 6-32)
```
record of a granule = {E3, E2, E1, E0}          -- Ei: the 8 check bits of lane i (E0 in record bits [7:0])
record slot s of the hidden ECC granule = bits [32s + 31 : 32s]
ECC granule = ECC base + (d >> 3);  s = d[2:0]
```
Lane i is assumed to be granule bits [64i+63:64i], which is bytes 8i..8i+7 (GAP-BE-04).

### 3.5 RMW merge and lane need (HAS Table 6-45; MAS §6.2.17 "Merge")
```
merged[b] = byte_enable[b] ? new_data[b] : old_data[b],  0 <= b < 32   -- old = corrected old data
needed lane = a lane the write does not fully overwrite (byte enables != 0xFF)
UE in a needed lane          -> abort: no DRAM write, SLVERR
UE only in overwritten lanes -> no abort; the write heals the lane, counted as corrected
ce_codewords (0..4) = corrected lanes (incl. healed UE lanes) = CE_COUNT increment
```

### 3.6 Timing derived delays (MAS Table 6-98; BL/2 = 4)
```
act_to_col_delay         = max(0, tRCD - AL)                 -- ACT->RD/WR same bank
rd_precharge_delay       = AL + tRTP                         -- RD->PRE same bank
wr_precharge_delay       = CWL + AL + BL/2 + tWR             -- WR->PRE same bank
wr_read_turnaround_delay = CWL + AL + BL/2 + tWTR            -- WR->RD channel
rank_switch_delay        = BL/2 + tRTRS                      -- column->column other rank
rd_pde_delay             = AL + CL + BL/2 + 1                -- RD->PDE/SRE
wr_pde_delay             = AL + CWL + BL/2 + tWR             -- WR->PDE/SRE
tRTW is a programmed total; tFAW: 4 expiry slots per rank, an ACT is legal only with a free slot
time_reached(t, dl) = signed(t - dl) >= 0;  T_next = (T + FREQ_RATIO) mod 2^32
DFI_WR_ENABLE_DELAY = CWL + AL - PWL - 2;  DFI_RD_ENABLE_DELAY = CL + AL + PRL + G - 4   (MAS §5.6)
```

### 3.7 Timing commit legality (MAS Table 5-20; HAS §6.2.7 BE13 adds "reject unrepresentable")
- **General.** Every timing value is non-zero where the DRAM standard has a minimum (which words: GAP-BE-09). Every value is < 2^(BE_TIME_W−1) = 2^31.
- **Latencies.** CL 5..11 (12..16 reserved, so rejected). CWL 5..8. T_WR 5..16. AL ∈ {0, CL−1, CL−2}. `T_RTW ≥ CL + 6 − CWL`.
- **Floors (19-word bank).** T_CCD, T_RRD, T_RTP, T_WTR ≥ 4.
- **Floors (MAS-only words, become build-time asserts).** T_CKE, T_XP ≥ 3. T_CKESR ≥ T_CKE+1. T_XPDLL ≥ 10. T_CKSRE, T_CKSRX ≥ 5. T_XSDLL ≥ 512. T_MOD ≥ 12. T_MRD ≥ 4. T_ZQOPER ≥ 256. T_ZQCS ≥ 64.
- **DFI commit.** PWL ≤ WL. tphy_wrlat ≥ R and trddata_en ≥ R. `floor((2R−1+Dmax+3)/R)+1 ≤ CAL_DEPTH−1`. PHY read latency ≠ 0. Margin ≥ 1. The 8-bit fields are < 256. No read token pending and adapter idle. READ_TIMEOUT legal (§3.8).
- **Not in any source, so do not add.** tRC ≥ tRAS+tRP and tRAS ≤ tRC. Checking these would reject images the hardware accepts.

### 3.8 Read timeout and return match (HAS Table 6-40; MAS §6.2.11)
```
return_match = return_valid ∧ ¬quarantine ∧ token_count > 0 ∧ ¬malformed ∧ ¬empty_token ∧ ¬phy_error
return_identity = head_token.identity
deadline = i_now_phase at issue + i_timeout_val              -- READ_TIMEOUT in now_phase (tCK) units
READ_TIMEOUT >= 5 x FREQ_RATIO + trddata_en + tphy_rdlat + 4 + assembly margin;  READ_TIMEOUT < 2^31; != 0
deliver = ctx_valid[idx] ∧ ctx_kind[idx] = token.kind ∧ ctx_req_id[idx] = token.req_id
```

### 3.9 Quarantine first-cause record (MAS Table 6-137; HAS RS10)
| Register | Bits | Field |
|---|---|---|
| QUARANTINE_FIRST_REQ_LO | [5:0] | Request index |
| | [23:8] | generation, zero-extended |
| | [26:24] | token kind |
| | [30:28] | reason (0 timeout, 1 malformed, 2 tokenless/unexpected, 3 context/kind, 4 generation) |
| | [31] | valid |
| | [7:6], [27] | reserved, 0 |
| QUARANTINE_FIRST_REQ_HI | [31:0] | head deadline, low 32 bits (now_phase units) |

For a tokenless return the record holds the reason and zeros, with valid = 1. MAS internal `o_quarantine_status` is: [1:0] = 2'b11 when active, [4:2] = reason, [15:8] = open contexts while active. Its CSR packing is not given by HAS (GAP-BE-15).

### 3.10 Serializer word extraction (HAS §6.2.14; abstracted, for reference)
```
word_k = floor(data / 2^(D·k)) mod 2^D ; mask_k = floor(byte_enable / 2^((D/8)·k)) mod 2^(D/8)
physical_mask_k = bitwise_not(mask_k) ; 0 <= k < X, X = 256/D (2 native, 4 legacy)
```

### 3.11 Generation, hazard, drain, scrub (HAS §6.2.4, 6.2.5, 6.2.9, 6.2.20, §9)
```
apply_event   = event_valid ∧ resident_valid[index] ∧ (event_generation = resident_generation[index])
write_hazard(a,b) = same_physical_granule(a,b) ∧ (is_write(a) ∨ is_write(b))
enter_drain = occupancy >= enter_level ; exit_drain = occupancy <= exit_level
RS04: drain-enter = max(1, ceil(D/2)), drain-exit = floor(D/4)   (D = staged-write depth; D=16 -> 8, 4)
slot_advance = issue_live ∧ accepted_write
next_group   = current_group + 8 after accepted_write ∧ (slot = 8) ∧ ¬final_group
final_drain  = (state = DRAIN) ∧ dfi_idle
N_data = U ; N_ecc = U/8 ; N_total = U + U/8
```

---

## 4. HAS vs MAS conflicts

| # | Item | HAS (cite) | MAS (cite) | Resolution |
|---|---|---|---|---|
| C-01 | TOPO_SHADOW reset | 0x00000000 (Table 8-5; line 4364 "reset to 0x00000000") | 0x00000501 default build, 0x000006F1 for x4 2 Gb (Table 5-21) | **HAS wins.** Reset geometry is illegal (row bits 0), so mission admission stays closed until a legal TOPO_COMMIT. MAS values are used only to confirm field encoding. |
| C-02 | Timing word reset | 19 words and active mirrors reset to 0 (Table 8-5; line 4364) | Speed-bin images (Table 5-15). "TIMING_COMMIT … accepted straight after reset" (§5.6, Table 5-21) | **HAS wins.** A commit of the reset bank fails the non-zero rule. Firmware must program the bank first. |
| C-03 | Timing bank size | 19 words, T_RCD..PHY_READ_LATENCY (Table 8-12) | 31 words (19 + T_CKE..T_ZQCS, Table 5-17/5-21) | **HAS CSR map.** The 12 extra words are build-time parameters taken from MAS Table 5-17 (decided abstraction), with legality checked at elaboration. |
| C-04 | 0x30CC / 0x30D0 | T_CKSRE / T_CKSRX, INIT_RW, reset 0x1 (Table 8-5) | "reserved: read 0 and ignore writes". T_CKSRE/T_CKSRX exist only in the timing bank, floor ≥ 5 (§5.6, Table 5-20) | **HAS wins** (registers exist, reset 1). The HAS reset value of 1 is below the MAS floor of 5: GAP-BE-20. |
| C-05 | Max physical capacity | G_p = 28. "Capacity counts use G_p+1 = 29 bits because a legal count can equal 2^28". U0 = min(P, 2^27) (§7.5.2, Table 6-31) | P ≤ 2^27 required. 2 ranks of 4 Gb x4 / 8 Gb x8 are illegal (§6.2.2 geom_legal, BE-MAP-02, Table 6-77) | **HAS wins.** Accept P up to 2^28 and clamp U/U0 to 2^27. Flag for confirmation (GAP-BE-21). |
| C-06 | Ingress depth range | D_IN legal 2–64 (§7.5.1) | 4–64 (Table 6-71, ING-PA-01) | **HAS wins.** Abstracted in the VP anyway. |
| C-07 | Return-match gates | Includes ¬empty_token ∧ ¬phy_error; 260-bit input with these sidebands (Table 6-40, §7.5.11) | "There is no phase-count or empty-token check". Malformed = status 10 (§6.2.11) | **HAS wins.** Model both triggers via injection. Reason codes: GAP-BE-12. |
| C-08 | UE on RMW old data | §6.2.19 body: "Uncorrectable old data raises a fatal status and retains containment state" | UE abort gives SLVERR, lock released, not fatal (ECC-CT-01, Table 6-124) | HAS conflicts with itself: BE02 (§2.6, §6.2.18, §6.2.19 rules) says a pre-launch UE gives SLVERR with no write. **Adopt BE02** (= MAS). Fatal only after the first physical write. |
| C-09 | Lock vs write result | BE02: "lock remains held through the protected operation". A failure after an irreversible write is fatal and holds the lock (§2.6, §6.2.10) | Lock released in RELEASE without waiting for the DFI write result. A later write error is only a stale event, ERROR_STATUS[0] (§6.2.18) | **HAS wins.** The VP holds the lock until the data and record writes complete. A write-result error on an ECC parent is fatal (GAP-BE-22). |
| C-10 | Drain exit | Exit at the exit level or when the budget is consumed (§6.2.9) | Adds cool-down (max cycles) and forced exit on a rescued read or mandatory maintenance (§6.2.9 Write drain) | HAS silent on cool-down, **adopt MAS** (abstracted. Only SCHED_STATUS is visible). |
| C-11 | Drain max width | Table 8-14: [31:16] max writes (16 bits). §7.5.9: "each 8 bits within the 32-bit bundle" | 1..65535 (§6.2.9) | HAS conflicts with itself. **CSR Table 8-14 wins**: 16-bit max, consistent with MAS. |
| C-12 | Staged-write occupancy | 0–255, 8-bit (Table 6-39, §7.5.9) | WR_STAGE_OCC_W = 5, 0..R_P (§6.2.9) | **HAS width.** The value can never exceed 16 (REQ_DEPTH). Enter-level legal range 1..REQ_DEPTH from MAS. |
| C-13 | Scrub mapping fault | BE03: mapping faults enter sticky fatal (§6.2.20, §9) | Default build: the mapping-reply check is an assertion only, with no fatal path (§6.2.20) | **HAS wins.** A scrub lookup error latches scrub fatal. |
| C-14 | Scrub on prerequisite loss | BE03: return to WAIT to re-arm. RS05: restart from the beginning. Table 9-5: "latch fatal status when the policy requires recovery" | WAIT on a start-condition drop (resume not stated). Restart from 0 on reset/reinit/freq/init fail | **HAS wins:** WAIT, then restart at granule 0. Explicit failure or containment gives fatal. |
| C-15 | DFI bank words | 4 words 0x5010–0x501C (Table 8-5) | 12 words, adds 0x5008, 0x500C, 0x5028–0x503C (§6.2.12) | **HAS map.** The extra 8 words are build-time parameters. |
| C-16 | CE_THRESHOLD = 0 | "nonzero correctable-error threshold" (Table 8-16) | "0 disables the CE interrupt" (Table 5-21, 6-156) | **HAS wins**: 0 is not a legal value. Write behaviour: GAP-BE-16. |
| C-17 | Extra status CSRs | Table 8-5 "gives every defined address". It has no 0x201C, 0x202C, 0x2054 or 0x404C | SCHED_DRAIN_STATUS 0x201C, SCHED_EVENT_COUNT 0x202C, WRITE_FETCH_TIMEOUT 0x2054, ERROR_STATUS 0x404C | **HAS wins.** These are not CSR-visible. The VP keeps them as internal diagnostics or trace only. |
| C-18 | Check-bit error class | Lane classes clean / corrected-data / corrected-parity / UE (§6.2.17) | Error in check bits 0–6 gives CORRECTED_DATA. Only an overall-parity error gives CORRECTED_PARITY (Table 6-153/6-154) | HAS silent on the mapping, **adopt MAS**. |
| C-19 | READ_TIMEOUT lower bound | Nonzero, below half the timestamp range (§6.2.11, Table 8-14) | ≥ 5×R + trddata_en + tphy_rdlat + 4 + margin (§6.2.12) | HAS silent on the lower bound, **adopt MAS**. |
| C-20 | Full-write companion | "Complete overwrite can generate new ECC without reading old data". "Companion still follows the selected protection sequence" (§2.6, §6.2.19) | A full write reads the ECC granule (ECC-SQ-03) | Consistent: the old data is not read, the companion granule is. **Adopt MAS** sequence. |

---

## 5. Gaps / open issues
All "VP default" entries are **proposals**, not spec.

| ID | Issue | Proposed VP default (PROPOSAL) |
|---|---|---|
| GAP-BE-01 | ECC_LAST_INFO bit encoding is undefined. HAS gives only "last-error type/codeword/syndrome". MAS ch8 is missing. | Keep the encoding behind a single formatter function so it can change without touching the model. Provisional layout: [1:0] type (01 CE, 10 UE), [5:2] affected-lane bitmap, [15:8] {mismatch, syndrome[6:0]} of the lowest affected lane, [18:16] ce_codewords, rest 0. Block golden tests on the info word until the spec is resolved. |
| GAP-BE-02 | ECC_LAST_ADDR update policy (CE and UE, or UE only? overwrite or sticky?) and its unit. | Update on every ECC event, last writer wins. Unit = physical data granule index (equal to d), not a byte address. |
| GAP-BE-03 | The event and info carry one 8-bit syndrome but there are four lanes. | Report the lowest-index lane with CE or UE. |
| GAP-BE-04 | The lane ↔ granule bit mapping is not stated explicitly. | Lane i = data bits [64i+63:64i] = bytes 8i..8i+7, inferred from MAS Table 6-155 and the record order {E3..E0}. |
| GAP-BE-05 | No runtime ECC-enable bit exists in TOPO_SHADOW (HAS Table 8-11), but W_geom has an "ECC policy" bit (HAS §7.5.2, line 4748). | ECC active = build parameter `ecc_enable` (reflected in CAP_FEATURE[0]). No runtime toggle. |
| GAP-BE-06 | TOPO[12] encoding (linear vs interleaved) is not given. | 0 = linear, 1 = interleaved, following MAS MAP_PROFILE_MASK bit order. TOPO[12] = 1 with mask 0b01 is a TOPO_COMMIT failure. |
| GAP-BE-07 | Page-code encoding TOPO[10:9]. | code = log2(Wp/256), so 0→256, 1→512, 2→1024, 3→2048. Derived from the MAS reset examples 0x501 (code 2 = 1024) and 0x6F1 (code 3 = 2048). |
| GAP-BE-08 | Commit blocker codes beyond 0x11 (topology) and 0x04 (timing) are in the missing MAS ch8. | Use 0x11 and 0x04 in the immediate-reject field [23:16]. Leave the others TBD (CSR audit). |
| GAP-BE-09 | Which of the 19 words may be 0 ("non-zero where the DRAM standard has a minimum"). | Zero allowed: T_RTRS, ADDITIVE_LATENCY, PHY_WRITE_LATENCY. All others must be non-zero. PHY_READ_LATENCY ≠ 0 is checked at the DFI commit. |
| GAP-BE-10 | Encoding of the 9 timing reason bits. | Not needed. Not observable. |
| GAP-BE-11 | FATAL_STATUS bits for RMW post-write fatal, stale lock release, and scrub fatal. MAS gives only [3]–[7]. | Defer to the CSR audit. The VP exposes these as named internal flags until bits are assigned. |
| GAP-BE-12 | Quarantine reason for the HAS-only empty_token and phy_error gates. | phy_error → reason 1 (malformed). empty_token → reason 2 (tokenless). |
| GAP-BE-13 | READ_TIMEOUT unit is not stated in HAS. | tCK (now_phase units, MAS §6.2.11). |
| GAP-BE-14 | Numeric 3-bit token-kind codes (RD_DATA / RD_ECC / …) used in FIRST_REQ_LO[26:24]. | TBD. Use a named enum in the VP and do not golden-test the numeric value. |
| GAP-BE-15 | QUARANTINE_STATUS CSR packing. HAS: "active/reset-required/cause/context status". | Use the MAS `o_quarantine_status` layout ([1:0]=11, [4:2] reason, [15:8] open contexts). Mark as provisional. |
| GAP-BE-16 | CE threshold "reaches" semantics when CE_COUNT jumps by up to 4, and the response to a write of 0. | Set IRQ_RAW[0] once when `old < THR ≤ new`. A write of 0 gives SLVERR and keeps the old value (HAS "nonzero"). |
| GAP-BE-17 | "Full-overwrite bypass behavior is profile-controlled" (HAS §6.2.19), but no control is defined. | Always bypass the old-data read when all 32 BE are set. Always read the ECC granule. |
| GAP-BE-18 | HAS internal conflict on old-data UE: fatal vs BE02 (C-08). | Follow BE02. Raise with the HAS owner. |
| GAP-BE-19 | Scrub progress, busy, complete and fatal CSR location (HAS: "exact packing defined by the control register contract"). | VP debug attribute plus status hook. CSR field TBD in the CSR audit. |
| GAP-BE-20 | HAS reset T_CKSRE/T_CKSRX = 1, but the MAS floor is ≥ 5. | Keep reset 1 (HAS). Apply the floor check only if these words take part in a commit. HAS does not include them in the 19-word bank, so this is open. |
| GAP-BE-21 | P > 2^27 legality (C-05). | HAS clamp. Add a VP config switch `strict_mas_capacity` that defaults to off. |
| GAP-BE-22 | Lock hold vs write-result timing (C-09). | Hold the lock until write results arrive. A write-result error on an ECC parent is fatal, with the lock held. |
| GAP-BE-23 | Timing commit while deadlines are live (HAS silent). | TIMING_COMMIT requires quiesce (HAS §2.7 lifecycle). New values apply to commands issued after the commit. |
| GAP-BE-24 | HAS reuses decision ID BE13 for two rules (§6.2.7 timing conversion, §6.2.14 serializer). | Cite as BE13(timing) and BE13(serializer). Report to the HAS owner. |
| GAP-BE-25 | ECC counters and metadata are "acknowledged snapshots" (HAS §8 status rules). Capture semantics belong to the CSR audit. | Backend exposes live values. The CSR layer snapshots them on SNAPSHOT_CMD. |
| GAP-BE-26 | The VP's approximate read latency must stay below READ_TIMEOUT for every legal configuration, or quarantine fires spuriously. | Assert at runtime `modelled_return_delay < READ_TIMEOUT`. A timeout happens only through fault injection, which adds a delay or drops a return. |
| GAP-BE-27 | Whether TOPO_COMMIT with a different geometry erases or invalidates data, and whether scrub reruns. | MAS: a geometry change happens only under quiesce, and BE17 requires reinit. Proposal: a geometry commit clears scrub_complete and the sparse store is kept as-is (contents undefined per the spec). |
| GAP-BE-28 | Mapping/config error event → IRQ_RAW[4] ("mapping/configuration") link is not explicit. | Set IRQ_RAW[4] on the geom_impossible/range_fault error event. |

---

## 6. Worked examples usable as golden vectors

**G1. Capacity (HAS §2.5, §6.2.2).** P=9 gives U=8, Q=1, base=8. That is 256 B of data plus one 32-B companion. d=7 gives e=8, s=7. Wp=256 gives 32 granules/row and C=5.

**G2. Reference system (MAS Table 6-79).** 2 ranks, 4 Gb x8, R_row=16, Wp=1024 (C=7), ECC on.

| Item | Value |
|---|---|
| P | 2^27 = 0x800_0000 |
| usable = ECC base | 119 304 640 = 0x71C_71C0 |
| d | 0x123_4567 |
| data, interleaved | rank 1, bank 7, row 0x2468, column 44 |
| data, linear | rank 1, bank 2, row 0x2468, column 103 |
| ECC granule, slot | 0x740_DA6C, slot 7 |
| ECC, interleaved | rank 0, bank 4, row 0xE81B, column 77 |
| ECC, linear (*derived*) | rank 0, bank 4, row 0xE81B, column 108 |
| d = 0x71C_71C0 | DECERR, poisoned |
| d = 2^27−1, usable = 2^27 (no ECC) | OKAY (MAS Table 6-81) |

**G3. Consecutive granules (MAS Table 6-78, reference system).** Format is d → linear (rank, bank, row, col) / interleaved (rank, bank, row, col):
- 0 → (0,0,0,0) / (0,0,0,0)
- 1 → (0,0,0,1) / (0,1,0,0)
- 2 → (0,0,0,2) / (0,2,0,0)
- 7 → (0,0,0,7) / (0,7,0,0)
- 8 → (0,0,0,8) / (0,0,0,1)
- 9 → (0,0,0,9) / (0,1,0,1)
- 127 → (0,0,0,127) / (0,7,0,15)
- 128 → (0,1,0,0) / (0,0,0,16)
- 1024 → (1,0,0,0) / (1,0,0,0)

**G4. Capacity per organisation (*derived* from §3.1; MAS Table 6-77 gives P per rank).** Format is P / U(ECC) / U0 / scrub writes:
- 1 Gb x8 ×1: 0x100_0000 / 14 913 080 / 0x100_0000 / 16 777 215
- 4 Gb x8 ×2: 0x800_0000 / 119 304 640 / 0x800_0000 / 134 217 720
- 4 Gb x4 ×2 (HAS-legal only, C-05): 0x1000_0000 / 134 217 728 (clamped) / 0x800_0000 / 150 994 944

**G5. SECDED (MAS Table 6-154).** Lane 0x0123456789ABCDEF has check byte 0x9C.

| Error injected | Syndrome | Mismatch | Result |
|---|---|---|---|
| none | 0 | no | CLEAN |
| data bit 10 (pos 15) | 15 | yes | CORRECTED_DATA |
| check bit 3 (pos 8) | 8 | yes | CORRECTED_DATA (no data flip) |
| overall parity bit 7 | 0 | yes | CORRECTED_PARITY |
| data bits 0,1 (pos 3,5) | 6 | no | UNCORRECTABLE |
| check bits 0,3,6 (pos 1,8,64) | 73 | yes | UNCORRECTABLE |

*Derived* from §3.3 (regression only; not in the docs): `enc(0x0)=0x00`, `enc(0xFFFF_FFFF_FFFF_FFFF)=0xFF`, `enc(0x1)=0x83`, `enc(0x8000_0000_0000_0000)=0xC7`.

**G6. Partial write merge (MAS Table 6-155).** Lane BE / decode / counted:
- L0: 0xFF / CLEAN / –
- L1: 0x0F / CLEAN / –
- L2: 0x00 / CORRECTED_DATA / corrected
- L3: 0xFF / UNCORRECTABLE / corrected (healed)

The result is that the write proceeds and ce_codewords = 2. HAS §6.2.19: with BE bits 0 and 3 set, only bytes 0 and 3 change.

**G7. Scrub (MAS Table 6-164; HAS §9).** P=18 gives U = base = 16. Writes, in order:
1. Data granules 0..7.
2. ECC granule 16 (records of 0..7).
3. Data granules 8..15.
4. ECC granule 17 (records of 8..15).
5. DRAIN, then complete.

Total = 18 = 16 + 16/8. HAS: U=16 gives 16 data writes and 2 companion writes.

**G8. Generation and hazard (HAS §6.2.4/6.2.5; MAS Table 6-89).**
- An event for slot 3 with generation 5 is ignored after slot 3 is reused at generation 6.
- Two reads to the same granule: no conflict. A read then a write: conflict until the read closes.
- MAS A..F set: A(gtag1, 0x100, W), B(2, 0x100, R) waits for A, C(3, 0x100, R) waits for A, D(1 seq1, 0x200, W) waits for A, E(4, 0x300, R) waits for none, F(5, 0x100, W, poisoned) waits for none.

**G9. Timing (MAS Table 6-102, DDR3-1600 11-11-11, times in tCK).**
- ACT at 100 gives rd_dl = 111.
- tRRD 6 / tFAW 32: ACTs at 100, 106, 112, 118. A fifth ACT at 124 is illegal (FAW) and becomes legal at 132.
- WR on rank 0 at 100: rd_dl = 118, wr_dl = 104, sw[0] = 106. A WR on rank 1 is legal at 106.
- SRX at 1000: ACT/REF/PRE/ZQ legal from 1216. RD/WR legal from 1512.
- RD at 100 gives pde_dl = 116.

**G10. Timing commit consistency (*derived* from MAS Table 5-15 vs Table 5-20).** `T_RTW_min = CL+6−CWL` gives 7/8/9/9 for DDR3-800/1066/1333/1600, which equals the reset T_RTW values 7/8/9/9. CL/CWL reset pairs: 6/5, 8/6, 10/7, 11/8. Negative test: CL=12 is rejected (reserved).

**G11. DFI reset image (MAS §6.2.12).** CL 11, CWL 8, AL 0, PWL 2, PRL 6, G 0 gives tphy_wrlat 4 and trddata_en 13, and `floor((3+13+3)/2)+1 = 10`. The READ_TIMEOUT minimum with these values plus tphy_rdlat 256 and margin 1 is 10+13+256+4+1 = **284** (*derived*), which is ≤ the reset value 1024.

**G12. Write drain (HAS §6.2.9; MAS Table 6-115).**
- HAS: enter=8, exit=4. Drain is active at occupancy 9, still active at 6, and exits at 4.
- RS04 with D=16 gives enter 8, exit 4.
- MAS max=3: three accepted WRs cause a budget exit, followed by a 3-cycle cool-down. Re-entry happens at cycle 11.

**G13. Quarantine walk (MAS Table 6-138).**
- Setup: head timeout with contexts 5 and 7 open, reads 9 and 11 waiting, RMW parent 12.
- First cause latched: {5, 2, RD_DATA, 0, deadline}.
- 5 and 7 close with SLVERR. 12 is notified with SLVERR and no write. 9 and 11 get SLVERR NOP terminals and SLVERR zero-data returns.
- A later late return is discarded and the first cause is unchanged.
