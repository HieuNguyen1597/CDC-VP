// SPDX-License-Identifier: Apache-2.0
// ECC, startup scrub, read quarantine, fatal containment and reset regressions with live
// memory traffic (subtask 3). Expected ECC classes and syndromes follow MAS Tables
// 6-153/6-154/6-155 (audit G5/G6); the quarantine walk follows MAS Table 6-138 (G13).
#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "ddr3_bench.h"

#include <random>

using namespace fx1_ddr3_test;
using namespace fx1::ddr3;

namespace {

constexpr std::uint32_t kBase = 0x80000000u;

AxiRequest make(std::uint32_t addr, AxiBurst burst, unsigned len, unsigned size, bool write, unsigned id = 0) {
    AxiRequest r;
    r.addr = addr;
    r.burst = burst;
    r.len = static_cast<std::uint8_t>(len);
    r.size = static_cast<std::uint8_t>(size);
    r.write = write;
    r.id = static_cast<std::uint8_t>(id);
    return r;
}

std::vector<std::uint8_t> pattern(std::size_t n, unsigned seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i * 29u + seed * 13u + 3u);
    return v;
}

struct Join {
    int pending = 0;
    sc_core::sc_event ev;
    void spawn(std::function<void()> fn) {
        ++pending;
        sc_core::sc_spawn([this, fn] {
            fn();
            --pending;
            ev.notify(sc_core::SC_ZERO_TIME);
        });
    }
    void wait() {
        while (pending) sc_core::wait(ev);
    }
};

// Stored bytes; empty when the debug access is refused (no committed topology).
std::vector<std::uint8_t> raw(Ddr3Controller& d, std::uint64_t off, std::size_t n) {
    std::vector<std::uint8_t> v(n);
    if (!d.debug_read(off, v.data(), n)) v.clear();
    return v;
}

// One 32-byte granule write with per-byte strobes (two 16-byte beats on AXI128).
AxiOut write_granule(MemPort& m, std::uint32_t off, const std::vector<std::uint8_t>& data, std::uint32_t mask) {
    const unsigned bus = m.bus;
    AxiRequest r = make(kBase + off, AxiBurst::Incr, 32 / bus - 1, bus == 16 ? 4 : 3, true);
    std::vector<std::uint8_t> be(32);
    for (unsigned b = 0; b < 32; ++b) be[b] = (mask >> b) & 1u ? TLM_BYTE_ENABLED : TLM_BYTE_DISABLED;
    return m.axi(r, data, be);
}

// ------------------------------------------------------------ ECC events and RMW
void scenario_ecc(Bench& b) {
    expect(boot_memory(b), "boot");
    Ddr3Controller& d = *b.ctrl;
    CsrPort& c = b.csr;
    MemPort& m = *b.mem[0];
    std::vector<std::uint8_t> got;
    const std::uint32_t cap = b.snap(FX1_DDR3_ECC_CAP_STATUS);
    expect((cap & FX1_DDR3_ECC_CAP_PRESENT) && ((cap >> 9) & 0xFu) == 4 && ((cap >> 13) & 0xFFu) == 8 &&
               !(cap & FX1_DDR3_ECC_CAP_RMW_BUSY) && FX1_DDR3_ECC_CAP_LOCKS(cap) == 0,
           "ECC_CAP_STATUS: ECC, 4 codewords, 8 records, idle");
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);

    // 1. Single data-bit error: corrected data, OKAY, counted on every read (no write-back).
    const std::vector<std::uint8_t> p0 = pattern(32, 0);
    m.write(kBase + 0x4000, p0);
    d.fault_flip_data_bit(0x200, 10);
    expect(m.read(kBase + 0x4000, got, 32) == AxiResp::Okay && got == p0, "CE: corrected data with OKAY (BE01)");
    expect_eq(b.snap(FX1_DDR3_CE_COUNT), 1u, "CE_COUNT 1");
    expect_eq(c.rd(FX1_DDR3_ECC_LAST_ADDR_LO), 0x200u, "ECC_LAST_ADDR = physical data granule 0x200");
    expect_eq(c.rd(FX1_DDR3_ECC_LAST_INFO), FX1_DDR3_ECC_INFO_TYPE_CE | (0x8Fu << FX1_DDR3_ECC_INFO_SYNDROME_SHIFT),
              "ECC_LAST_INFO: CE, codeword 0, {mismatch, syndrome 15}");
    expect(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_CE_THRESHOLD, "IRQ_RAW[0]: CE_COUNT reached CE_THRESHOLD (1)");
    m.read(kBase + 0x4000, got, 32);
    expect(b.snap(FX1_DDR3_CE_COUNT) == 2 && raw(d, 0x4000, 32) != p0, "CE counted again: no write-back (RS05)");
    // Threshold crossing (GAP-BE-16): IRQ once when old < THR <= new.
    expect(c.ok(FX1_DDR3_CE_THRESHOLD, 5), "CE_THRESHOLD = 5");
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_CE_THRESHOLD);
    m.read(kBase + 0x4000, got, 32);
    m.read(kBase + 0x4000, got, 32);
    const bool below = !(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_CE_THRESHOLD);
    m.read(kBase + 0x4000, got, 32);
    expect(below && (c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_CE_THRESHOLD) && b.snap(FX1_DDR3_CE_COUNT) == 5,
           "IRQ_RAW[0] stays low at 4 and rises at 5");

    // 2. Double-bit error in lane 1: SLVERR, zero data, UE event.
    const std::vector<std::uint8_t> p1 = pattern(32, 1);
    m.write(kBase + 0x4020, p1);
    d.fault_flip_data_bit(0x201, 64 + 3);
    d.fault_flip_data_bit(0x201, 64 + 9);
    AxiOut o = m.axi(make(kBase + 0x4020, AxiBurst::Incr, 1, 4, false));
    bool zero = true;
    for (auto v : o.data) zero &= v == 0;
    expect(o.resp == AxiResp::SlvErr && zero && o.beats[0] == AxiResp::SlvErr, "UE: SLVERR with zero data (BE01, RS05)");
    expect(b.snap(FX1_DDR3_UE_COUNT) == 1 && (c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_ECC_UE), "UE_COUNT 1, IRQ_RAW[1]");
    // Sticky forward (SOQ-23): the clean granule after the UE one still answers SLVERR.
    o = m.axi(make(kBase + 0x4020, AxiBurst::Incr, 3, 4, false));
    expect(o.beats.size() == 4 && o.beats[1] == AxiResp::SlvErr && o.beats[2] == AxiResp::SlvErr &&
               o.beats[3] == AxiResp::SlvErr && o.data[40] == 0,
           "beats after the UE beat carry SLVERR and zero data");
    const std::uint32_t info = c.rd(FX1_DDR3_ECC_LAST_INFO);
    expect((info & 3u) == FX1_DDR3_ECC_INFO_TYPE_UE && ((info >> 2) & 3u) == 1 && c.rd(FX1_DDR3_ECC_LAST_ADDR_LO) == 0x201,
           "ECC_LAST_INFO: UE, codeword 1, granule 0x201");

    // 3. Partial write with a UE in a needed lane: SLVERR, no write (BE02, ECC-CT-01).
    const std::vector<std::uint8_t> p2 = pattern(32, 2);
    m.write(kBase + 0x4040, p2);
    d.fault_flip_data_bit(0x202, 128 + 1);
    d.fault_flip_data_bit(0x202, 128 + 2);
    const std::vector<std::uint8_t> before = raw(d, 0x4040, 32);
    const std::uint32_t ue_pre = b.snap(FX1_DDR3_UE_COUNT);
    o = write_granule(m, 0x4040, std::vector<std::uint8_t>(32, 0xAA), 0x0000000Fu);
    expect(o.resp == AxiResp::SlvErr && raw(d, 0x4040, 32) == before && b.snap(FX1_DDR3_UE_COUNT) == ue_pre + 1,
           "UE in a needed lane: B = SLVERR, DRAM unchanged, one UE event");

    // 4. G6 / MAS Table 6-155: L0 BE 0xFF clean, L1 0x0F clean, L2 0x00 CE, L3 0xFF UE.
    const std::vector<std::uint8_t> p3 = pattern(32, 3);
    m.write(kBase + 0x4060, p3);
    d.fault_flip_data_bit(0x203, 128 + 5);
    d.fault_flip_data_bit(0x203, 192 + 0);
    d.fault_flip_data_bit(0x203, 192 + 1);
    const std::uint32_t ce_before = b.snap(FX1_DDR3_CE_COUNT);
    const std::vector<std::uint8_t> nw(32, 0xC3);
    o = write_granule(m, 0x4060, nw, 0xFF000FFFu);
    expect(o.resp == AxiResp::Okay && b.snap(FX1_DDR3_CE_COUNT) == ce_before + 2,
           "G6: the write proceeds, ce_codewords = 2 (corrected L2 + healed L3)");
    std::vector<std::uint8_t> want = p3;
    for (unsigned i = 0; i < 32; ++i)
        if (i < 12 || i >= 24) want[i] = 0xC3;
    expect(m.read(kBase + 0x4060, got, 32) == AxiResp::Okay && got == want && b.snap(FX1_DDR3_CE_COUNT) == ce_before + 2,
           "G6: merged data re-encoded; the read is clean");

    // 5. A full write never decodes the old data: no event even over a UE.
    const std::uint32_t ue_before = b.snap(FX1_DDR3_UE_COUNT);
    expect(m.write(kBase + 0x4020, p1) == AxiResp::Okay && m.read(kBase + 0x4020, got, 32) == AxiResp::Okay && got == p1 &&
               b.snap(FX1_DDR3_UE_COUNT) == ue_before,
           "full write over a UE granule: OKAY, no event, clean afterwards (GAP-BE-17)");

    // 6. Check-bit and overall-parity errors are corrected and counted.
    const std::vector<std::uint8_t> p4 = pattern(32, 4);
    m.write(kBase + 0x4080, p4);
    d.fault_flip_check_bit(0x204, 3, 3);
    std::uint32_t ce = b.snap(FX1_DDR3_CE_COUNT);
    expect(m.read(kBase + 0x4080, got, 32) == AxiResp::Okay && got == p4 && b.snap(FX1_DDR3_CE_COUNT) == ce + 1 &&
               c.rd(FX1_DDR3_ECC_LAST_INFO) == (FX1_DDR3_ECC_INFO_TYPE_CE | (3u << 2) | (0x88u << 8)),
           "check bit 3 of lane 3: CORRECTED_DATA, syndrome 8 (C-18)");
    m.write(kBase + 0x40A0, p4);
    d.fault_flip_check_bit(0x205, 0, 7);
    ce = b.snap(FX1_DDR3_CE_COUNT);
    expect(m.read(kBase + 0x40A0, got, 32) == AxiResp::Okay && got == p4 && b.snap(FX1_DDR3_CE_COUNT) == ce + 1,
           "overall parity bit: CORRECTED_PARITY, counted");

    // 7. Sibling records of one companion granule are untouched by a record write.
    const std::vector<std::uint8_t> eight = pattern(256, 5);
    m.write(kBase + 0x4100, eight);
    write_granule(m, 0x4140, std::vector<std::uint8_t>(32, 0x11), 0x1u);
    ce = b.snap(FX1_DDR3_CE_COUNT);
    std::vector<std::uint8_t> want8 = eight;
    want8[0x40] = 0x11;
    expect(m.read(kBase + 0x4100, got, 256) == AxiResp::Okay && got == want8 && b.snap(FX1_DDR3_CE_COUNT) == ce &&
               b.snap(FX1_DDR3_UE_COUNT) == ue_before,
           "slot write keeps the other seven records (BE-6.2.19-08)");

    // 8. ECC_CAP_STATUS live RMW busy / lock occupancy during a long ECC write.
    Join j;
    AxiOut w;
    j.spawn([&] { w = m.axi(make(kBase + 0x8000, AxiBurst::Incr, 255, 4, true), pattern(4096, 6)); });
    sc_core::wait(20, SC_NS);
    const std::uint32_t live = c.rd(FX1_DDR3_ECC_CAP_STATUS);
    const std::uint32_t busy = b.snap(FX1_DDR3_ECC_CAP_STATUS);
    j.wait();
    const std::uint32_t stale = c.rd(FX1_DDR3_ECC_CAP_STATUS);
    const std::uint32_t idle = b.snap(FX1_DDR3_ECC_CAP_STATUS);
    expect((busy & FX1_DDR3_ECC_CAP_RMW_BUSY) && FX1_DDR3_ECC_CAP_LOCKS(busy) > 0 && !(idle & FX1_DDR3_ECC_CAP_RMW_BUSY) &&
               FX1_DDR3_ECC_CAP_LOCKS(idle) == 0,
           "ECC_CAP_STATUS[1] RMW busy and [8:2] locks captured while the write is in service");
    expect(!(live & FX1_DDR3_ECC_CAP_RMW_BUSY) && stale == busy, "the dynamic fields change only at a snapshot capture");
    expect(w.resp == AxiResp::Okay, "long ECC write OKAY");
}

void scenario_ecc_off(Bench& b) {
    expect(boot_memory(b), "boot without ECC (no scrub)");
    Ddr3Controller& d = *b.ctrl;
    MemPort& m = *b.mem[0];
    std::vector<std::uint8_t> got;
    const std::vector<std::uint8_t> p = pattern(32, 7);
    m.write(kBase + 0x100, p);
    write_granule(m, 0x100, std::vector<std::uint8_t>(32, 0xEE), 0x00010002u);
    std::vector<std::uint8_t> want = p;
    want[1] = 0xEE;
    want[16] = 0xEE;
    expect(m.read(kBase + 0x100, got, 32) == AxiResp::Okay && got == want, "partial write updates exactly the strobed bytes (BE-6.2.14-02)");
    expect(d.fault_flip_data_bit(0x8, 0) && !d.fault_flip_check_bit(0x8, 0, 0), "no companion record without ECC");
    want[0] ^= 1;
    expect(m.read(kBase + 0x100, got, 32) == AxiResp::Okay && got == want, "corrupted data is returned as stored (no codec)");
    expect(b.snap(FX1_DDR3_CE_COUNT) == 0 && b.csr.rd(FX1_DDR3_ECC_CAP_STATUS) == 0 && b.csr.rd(FX1_DDR3_CE_THRESHOLD) == 0,
           "ECC words read 0 without ECC (BE-6.2.17-07)");
}

// ------------------------------------------------------------ startup scrub
void scenario_scrub(Bench& b) {
    Ddr3Controller& d = *b.ctrl;
    CsrPort& c = b.csr;
    MemPort& m = *b.mem[0];
    const BuildConfig& cfg = d.config();
    d.set_bootstrap_region(0, 0, {kBase, 0x02000000u});
    d.qos_set_configured(0, true);
    expect(program_and_commit(b, cfg, 2, 12, 0), "program and commit");
    c.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE);
    const std::uint64_t top = d.geometry().usable_granules * 32u - 32u;
    const std::vector<std::uint8_t> p = pattern(32, 8);
    expect(d.debug_write(0, p.data(), 32), "preload before the scrub (accepted, warned)");
    expect(b.wait_until([&] { return d.scrub_progress() > d.scrub_total() / 2; }, sc_time(50, SC_MS), sc_time(20, SC_US)),
           "scrub past half of its writes");
    const std::uint64_t passed = d.scrub_progress();
    expect(d.debug_write(0x40, p.data(), 32) && d.debug_write(top, p.data(), 32), "preload behind and ahead of the frontier");
    expect(d.scrub_progress() >= passed && !d.mission_ready(), "mission readiness closed while scrubbing (BE03)");
    expect(wait_mission(b, sc_time(50, SC_MS)), "scrub completes, mission ready");
    expect(d.scrub_progress() == d.scrub_total() && d.scrub_total() == d.geometry().usable_granules * 9 / 8,
           "N_total = U + U/8 writes (HAS Table 9-3)");
    expect(raw(d, 0, 32) == std::vector<std::uint8_t>(32, 0), "data preloaded before the frontier passed is erased");
    {
        std::vector<std::uint8_t> z;
        expect(m.read(kBase, z, 32) == AxiResp::Okay && z == std::vector<std::uint8_t>(32, 0) &&
                   b.snap(FX1_DDR3_CE_COUNT) == 0 && b.snap(FX1_DDR3_UE_COUNT) == 0,
               "its companion record was erased too: the granule reads zero, CLEAN");
    }
    expect(raw(d, 0x40, 32) == p, "data preloaded behind the frontier survives");
    expect(raw(d, top, 32) == std::vector<std::uint8_t>(32, 0), "data ahead of the frontier is erased when it passes");
    expect(d.resident_pages() <= 2, "scrub allocated no storage (frontier, dropped pages)");
    // Every user granule reads back zero and CLEAN (BE-6.2.20-10).
    std::mt19937 rng(5);
    bool clean = true;
    std::vector<std::uint8_t> got;
    for (unsigned i = 0; i < 40; ++i) {
        const std::uint32_t off = static_cast<std::uint32_t>(rng() % 0x01F00000u) & ~31u;
        if (off == 0x40) continue;
        clean &= m.read(kBase + off, got, 32) == AxiResp::Okay && got == std::vector<std::uint8_t>(32, 0);
    }
    expect(clean && b.snap(FX1_DDR3_CE_COUNT) == 0 && b.snap(FX1_DDR3_UE_COUNT) == 0, "scrubbed granules read zero, CLEAN");
    // Preload after readiness: consistent ECC.
    expect(d.debug_write(0x100, p.data(), 32) && m.read(kBase + 0x100, got, 32) == AxiResp::Okay && got == p &&
               b.snap(FX1_DDR3_CE_COUNT) == 0,
           "preload after the scrub: data with consistent ECC");
    // A topology publication re-initializes and re-scrubs from granule 0 (BE05, RS05).
    expect(c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST) &&
               (c.rd(FX1_DDR3_QUIESCE_STATUS) & FX1_DDR3_QUIESCE_ST_COMPLETE),
           "global quiesce complete");
    expect(c.ok(FX1_DDR3_TOPO_COMMIT, 1) && !d.mission_ready(), "TOPO_COMMIT restarts initialization");
    expect(b.wait_until([&] { return d.scrub_progress() > 0; }, sc_time(5, SC_MS), sc_time(1, SC_US)) &&
               d.scrub_progress() < d.scrub_total() / 4,
           "the new sweep starts again at granule 0");
    // Readiness loss mid-sweep (core reset): progress returns to 0 (BE-6.2.20-07); the scrub
    // writes already accepted stay applied (no rollback).
    // Preload one group ahead of the frontier, let the frontier pass it, then abort.
    const std::uint64_t group = d.scrub_progress() / 9 + 200;  // write index of its data: group * 9
    const std::uint64_t ahead = group * 8 * 32;
    expect(d.scrub_progress() < group * 9 && d.debug_write(ahead, p.data(), 32), "preload ahead of the new frontier");
    expect(b.wait_until([&] { return d.scrub_progress() > group * 9 + 9; }, sc_time(5, SC_MS), sc_time(100, SC_NS)),
           "frontier passed it");
    d.set_core_reset(true);
    d.set_core_reset(false);
    expect(d.scrub_progress() == 0 && !d.scrub_complete(), "core reset aborts the sweep; no resume checkpoint");
    expect(raw(d, 0x40, 32).empty(), "no debug access without a committed topology");
    expect(program_and_commit(b, cfg, 2, 12, 0), "recommit");
    expect(!d.scrub_running() && raw(d, ahead, 32) == std::vector<std::uint8_t>(32, 0),
           "scrub writes issued before the abort remain applied (seen before the next sweep)");
    expect(c.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE) && wait_mission(b, sc_time(50, SC_MS)),
           "scrub again to readiness");
    expect(raw(d, 0x100, 32) == std::vector<std::uint8_t>(32, 0) && raw(d, 0x40, 32) == std::vector<std::uint8_t>(32, 0),
           "re-scrub erased the earlier data");
}

// ------------------------------------------------------------ read quarantine
void scenario_quarantine(Bench& b) {
    expect(boot_memory(b), "boot");
    Ddr3Controller& d = *b.ctrl;
    CsrPort& c = b.csr;
    MemPort& m = *b.mem[0];
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
    const std::vector<std::uint8_t> p = pattern(64, 9);
    m.write(kBase, p);
    d.fault_read_return(Ddr3Controller::ReturnFault::Timeout);
    AxiOut o = m.axi(make(kBase, AxiBurst::Incr, 1, 4, false));
    bool zero = true;
    for (auto v : o.data) zero &= v == 0;
    // READ_TIMEOUT reset value 1024 tCK = 1.28 us at DDR3-1600.
    expect(o.resp == AxiResp::SlvErr && zero && o.end - o.start >= sc_time(1280, SC_NS),
           "lost return: SLVERR with zero data at the head deadline");
    const std::uint32_t st = c.rd(FX1_DDR3_QUARANTINE_STATUS);
    expect((st & 3u) == FX1_DDR3_QUAR_ST_ACTIVE && FX1_DDR3_QUAR_ST_REASON(st) == FX1_DDR3_QUAR_REASON_TIMEOUT &&
               FX1_DDR3_QUAR_ST_OPEN_CTX(st) == 0,
           "QUARANTINE_STATUS: active, reason 0 (timeout), no open context left");
    const std::uint32_t lo = c.rd(FX1_DDR3_QUARANTINE_FIRST_REQ_LO), hi = c.rd(FX1_DDR3_QUARANTINE_FIRST_REQ_HI);
    expect((lo & FX1_DDR3_QUAR_FIRST_VALID) && FX1_DDR3_QUAR_FIRST_REASON(lo) == 0 &&
               FX1_DDR3_QUAR_FIRST_KIND(lo) == FX1_DDR3_QUAR_KIND_RD_ECC && FX1_DDR3_QUAR_FIRST_INDEX(lo) < 16 &&
               FX1_DDR3_QUAR_FIRST_GEN(lo) != 0 && hi != 0 && (lo & 0x080000C0u) == 0,
           "QUARANTINE_FIRST_REQ: valid, reason, kind RD_ECC, index, generation, deadline (MAS T6-137)");
    const std::uint32_t irq = c.rd(FX1_DDR3_IRQ_RAW);
    expect((irq & FX1_DDR3_IRQ_READ_TIMEOUT) && !(irq & FX1_DDR3_IRQ_QUARANTINE), "IRQ_RAW[2] (timeout), not [3]");
    o = m.axi(make(kBase, AxiBurst::Incr, 0, 4, false));
    expect(o.resp == AxiResp::SlvErr && o.end - o.start < sc_time(50, SC_NS), "new reads answer SLVERR at once (not mapped)");
    const std::vector<std::uint8_t> before = raw(d, 0, 64);
    expect(m.write(kBase, pattern(32, 1)) == AxiResp::SlvErr && raw(d, 0, 64) == before,
           "ECC writes need an internal read: SLVERR, no write (MAS T6-136)");
    // S3-R3: writes that emit no backend atom complete locally with OKAY.
    {
        AxiRequest masked = make(kBase, AxiBurst::Incr, 1, 4, true);
        const AxiOut mo = m.axi(masked, std::vector<std::uint8_t>(32, 0x55), std::vector<std::uint8_t>(32, TLM_BYTE_DISABLED));
        AxiRequest xw = make(kBase, AxiBurst::Incr, 0, 3, true, 4);
        xw.exclusive = true;
        const AxiOut xo = m.axi(xw, std::vector<std::uint8_t>(16, 0x66));
        expect(mo.resp == AxiResp::Okay && xo.resp == AxiResp::Okay && raw(d, 0, 64) == before,
               "fully masked write and exclusive miss: local OKAY under quarantine, memory unchanged");
    }
    d.fault_read_return(Ddr3Controller::ReturnFault::Unexpected);
    expect(c.rd(FX1_DDR3_QUARANTINE_FIRST_REQ_LO) == lo && c.rd(FX1_DDR3_QUARANTINE_FIRST_REQ_HI) == hi,
           "a later fault does not overwrite the first cause (RS10)");
    // Sticky across a core reset; cleared by cold reset only.
    d.set_core_reset(true);
    d.set_core_reset(false);
    expect(d.quarantined() && c.rd(FX1_DDR3_QUARANTINE_STATUS) != 0, "quarantine survives a core reset");
    d.set_cold_reset(true);
    d.set_cold_reset(false);
    expect(!d.quarantined() && c.rd(FX1_DDR3_QUARANTINE_STATUS) == 0 && c.rd(FX1_DDR3_QUARANTINE_FIRST_REQ_LO) == 0,
           "cold reset clears the quarantine and its record");
    d.fault_clear();
    expect(boot_memory(b), "boot again");
    expect(m.read(kBase, o.data, 32) == AxiResp::Okay, "reads work after the coordinated recovery (BE-6.2.11-08)");
}

// G13 / MAS Table 6-138: reads and an RMW parent in flight when the head deadline expires.
void scenario_quarantine_walk(Bench& b) {
    expect(boot_memory(b), "boot");
    Ddr3Controller& d = *b.ctrl;
    MemPort& m0 = *b.mem[0];
    MemPort& m1 = *b.mem[1];
    m0.write(kBase, pattern(32, 1));
    m0.write(kBase + 0x1000, pattern(32, 2));
    m0.write(kBase + 0x2000, pattern(32, 3));
    const std::vector<std::uint8_t> before = raw(d, 0x2000, 32);
    d.fault_read_return(Ddr3Controller::ReturnFault::Timeout);
    Join j;
    AxiOut a, bb, cc, rmw;
    const sc_time t0 = sc_core::sc_time_stamp();
    j.spawn([&] { a = m0.axi(make(kBase, AxiBurst::Incr, 1, 4, false, 1)); });
    j.spawn([&] {
        sc_core::wait(100, SC_NS);
        cc = m1.axi(make(kBase + 0x1000, AxiBurst::Incr, 1, 4, false, 3));
    });
    j.spawn([&] {
        sc_core::wait(1275, SC_NS);  // returns after the deadline at t0 + 1280 ns
        bb = m1.axi(make(kBase + 0x1000, AxiBurst::Incr, 1, 4, false, 1));
    });
    j.spawn([&] {
        sc_core::wait(1272, SC_NS);  // its internal reads finish after the deadline
        rmw = write_granule(m1, 0x2000, std::vector<std::uint8_t>(32, 0x77), 0xFu);
    });
    j.wait();
    expect(a.resp == AxiResp::SlvErr && a.end - t0 >= sc_time(1280, SC_NS), "head read: SLVERR at the deadline");
    expect(cc.resp == AxiResp::Okay && cc.data[0] == pattern(32, 2)[0], "a read returned before the deadline completes");
    expect(bb.resp == AxiResp::SlvErr && bb.data[0] == 0, "an open read whose return comes later: SLVERR, zero data");
    expect(rmw.resp == AxiResp::SlvErr && raw(d, 0x2000, 32) == before, "RMW parent: B = SLVERR, no write");
    expect(FX1_DDR3_QUAR_FIRST_KIND(b.csr.rd(FX1_DDR3_QUARANTINE_FIRST_REQ_LO)) == FX1_DDR3_QUAR_KIND_RD_ECC,
           "first cause is the head read");
}

void scenario_quarantine_reasons(Bench& b) {
    Ddr3Controller& d = *b.ctrl;
    CsrPort& c = b.csr;
    MemPort& m = *b.mem[0];
    struct Case {
        Ddr3Controller::ReturnFault f;
        unsigned reason;
    };
    const Case cases[] = {{Ddr3Controller::ReturnFault::Malformed, FX1_DDR3_QUAR_REASON_MALFORMED},
                          {Ddr3Controller::ReturnFault::ContextMismatch, FX1_DDR3_QUAR_REASON_CONTEXT},
                          {Ddr3Controller::ReturnFault::GenerationMismatch, FX1_DDR3_QUAR_REASON_GENERATION},
                          {Ddr3Controller::ReturnFault::Unexpected, FX1_DDR3_QUAR_REASON_UNEXPECTED}};
    for (const Case& k : cases) {
        expect(boot_memory(b), "boot (no ECC)");
        c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
        const std::vector<std::uint8_t> p = pattern(32, k.reason);
        m.write(kBase, p);
        d.fault_read_return(k.f);
        std::vector<std::uint8_t> got;
        const AxiResp r = m.read(kBase, got, 32);
        const std::uint32_t lo = c.rd(FX1_DDR3_QUARANTINE_FIRST_REQ_LO);
        const std::string n = "reason " + std::to_string(k.reason);
        expect(r == AxiResp::SlvErr && FX1_DDR3_QUAR_ST_REASON(c.rd(FX1_DDR3_QUARANTINE_STATUS)) == k.reason &&
                   FX1_DDR3_QUAR_FIRST_REASON(lo) == k.reason && (lo & FX1_DDR3_QUAR_FIRST_VALID),
               n + ": read SLVERR, status and first cause");
        expect((c.rd(FX1_DDR3_IRQ_RAW) & (FX1_DDR3_IRQ_QUARANTINE | FX1_DDR3_IRQ_READ_TIMEOUT)) == FX1_DDR3_IRQ_QUARANTINE,
               n + ": IRQ_RAW[3]");
        if (k.f == Ddr3Controller::ReturnFault::Unexpected)
            expect(lo == (FX1_DDR3_QUAR_FIRST_VALID | (FX1_DDR3_QUAR_REASON_UNEXPECTED << 28)) &&
                       c.rd(FX1_DDR3_QUARANTINE_FIRST_REQ_HI) == 0,
                   "tokenless return: reason and zero identity, valid = 1");
        else
            expect(FX1_DDR3_QUAR_FIRST_KIND(lo) == FX1_DDR3_QUAR_KIND_RD_DATA, n + ": kind RD_DATA");
        // Writes needing no old-data read continue (no ECC here).
        const std::vector<std::uint8_t> q = pattern(32, 50 + k.reason);
        expect(m.write(kBase + 0x100, q) == AxiResp::Okay && raw(d, 0x100, 32) == q, n + ": writes continue");
        d.set_cold_reset(true);
        d.set_cold_reset(false);
        d.fault_clear();
    }
}

// ------------------------------------------------------------ fatal containment
void scenario_fatal(Bench& b) {
    expect(boot_memory(b), "boot");
    Ddr3Controller& d = *b.ctrl;
    CsrPort& c = b.csr;
    MemPort& m = *b.mem[0];
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
    const std::vector<std::uint8_t> nw = pattern(32, 4);
    d.fault_write_result(1);
    expect(m.write(kBase, nw) == AxiResp::SlvErr, "ECC write with a write-result error: SLVERR");
    expect((c.rd(FX1_DDR3_FATAL_STATUS) & FX1_DDR3_FATAL_ECC_HOLD) && (c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_FATAL) &&
               d.fatal() && !d.mission_ready(),
           "post-launch failure: ECC fatal hold, IRQ_RAW[11], admission closed (BE02)");
    expect(raw(d, 0, 32) == nw, "the issued write is not rolled back");
    expect(FX1_DDR3_ECC_CAP_LOCKS(b.snap(FX1_DDR3_ECC_CAP_STATUS)) == 1, "the granule lock stays held");
    expect(!c.ok(FX1_DDR3_FATAL_STATUS, 0) && c.rd(FX1_DDR3_FATAL_STATUS) != 0, "FATAL_STATUS has no software clear");
    Join j;
    AxiOut r;
    j.spawn([&] { r = m.axi(make(kBase + 0x100, AxiBurst::Incr, 0, 4, false)); });
    sc_core::wait(10, SC_US);
    expect(r.status == tlm::TLM_INCOMPLETE_RESPONSE, "a new request is held (admission closed, not failed)");
    d.core_reset_request(true);
    expect(d.core_reset_safe(), "a request held by the fatal hold does not block reset-safe");
    d.set_core_reset(true);
    d.set_core_reset(false);
    d.core_reset_request(false);
    j.wait();
    expect(r.resp == AxiResp::SlvErr, "the held request is aborted by the core reset (GAP-FE-21)");
    expect(program_and_commit(b, d.config(), 2, 12, 0), "recommit after the core reset");
    sc_core::wait(5, SC_MS);
    expect(d.fatal() && !d.mission_ready() && c.rd(FX1_DDR3_FATAL_STATUS) != 0, "fatal survives a core reset");
    d.set_cold_reset(true);
    d.set_cold_reset(false);
    expect(!d.fatal() && c.rd(FX1_DDR3_FATAL_STATUS) == 0, "cold reset clears FATAL_STATUS");
    expect(boot_memory(b), "boot after the cold reset");
    d.fault_fatal(Ddr3Controller::FatalFault::DfiProtocol);
    expect((c.rd(FX1_DDR3_FATAL_STATUS) & FX1_DDR3_FATAL_DFI_PROTOCOL) && !d.mission_ready(), "DFI protocol fault: FATAL_STATUS[3]");
    d.fault_fatal(Ddr3Controller::FatalFault::WriteResultUnmatched);
    expect(c.rd(FX1_DDR3_FATAL_STATUS) == (FX1_DDR3_FATAL_DFI_PROTOCOL | FX1_DDR3_FATAL_WR_UNMATCHED),
           "FATAL_STATUS is a sticky bitmap");
    // Scrub fatal (BE03): readiness never opens.
    d.set_cold_reset(true);
    d.set_cold_reset(false);
    d.fault_fatal(Ddr3Controller::FatalFault::ScrubMapping);
    d.set_bootstrap_region(0, 0, {kBase, 0x02000000u});
    d.qos_set_configured(0, true);
    expect(program_and_commit(b, d.config(), 2, 12, 0) && c.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE),
           "program with an armed scrub fault");
    expect(!wait_mission(b, sc_time(20, SC_MS)) && c.rd(FX1_DDR3_FATAL_STATUS) == FX1_DDR3_FATAL_SCRUB && !d.scrub_complete(),
           "scrub fatal: FATAL_STATUS[1], mission readiness stays 0");
}

// S3-R1: work that has not launched when a fatal event occurs is contained (held, never
// completed) until a reset aborts it; contained work does not hold off reset-safe; a cold
// reset and re-initialization recover. Waits: granule lock, request slot, and (ECC) the
// launch of the write after its internal reads.
void scenario_fatal_containment(Bench& b) {
    Ddr3Controller& d = *b.ctrl;
    const bool ecc = d.config().ecc_enable;
    MemPort& m0 = *b.mem[0];
    MemPort& m1 = *b.mem[1];
    enum Case { LockWait, SlotWait, PreWrite };
    for (Case k : {LockWait, SlotWait, PreWrite}) {
        if (k == PreWrite && !ecc) continue;  // without ECC a write applies at its launch
        const std::string n = k == LockWait ? "lock wait" : k == SlotWait ? "slot wait" : "pre-write";
        expect(d.mission_ready() || boot_memory(b), n + ": ready");
        const std::uint32_t target = 0x200000;
        const std::vector<std::uint8_t> old_v = raw(d, target, 8);
        Join j;
        std::vector<AxiOut> reads(16);
        AxiOut w;
        if (k == LockWait) {
            j.spawn([&] { reads[0] = m0.axi(make(kBase + target, AxiBurst::Incr, 255, 4, false, 1)); });
        } else if (k == SlotWait) {
            for (unsigned i = 0; i < 16; ++i)
                j.spawn([&, i] { reads[i] = m0.axi(make(kBase + 0x10000 * i, AxiBurst::Incr, 255, 4, false, i)); });
        }
        if (k == PreWrite) {
            j.spawn([&] { w = write_granule(m1, target, std::vector<std::uint8_t>(32, 0x77), 0xFFu); });
        } else {
            j.spawn([&] {
                sc_core::wait(1, SC_NS);
                w = m1.axi(make(kBase + target, AxiBurst::Incr, 0, 3, true, 2), std::vector<std::uint8_t>(16, 0x77));
            });
        }
        sc_core::wait(5, SC_NS);
        expect(w.status == tlm::TLM_INCOMPLETE_RESPONSE, n + ": write still queued at the fault");
        d.fault_fatal(Ddr3Controller::FatalFault::DfiProtocol);
        const unsigned nreads = k == SlotWait ? 16u : k == LockWait ? 1u : 0u;
        auto reads_done = [&] {
            bool done = true;
            for (unsigned i = 0; i < nreads; ++i) done &= reads[i].status == tlm::TLM_OK_RESPONSE;
            return done;
        };
        d.core_reset_request(true);
        sc_core::wait(5, SC_NS);
        const bool safe_while_draining = d.core_reset_safe();
        d.core_reset_request(false);
        const bool reads_ok = b.wait_until(reads_done, sc_time(500, SC_US), sc_time(1, SC_US));
        sc_core::wait(2, SC_US);  // the pre-write case: its internal reads finish, then it is held
        if (nreads) expect(!safe_while_draining, n + ": launched reads still drain: not reset-safe yet");
        expect(d.fatal() && !d.mission_ready() && reads_ok, n + ": launched reads completed; fatal hold");
        expect(w.status == tlm::TLM_INCOMPLETE_RESPONSE && raw(d, target, 8) == old_v,
               n + ": the queued write never launched (held, memory unchanged)");
        d.core_reset_request(true);
        expect(d.core_reset_safe(), n + ": contained work does not hold off reset-safe");
        d.set_core_reset(true);
        d.set_core_reset(false);
        d.core_reset_request(false);
        j.wait();
        expect(w.resp == AxiResp::SlvErr && d.fe_drained(), n + ": the reset aborts the contained write");
        d.set_cold_reset(true);
        d.set_cold_reset(false);
        expect(boot_memory(b), n + ": cold reset and re-initialization recover");
        std::vector<std::uint8_t> got;
        expect(m0.read(kBase + target, got, 8) == AxiResp::Okay && got[0] != 0x77, n + ": no late write landed");
        expect(m1.write(kBase + target, pattern(8, 3)) == AxiResp::Okay && raw(d, target, 8) == pattern(8, 3),
               n + ": traffic works after recovery");
    }
}

// S3-R1a: transactions waiting behind a fatal-contained predecessor (same-ID chain, or R
// order across IDs) are contained too: reset-safe rises without forcing a reset.
void scenario_fatal_followers(Bench& b) {
    Ddr3Controller& d = *b.ctrl;
    MemPort& m0 = *b.mem[0];
    // Same ID: three writes with ID 0 (the platform bridge uses ID 0 for everything).
    expect(boot_memory(b), "boot");
    d.fault_fatal(Ddr3Controller::FatalFault::DfiProtocol);
    Join j;
    std::vector<AxiOut> w(3);
    for (unsigned i = 0; i < 3; ++i)
        j.spawn([&, i] {
            sc_core::wait(i, SC_NS);
            w[i] = m0.axi(make(kBase + 0x1000 * i, AxiBurst::Incr, 0, 3, true, 0), std::vector<std::uint8_t>(16, 0x5A));
        });
    sc_core::wait(10, SC_NS);
    d.core_reset_request(true);
    sc_core::wait(1, SC_US);
    bool pending = true;
    for (auto& o : w) pending &= o.status == tlm::TLM_INCOMPLETE_RESPONSE;
    expect(pending && d.core_reset_safe(), "same-ID followers of a contained write: reset-safe without a forced reset");
    d.set_core_reset(true);
    d.set_core_reset(false);
    d.core_reset_request(false);
    j.wait();
    bool aborted = true;
    for (auto& o : w) aborted &= o.resp == AxiResp::SlvErr;
    expect(aborted && d.fe_drained(), "the reset aborts all three");
    d.set_cold_reset(true);
    d.set_cold_reset(false);
    expect(boot_memory(b), "cold reset and re-initialization");
    expect(m0.write(kBase, pattern(16, 1)) == AxiResp::Okay && raw(d, 0, 16) == pattern(16, 1), "traffic after recovery");

    // R order: R1 (ID 1) waits for the granule lock of a launched long read on port 1 and is
    // contained by the fault; R2 (ID 2) on another granule launches, completes in the
    // backend and then waits for R1 in read-return order.
    MemPort& m1 = *b.mem[1];
    AxiOut lw, r1, r2;
    j.spawn([&] { lw = m1.axi(make(kBase + 0x8000, AxiBurst::Incr, 255, 4, false, 5)); });
    j.spawn([&] {
        sc_core::wait(1, SC_NS);
        r1 = m0.axi(make(kBase + 0x8000 + 0xFF0, AxiBurst::Incr, 0, 4, false, 1));
    });
    j.spawn([&] {
        sc_core::wait(2, SC_NS);
        r2 = m0.axi(make(kBase + 0x20000, AxiBurst::Incr, 0, 4, false, 2));
    });
    sc_core::wait(5, SC_NS);
    d.fault_fatal(Ddr3Controller::FatalFault::DfiProtocol);
    d.core_reset_request(true);
    expect(b.wait_until([&] { return lw.status == tlm::TLM_OK_RESPONSE; }, sc_time(200, SC_US), sc_time(1, SC_US)),
           "the launched long read completes");
    sc_core::wait(1, SC_US);
    expect(r1.status == tlm::TLM_INCOMPLETE_RESPONSE && r2.status == tlm::TLM_INCOMPLETE_RESPONSE && d.core_reset_safe(),
           "R-order follower of a contained read is contained: reset-safe");
    d.set_core_reset(true);
    d.set_core_reset(false);
    d.core_reset_request(false);
    j.wait();
    expect(r1.resp == AxiResp::SlvErr && r2.resp == AxiResp::SlvErr, "the reset aborts both reads");
    d.set_cold_reset(true);
    d.set_cold_reset(false);
    expect(boot_memory(b), "recovered again");
}

// S4-R2: every immediate fatal source, each from a clean controller (cold reset before),
// sets exactly its own FATAL_STATUS bit and IRQ_RAW[11], closes admission (INIT_STATUS[15],
// held traffic), survives a core reset, and clears only on a cold reset.
void scenario_fatal_sources(Bench& b) {
    Ddr3Controller& d = *b.ctrl;
    CsrPort& c = b.csr;
    struct Src {
        Ddr3Controller::FatalFault f;
        std::uint32_t bit;
        const char* name;
    };
    const Src srcs[] = {{Ddr3Controller::FatalFault::DfiProtocol, FX1_DDR3_FATAL_DFI_PROTOCOL, "DfiProtocol"},
                        {Ddr3Controller::FatalFault::WriteDataProtocol, FX1_DDR3_FATAL_WS_PROTOCOL, "WriteDataProtocol"},
                        {Ddr3Controller::FatalFault::WriteResultUnmatched, FX1_DDR3_FATAL_WR_UNMATCHED,
                         "WriteResultUnmatched"},
                        {Ddr3Controller::FatalFault::StaleLockRelease, FX1_DDR3_FATAL_ECC_HOLD, "StaleLockRelease"}};
    for (const Src& s : srcs) {
        const std::string n = s.name;
        d.set_cold_reset(true);
        d.set_cold_reset(false);
        expect(boot_memory(b), n + ": boot from a clean controller");
        c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
        expect(c.rd(FX1_DDR3_FATAL_STATUS) == 0 && !d.fatal() && !(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_FATAL),
               n + ": no fatal before the injection");
        d.fault_fatal(s.f);
        expect_eq(c.rd(FX1_DDR3_FATAL_STATUS), s.bit, n + ": FATAL_STATUS holds exactly its source bit");
        expect((c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_FATAL) && d.fatal() && !d.mission_ready() &&
                   (b.snap(FX1_DDR3_INIT_STATUS) & FX1_DDR3_INIT_ST_ADM_BLOCK),
               n + ": IRQ_RAW[11], readiness closed, admission blocked");
        Join j;
        AxiOut r;
        j.spawn([&] { r = b.mem[0]->axi(make(kBase, AxiBurst::Incr, 0, 4, false)); });
        sc_core::wait(5, SC_US);
        expect(r.status == tlm::TLM_INCOMPLETE_RESPONSE, n + ": new traffic held");
        d.set_core_reset(true);
        d.set_core_reset(false);
        j.wait();
        expect(r.resp == AxiResp::SlvErr && c.rd(FX1_DDR3_FATAL_STATUS) == s.bit && d.fatal(),
               n + ": a core reset aborts the held read; the source bit stays");
        d.set_cold_reset(true);
        d.set_cold_reset(false);
        expect(c.rd(FX1_DDR3_FATAL_STATUS) == 0 && !d.fatal(), n + ": cold reset clears it");
    }
    expect(boot_memory(b), "recovered after the last source");
}

void scenario_fatal_noecc(Bench& b) {
    expect(boot_memory(b), "boot (no ECC)");
    Ddr3Controller& d = *b.ctrl;
    d.fault_write_result(1);
    expect(b.mem[0]->write(kBase, pattern(32, 1)) == AxiResp::SlvErr && !d.fatal() && d.mission_ready(),
           "write-result error without ECC: SLVERR only");
    std::vector<std::uint8_t> got;
    expect(b.mem[0]->read(kBase, got, 32) == AxiResp::Okay, "traffic continues");
}

// ------------------------------------------------------------ resets with traffic
void scenario_reset(Bench& b) {
    expect(boot_memory(b), "boot (no ECC: retention is observable)");
    Ddr3Controller& d = *b.ctrl;
    MemPort& m0 = *b.mem[0];
    MemPort& m1 = *b.mem[1];
    const std::vector<std::uint8_t> keep = pattern(64, 2);
    m0.write(kBase, keep);
    {
        Join j;
        AxiOut r0, r1;
        j.spawn([&] { r0 = m0.axi(make(kBase + 0x1000, AxiBurst::Incr, 255, 4, false)); });
        j.spawn([&] { r1 = m1.axi(make(kBase + 0x2000, AxiBurst::Incr, 255, 4, false)); });
        b.at(sc_time(5, SC_NS), [&d] { d.set_port_reset(0, true); });
        b.at(sc_time(6, SC_NS), [&d] { d.set_port_reset(0, false); });
        j.wait();
        expect(r0.resp == AxiResp::SlvErr && r1.resp == AxiResp::Okay, "port 0 reset aborts its read only");
    }
    d.port_quiesce_request(0, true);
    expect(d.port_quiesce_ack(0), "no stale context after the abort");
    d.port_quiesce_request(0, false);
    {
        // LC-7.1-03: core reset-safe waits until every enabled port and the backend drained.
        // The read is held at the backend gate: only the port owns it.
        Join j;
        AxiOut r;
        b.csr.ok(FX1_DDR3_GLOBAL_CTRL, 0);
        j.spawn([&] { r = m1.axi(make(kBase + 0x2000, AxiBurst::Incr, 255, 4, false)); });
        sc_core::wait(2, SC_NS);
        d.core_reset_request(true);
        const bool safe_early = d.core_reset_safe();
        b.csr.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE);
        j.wait();
        sc_core::wait(sc_core::SC_ZERO_TIME);
        expect(!safe_early && d.core_reset_safe() && r.resp == AxiResp::Okay,
               "reset-safe only after the port's accepted read drained");
        d.core_reset_request(false);
    }
    {
        Join j;
        AxiOut w, held;
        b.csr.ok(FX1_DDR3_GLOBAL_CTRL, 0);  // the second request waits at the backend gate
        j.spawn([&] { held = m1.axi(make(kBase + 0x3000, AxiBurst::Incr, 0, 4, false)); });
        b.csr.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE);
        sc_core::wait(1, SC_NS);
        b.csr.ok(FX1_DDR3_GLOBAL_CTRL, 0);
        j.spawn([&] { w = m0.axi(make(kBase + 0x10000, AxiBurst::Incr, 255, 4, true), pattern(4096, 3)); });
        AxiOut stalled;
        j.spawn([&] { stalled = m1.axi(make(kBase + 0x3000, AxiBurst::Incr, 0, 4, false)); });
        b.at(sc_time(30, SC_NS), [&d] { d.set_core_reset(true); });
        b.at(sc_time(31, SC_NS), [&d] { d.set_core_reset(false); });
        j.wait();
        expect(stalled.resp == AxiResp::SlvErr, "a request held at the backend gate is aborted by the core reset");
        expect(held.resp == AxiResp::Okay || held.resp == AxiResp::SlvErr, "earlier request terminated");
    }
    expect(d.fe_drained() && !d.mission_ready(), "frontend drained, readiness lost after the core reset");
    expect(program_and_commit(b, d.config(), 2, 12, 0) && b.csr.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE) &&
               wait_mission(b, sc_time(10, SC_MS)),
           "recommit after the core reset");
    std::vector<std::uint8_t> got;
    expect(m0.read(kBase, got, 64) == AxiResp::Okay && got == keep, "DRAM contents survive the core reset (no erase)");
}

} // namespace

int sc_main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    BuildConfig cfg = BuildConfig::native_r2();
    cfg.n_ports = 2;
    BuildConfig noecc = cfg;
    noecc.ecc_enable = false;
    if (which == "ecc") return run_bench(cfg, scenario_ecc, "ecc ecc");
    if (which == "ecc_legacy") {
        BuildConfig c = BuildConfig::legacy();
        c.n_ports = 1;
        return run_bench(c, [](Bench& b) {
            expect(boot_memory(b), "boot legacy");
            MemPort& m = *b.mem[0];
            const std::vector<std::uint8_t> p = pattern(32, 1);
            m.write(kBase + 0x40, p);
            b.ctrl->fault_flip_data_bit(2, 200);
            std::vector<std::uint8_t> got;
            expect(m.read(kBase + 0x40, got, 32) == AxiResp::Okay && got == p && b.snap(FX1_DDR3_CE_COUNT) == 1,
                   "AXI64: CE corrected through four 8-byte beats");
            write_granule(m, 0x40, std::vector<std::uint8_t>(32, 0x99), 0x00F00000u);
            std::vector<std::uint8_t> want = p;
            for (unsigned i = 20; i < 24; ++i) want[i] = 0x99;
            expect(m.read(kBase + 0x40, got, 32) == AxiResp::Okay && got == want, "AXI64 partial write RMW");
        }, "ecc ecc_legacy");
    }
    if (which == "ecc_off") return run_bench(noecc, scenario_ecc_off, "ecc ecc_off");
    if (which == "scrub") return run_bench(cfg, scenario_scrub, "ecc scrub");
    if (which == "quarantine") return run_bench(cfg, scenario_quarantine, "ecc quarantine");
    if (which == "quarantine_walk") return run_bench(cfg, scenario_quarantine_walk, "ecc quarantine_walk");
    if (which == "quarantine_reasons") return run_bench(noecc, scenario_quarantine_reasons, "ecc quarantine_reasons");
    if (which == "fatal") return run_bench(cfg, scenario_fatal, "ecc fatal");
    if (which == "fatal_noecc") return run_bench(noecc, scenario_fatal_noecc, "ecc fatal_noecc");
    if (which == "fatal_sources") return run_bench(cfg, scenario_fatal_sources, "ecc fatal_sources");
    if (which == "fatal_followers") return run_bench(cfg, scenario_fatal_followers, "ecc fatal_followers");
    if (which == "fatal_containment") return run_bench(cfg, scenario_fatal_containment, "ecc fatal_containment");
    if (which == "fatal_containment_noecc")
        return run_bench(noecc, scenario_fatal_containment, "ecc fatal_containment_noecc");
    if (which == "reset") return run_bench(noecc, scenario_reset, "ecc reset");
    std::cerr << "unknown case '" << which << "'\n";
    return 2;
}
