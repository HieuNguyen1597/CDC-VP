// SPDX-License-Identifier: Apache-2.0
// CSR boundary regressions (subtask 2): register inventory, access classes, commits,
// mutex, snapshots, interrupts, live policy, global quiesce and FE_PORT_CMD.
// The inventory below is transcribed from docs/register_inventory.md (HAS Table 8-5),
// independently of the model's csr_map table.
#include "ddr3_bench.h"

#include <map>
#include <set>

using namespace fx1_ddr3_test;
using namespace fx1::ddr3;

namespace {

enum Cls { RO, RW, SH, IR, WC, CMD };
struct Exp {
    std::uint32_t off;
    Cls cls;
    std::uint32_t reset;  // read value at time 0 (native R2, ECC on, default PHY)
};

std::vector<Exp> inventory() {
    std::vector<Exp> v = {
        {0x0000, RO, 0x44445233}, {0x0004, RO, 0x00010000}, {0x0008, RO, 0x00000DD5}, {0x000C, RO, 0x04101008},
        {0x0010, RO, 0x00208410}, {0x0014, RW, 0}, {0x0018, RO, 0}, {0x001C, CMD, 0},
        {0x0020, RO, 0x1C},  // QUIESCE_STATUS live: global idle, mission idle, DFI tokens empty
        {0x0024, WC, 0}, {0x0028, CMD, 0}, {0x002C, RO, 0}, {0x0030, WC, 0}, {0x0034, RW, 0},
        {0x0040, CMD, 0}, {0x0044, RO, 0}, {0x0048, RO, 0}, {0x0180, CMD, 0}, {0x0184, RO, 0},
        {0x1000, SH, 0}, {0x1004, RO, 0}, {0x1008, RO, 0}, {0x100C, RO, 0}, {0x1010, RO, 0}, {0x1014, RO, 0},
        {0x1018, RO, 0}, {0x101C, RO, 0}, {0x1020, CMD, 0}, {0x1024, RO, 0}, {0x1090, CMD, 0}, {0x1094, RO, 0},
        {0x2000, RW, 0x00100408}, {0x2004, RW, 0x400}, {0x2008, RO, 0}, {0x200C, RO, 0}, {0x2010, RO, 0},
        {0x2014, RO, 0}, {0x2018, RO, 0}, {0x2020, RO, 0}, {0x2024, RO, 0}, {0x2028, RO, 0}, {0x2040, RW, 0x400},
        {0x2044, RO, 0}, {0x2048, RO, 0},
        {0x3000, RW, 8}, {0x3004, RO, 0}, {0x3008, RO, 0}, {0x300C, RO, 0}, {0x3040, CMD, 0}, {0x3044, RO, 0},
        {0x3048, WC, 0}, {0x3050, IR, 0}, {0x3054, IR, 0}, {0x3058, IR, 0}, {0x305C, IR, 0}, {0x3060, IR, 1},
        {0x3064, IR, 1}, {0x3068, IR, 1}, {0x306C, IR, 1}, {0x3070, IR, 1}, {0x3074, IR, 1}, {0x3078, IR, 0x400},
        {0x307C, IR, 0x14}, {0x30C0, CMD, 0}, {0x30C4, RO, 0}, {0x30C8, RW, 0x400}, {0x30CC, IR, 1},
        {0x30D0, IR, 1}, {0x30D4, WC, 0},
        {0x4000, RO, 0x00010801},  // ECC_CAP_STATUS: [0]=1, [12:9]=4, [20:13]=8
        {0x4004, RW, 1}, {0x4008, RO, 0}, {0x400C, RO, 0}, {0x4010, RO, 0}, {0x4014, RO, 0}, {0x4018, RO, 0},
        {0x4040, RO, 0}, {0x4044, RO, 0}, {0x4048, RO, 0}, {0x4050, RO, 0}, {0x4054, RO, 0}, {0x4058, RO, 0},
        {0x5000, RO, 0}, {0x5004, RO, 0}, {0x5010, SH, 0}, {0x5014, SH, 0}, {0x5018, SH, 0x100}, {0x501C, SH, 1},
        {0x5020, CMD, 0}, {0x5024, RO, 0}, {0x5040, IR, 0}, {0x5044, IR, 0}, {0x5048, IR, 0}, {0x504C, IR, 0},
        {0x5050, IR, 0}, {0x5080, CMD, 0}, {0x5084, RO, 0}, {0x5088, RW, 0x400}, {0x508C, RO, 0}, {0x5090, RO, 0},
        {0x5094, RO, 0}, {0x5098, RO, 0}, {0x509C, WC, 0},
        {0x6000, RO, 0}, {0x6004, RW, 0}, {0x6008, RO, 0}, {0x600C, WC, 0}, {0x6010, CMD, 0},
    };
    for (std::uint32_t i = 0; i < 32; ++i) {
        v.push_back({0x0100 + 4 * i, SH, 0});
        v.push_back({0x0200 + 4 * i, RO, 0});
    }
    for (std::uint32_t i = 0; i < 19; ++i) {
        v.push_back({0x1040 + 4 * i, SH, 0});
        v.push_back({0x1100 + 4 * i, RO, 0});
    }
    return v;
}

void scenario_inventory(Bench& b) {
    CsrPort& c = b.csr;
    const auto inv = inventory();
    expect(inv.size() == 210, "independent inventory lists 210 words");
    std::set<std::uint32_t> expected;
    for (const auto& e : inv) expected.insert(e.off);

    // Every aligned offset of the 64 KiB aperture: decoded set == inventory, others DECERR.
    std::set<std::uint32_t> decoded;
    unsigned bad_unmapped = 0;
    for (std::uint32_t a = 0; a < 0x10000; a += 4) {
        std::uint32_t v = 0xDEADBEEF;
        const auto s = c.access(tlm::TLM_READ_COMMAND, a, v);
        if (s == tlm::TLM_ADDRESS_ERROR_RESPONSE) {
            if (v != 0) ++bad_unmapped;
        } else {
            decoded.insert(a);
        }
    }
    expect(decoded.size() == 210 && decoded == expected, "exactly the 210 inventory words decode");
    expect(bad_unmapped == 0, "DECERR reads return zero data");
    std::uint32_t junk = 0x12345678;
    expect(c.access(tlm::TLM_WRITE_COMMAND, 0x0038, junk) == tlm::TLM_ADDRESS_ERROR_RESPONSE,
           "write to an unmapped word -> DECERR");

    // Reset values and access classes.
    unsigned reset_bad = 0, ro_bad = 0, cmd_bad = 0, w1c_bad = 0;
    for (const auto& e : inv) {
        const std::uint32_t got = c.rd(e.off);
        if (got != e.reset) {
            ++reset_bad;
            expect(false, "reset value @0x" + CsrPort::hex(e.off) + " = 0x" + CsrPort::hex(got));
        }
        if (e.cls == RO) {
            if (c.wr(e.off, 0xFFFFFFFFu) != tlm::TLM_GENERIC_ERROR_RESPONSE || c.rd(e.off) != got) ++ro_bad;
        }
        if (e.cls == CMD) {
            std::uint32_t v = 0x5A5A5A5A;
            if (c.access(tlm::TLM_READ_COMMAND, e.off, v) != tlm::TLM_OK_RESPONSE || v != 0) ++cmd_bad;
            if (c.wr(e.off, 0x1, 0x1) != tlm::TLM_GENERIC_ERROR_RESPONSE) ++cmd_bad;  // partial strobe
        }
        if (e.cls == WC && c.wr(e.off, 0xFFFFFFFFu, 0x3) != tlm::TLM_GENERIC_ERROR_RESPONSE) ++w1c_bad;
    }
    expect(reset_bad == 0, "all reset values match the inventory");
    expect(ro_bad == 0, "RO writes -> SLVERR, value unchanged");
    expect(cmd_bad == 0, "WO-CMD reads 0; partial-strobe command writes -> SLVERR");
    expect(w1c_bad == 0, "W1C partial-strobe writes -> SLVERR");
    expect(c.rd(FX1_DDR3_COMMAND_ERROR) == 0 && c.rd(FX1_DDR3_IRQ_RAW) == 0,
           "decode/strobe errors set no sticky status (side-effect free)");

    // Byte merge on byte-writeable classes; WSTRB = 0 is a legal no-op.
    const std::uint32_t merge_regs[] = {FX1_DDR3_SCRATCH, FX1_DDR3_FE_REGION_SHADOW(5), FX1_DDR3_TOPO_SHADOW,
                                        FX1_DDR3_TIMING_SHADOW(3), FX1_DDR3_PHY_SKEW_CTRL0, FX1_DDR3_MR(0)};
    for (std::uint32_t r : merge_regs) {
        const std::uint32_t before = c.rd(r);
        expect(c.wr(r, 0xAABBCCDDu, 0x2) == tlm::TLM_OK_RESPONSE &&
                   c.rd(r) == ((before & ~0x0000FF00u) | 0x0000CC00u),
               "byte-lane merge @0x" + CsrPort::hex(r));
        expect(c.wr(r, 0xFFFFFFFFu, 0x0) == tlm::TLM_OK_RESPONSE && c.rd(r) == ((before & ~0x0000FF00u) | 0x0000CC00u),
               "WSTRB = 0 is a no-op @0x" + CsrPort::hex(r));
    }
    // Reserved "write zero" bits -> SLVERR without effect.
    struct Rz { std::uint32_t off, bad; const char* what; };
    const Rz rz[] = {{FX1_DDR3_GLOBAL_CTRL, 0x4, "GLOBAL_CTRL[2]"},
                     {FX1_DDR3_PHY_RDLVL_DELAY, 0x01000000, "PHY_RDLVL_DELAY[24]"},
                     {FX1_DDR3_PHY_GATE_DELAY, 0x80000000, "PHY_GATE_DELAY[31]"},
                     {FX1_DDR3_PHY_WRLVL_DELAY, 0x8, "PHY_WRLVL_DELAY[3]"},
                     {FX1_DDR3_QUIESCE_CTRL, 0x4, "QUIESCE_CTRL[2]"},
                     {FX1_DDR3_SNAPSHOT_CMD, 0x2, "SNAPSHOT_CMD[1]"},
                     {FX1_DDR3_FE_PORT_CMD, 0x9, "FE_PORT_CMD[3]"},
                     {FX1_DDR3_TRAIN_CMD, 0x10000001, "TRAIN_CMD[28]"}};
    for (const auto& r : rz) {
        const std::uint32_t before = c.rd(r.off);
        expect(c.wr(r.off, r.bad) == tlm::TLM_GENERIC_ERROR_RESPONSE && c.rd(r.off) == before,
               std::string("reserved write-zero bit rejected: ") + r.what);
    }
    // "Read zero / writes ignored" fields are dropped, not rejected (GAP-CSR-03).
    expect(c.ok(FX1_DDR3_IRQ_MASK, 0xFFFFFFFFu) && c.rd(FX1_DDR3_IRQ_MASK) == 0xFFFu, "IRQ_MASK[31:12] ignored");
    expect(c.ok(FX1_DDR3_IRQ_MASK, 0), "IRQ_MASK cleared");
    // Commands that never launch.
    expect(c.wr(FX1_DDR3_INIT_CMD, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               c.wr(FX1_DDR3_INIT_CMD, 2) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "INIT_CMD start/retry -> SLVERR (DEC-05)");
    expect(c.wr(FX1_DDR3_POWER_CMD, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE, "POWER_CMD -> SLVERR (P-channel only)");
    expect(c.rd(FX1_DDR3_INIT_STATUS) == 0 && c.rd(FX1_DDR3_COMMAND_ERROR) == 0, "...with no side effect");
    // transport_dbg: side-effect-free reads.
    unsigned char buf[4]{};
    tlm::tlm_generic_payload dbg;
    dbg.set_command(tlm::TLM_READ_COMMAND);
    dbg.set_address(FX1_DDR3_IP_ID);
    dbg.set_data_ptr(buf);
    dbg.set_data_length(4);
    expect(b.mux_out->transport_dbg(dbg) == 4 && buf[0] == 0x33 && buf[3] == 0x44, "transport_dbg reads IP_ID");
    dbg.set_command(tlm::TLM_WRITE_COMMAND);
    expect(b.mux_out->transport_dbg(dbg) == 0, "transport_dbg writes are not supported");
}

// ---------------------------------------------------------------- commits
void scenario_commits(Bench& b) {
    CsrPort& c = b.csr;
    const BuildConfig cfg = b.ctrl->config();
    // Topology: reset image (row bits 0) is illegal: immediate rejection, blocker in [23:16].
    expect(c.wr(FX1_DDR3_TOPO_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE, "TOPO_COMMIT of the reset image rejected");
    expect(c.rd(FX1_DDR3_TOPO_COMMIT_STATUS) == (FX1_DDR3_COMMIT_ST_ERROR | (0x11u << 16)),
           "TOPO_COMMIT_STATUS: error, immediate blocker 0x11");
    expect(c.rd(FX1_DDR3_COMMAND_ERROR) == FX1_DDR3_CMD_ERR_COMMIT, "COMMAND_ERROR[2] commit failure");
    expect(c.rd(FX1_DDR3_IRQ_RAW) == (FX1_DDR3_IRQ_MAPPING | FX1_DDR3_IRQ_CSR_CMD), "IRQ_RAW[4] and [10] raised");
    expect(c.ok(FX1_DDR3_COMMAND_ERROR, 0x7) && c.rd(FX1_DDR3_COMMAND_ERROR) == 0, "COMMAND_ERROR W1C");
    expect(c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL) && c.rd(FX1_DDR3_IRQ_RAW) == 0, "IRQ_CLEAR W1C");
    // Region commit before a topology: USABLE_GRANULES = 0 -> 0x0D.
    expect(c.ok(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 0)), 0x80000000u) &&
               c.ok(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 1)), 0x20000000u),
           "region 0 of port 0 programmed");
    expect(c.wr(FX1_DDR3_FE_REGION_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               c.rd(FX1_DDR3_FE_REGION_STATUS) == (FX1_DDR3_FE_REGION_ST_ERROR | (0x0Du << 8)),
           "region commit before topology -> blocker 0x0D");
    // DFI before timing: dependency.
    expect(c.wr(FX1_DDR3_DFI_TIMING_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_COMMIT_ST_IMM_BLOCKER(c.rd(FX1_DDR3_DFI_TIMING_STATUS)) == FX1_DDR3_BLK_DEPENDENCY,
           "DFI commit without an active timing image -> dependency blocker (provisional)");
    // Legal topology: 2 ranks, 16 rows, 1024-word page.
    const std::uint32_t topo = FX1_DDR3_TOPO_VAL(2, 16, FX1_DDR3_TOPO_PAGE_1024, FX1_DDR3_TOPO_MAP_LINEAR);
    expect(topo == 0x501, "TOPO_VAL(2,16,1024,linear) == MAS reference 0x501");
    expect(c.ok(FX1_DDR3_TOPO_SHADOW, topo) && c.ok(FX1_DDR3_TOPO_COMMIT, 1), "legal TOPO_COMMIT accepted");
    expect(c.rd(FX1_DDR3_TOPO_ACTIVE) == topo, "TOPO_ACTIVE mirrors the committed image");
    expect(c.rd(FX1_DDR3_TOPO_COMMIT_STATUS) == (FX1_DDR3_COMMIT_ST_DONE | FX1_DDR3_COMMIT_ST_REINIT),
           "TOPO_COMMIT_STATUS: done, reinit required");
    expect(c.rd(FX1_DDR3_USABLE_GRANULES_LO) == 0, "capacity words are snapshot-backed (0 before a capture)");
    // P = 2 x 2^16 x 1024 = 2^27; U = 8 x floor(2^27 / 9) = 119304640 (SPEC-07).
    expect(b.snap(FX1_DDR3_PHYSICAL_GRANULES_LO) == 0x08000000u && c.rd(FX1_DDR3_PHYSICAL_GRANULES_HI) == 0,
           "PHYSICAL_GRANULES = 2^27");
    expect(c.rd(FX1_DDR3_USABLE_GRANULES_LO) == 119304640u && c.rd(FX1_DDR3_ECC_BASE_LO) == 119304640u,
           "USABLE_GRANULES = ECC_BASE = 8 floor(P/9)");
    // Timing: reset zeros rejected with 0x04, then a legal image.
    expect(c.wr(FX1_DDR3_TIMING_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               c.rd(FX1_DDR3_TIMING_COMMIT_STATUS) == (FX1_DDR3_COMMIT_ST_ERROR | (0x04u << 16)),
           "TIMING_COMMIT of the reset image -> blocker 0x04 (HAS reset 0, SPEC-03)");
    const TimingImage t = timing_image(cfg);
    for (unsigned i = 0; i < kTimingWords; ++i) c.ok(FX1_DDR3_TIMING_SHADOW(i), t[i]);
    expect(c.ok(FX1_DDR3_TIMING_COMMIT, 1) && c.rd(FX1_DDR3_TIMING_ACTIVE(FX1_DDR3_T_CL)) == 11 &&
               c.rd(FX1_DDR3_TIMING_ACTIVE(FX1_DDR3_T_REFI)) == 6240,
           "TIMING_COMMIT accepted; A_CL = 11, A_T_REFI = 6240");
    // DFI: shadow must equal the formulas against the active timing.
    expect(c.wr(FX1_DDR3_DFI_TIMING_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_COMMIT_ST_IMM_BLOCKER(c.rd(FX1_DDR3_DFI_TIMING_STATUS)) == FX1_DDR3_BLK_FIELD_ILLEGAL,
           "DFI image not matching CWL+AL-PWL-2 / CL+AL+PRL+G-4 rejected");
    c.ok(FX1_DDR3_DFI_WR_ENABLE_DELAY, 4);
    c.ok(FX1_DDR3_DFI_RD_ENABLE_DELAY, 13);
    expect(c.ok(FX1_DDR3_DFI_TIMING_COMMIT, 1) && c.rd(FX1_DDR3_DFI_TIMING_STATUS) == FX1_DDR3_COMMIT_ST_DONE,
           "DFI commit 4/13 accepted");
    expect(b.ctrl->images_valid(), "topology, timing and DFI images valid");
    // A timing change that breaks the DFI formula invalidates the DFI image (init re-holds).
    c.ok(FX1_DDR3_TIMING_SHADOW(FX1_DDR3_T_CL), 10);
    c.ok(FX1_DDR3_TIMING_SHADOW(FX1_DDR3_T_RTW), 8);
    expect(c.ok(FX1_DDR3_TIMING_COMMIT, 1) && !b.ctrl->images_valid(), "new CL invalidates the DFI image");
    c.ok(FX1_DDR3_DFI_RD_ENABLE_DELAY, 12);
    expect(c.ok(FX1_DDR3_DFI_TIMING_COMMIT, 1) && b.ctrl->images_valid(), "matching DFI image re-committed");
    // READ_TIMEOUT bound against the active DFI image: 5R + 12 + 256 + 4 + 1 = 283.
    expect(c.wr(FX1_DDR3_READ_TIMEOUT, 282) == tlm::TLM_GENERIC_ERROR_RESPONSE && c.ok(FX1_DDR3_READ_TIMEOUT, 283),
           "READ_TIMEOUT floor checked against the active DFI image");
    c.ok(FX1_DDR3_READ_TIMEOUT, 0x400);
    // Region map: DEC-07 readback before the first commit, then rules 0x0C / success.
    b.ctrl->set_bootstrap_region(0, 0, {0x80000000u, 0x20000000u});
    expect(c.rd(FX1_DDR3_FE_REGION_ACTIVE(0)) == 0 && !(c.rd(FX1_DDR3_FE_REGION_STATUS) & FX1_DDR3_FE_REGION_ST_VALID),
           "FE_REGION_ACTIVE and STATUS[16] read 0 before the first commit despite the bootstrap map (DEC-07)");
    c.ok(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 1, 0)), 0x90000010u);  // not 32-aligned
    c.ok(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 1, 1)), 0x1000u);
    expect(c.wr(FX1_DDR3_FE_REGION_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_FE_REGION_ST_BLOCKER(c.rd(FX1_DDR3_FE_REGION_STATUS)) == 0x0C,
           "misaligned region -> 0x0C");
    c.ok(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 1, 0)), 0x9FFFF000u);  // overlaps region 0
    expect(c.wr(FX1_DDR3_FE_REGION_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_FE_REGION_ST_BLOCKER(c.rd(FX1_DDR3_FE_REGION_STATUS)) == 0x0C,
           "same-port overlap -> 0x0C");
    c.ok(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(1, 0, 0)), 0x80000000u);  // other port may overlap
    c.ok(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(1, 0, 1)), 0x1000u);
    c.ok(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 1, 0)), 0xA0000000u);
    expect(c.ok(FX1_DDR3_FE_REGION_COMMIT, 1) &&
               c.rd(FX1_DDR3_FE_REGION_STATUS) == (FX1_DDR3_FE_REGION_ST_DONE | FX1_DDR3_FE_REGION_ST_VALID),
           "legal region image installed; cross-port overlap allowed");
    expect(c.rd(FX1_DDR3_FE_REGION_ACTIVE(0)) == 0x80000000u && c.rd(FX1_DDR3_FE_REGION_ACTIVE(1)) == 0x20000000u &&
               c.rd(FX1_DDR3_FE_REGION_ACTIVE(FX1_DDR3_REGION_INDEX(0, 1, 0))) == 0xA0000000u,
           "FE_REGION_ACTIVE mirrors the committed image");
}

// ---------------------------------------------------------------- mutex and snapshots
void scenario_mutex_snapshot(Bench& b) {
    CsrPort& c = b.csr;
    expect(c.rd(FX1_DDR3_SNAPSHOT_STATUS) == 0, "no valid snapshot after reset");
    // A second master's command during a commit's endpoint wait -> busy SLVERR, not queued.
    tlm::tlm_response_status second = tlm::TLM_INCOMPLETE_RESPONSE;
    c.ok(FX1_DDR3_TOPO_SHADOW, 0x501);
    b.at(sc_time(1, SC_NS), [&b, &second] { second = b.csr2.wr(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST); });
    const sc_time t0 = sc_core::sc_time_stamp();
    expect(c.ok(FX1_DDR3_TOPO_COMMIT, 1), "first command owns the mutex and completes");
    expect(sc_core::sc_time_stamp() - t0 == sc_time(12.5, SC_NS),
           "commit response waits for the endpoint (4 cycles) plus the access cycle");
    expect(second == tlm::TLM_GENERIC_ERROR_RESPONSE, "colliding command -> SLVERR");
    expect((c.rd(FX1_DDR3_COMMAND_ERROR) & FX1_DDR3_CMD_ERR_BUSY) && (c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_CSR_CMD),
           "busy collision sets COMMAND_ERROR[0] and IRQ_RAW[10]");
    expect(!(c.rd(FX1_DDR3_QUIESCE_STATUS) & FX1_DDR3_QUIESCE_ST_REQUEST), "the rejected command had no effect");
    c.ok(FX1_DDR3_COMMAND_ERROR, 7);
    // Snapshot: valid + sequence, stable until the next capture.
    expect(c.ok(FX1_DDR3_SNAPSHOT_CMD, 1), "capture 1");
    std::uint32_t st = c.rd(FX1_DDR3_SNAPSHOT_STATUS);
    expect((st & FX1_DDR3_SNAPSHOT_ST_VALID) && FX1_DDR3_SNAPSHOT_ST_SEQ(st) == 1 && !(st & FX1_DDR3_SNAPSHOT_ST_BUSY),
           "valid, sequence 1");
    const std::uint32_t usable = c.rd(FX1_DDR3_USABLE_GRANULES_LO);
    expect(usable == 119304640u, "captured capacity");
    c.ok(FX1_DDR3_TOPO_SHADOW, FX1_DDR3_TOPO_VAL(1, 16, FX1_DDR3_TOPO_PAGE_1024, 0));
    c.ok(FX1_DDR3_TOPO_COMMIT, 1);
    expect(c.rd(FX1_DDR3_USABLE_GRANULES_LO) == usable, "snapshot word stable until the next capture");
    c.ok(FX1_DDR3_SNAPSHOT_CMD, 1);
    st = c.rd(FX1_DDR3_SNAPSHOT_STATUS);
    expect(FX1_DDR3_SNAPSHOT_ST_SEQ(st) == 2 && c.rd(FX1_DDR3_USABLE_GRANULES_LO) == 59652320u,
           "capture 2: sequence 2, one-rank capacity 8 floor(2^26/9)");
    // Concurrent capture requests: the second collides with the busy capture.
    tlm::tlm_response_status snap2 = tlm::TLM_INCOMPLETE_RESPONSE;
    b.at(sc_time(1, SC_NS), [&b, &snap2] { snap2 = b.csr2.wr(FX1_DDR3_SNAPSHOT_CMD, 1); });
    expect(c.ok(FX1_DDR3_SNAPSHOT_CMD, 1) && snap2 == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "snapshot busy collision -> SLVERR");
    expect(FX1_DDR3_SNAPSHOT_ST_SEQ(c.rd(FX1_DDR3_SNAPSHOT_STATUS)) == 3, "only one capture happened");
}

// ---------------------------------------------------------------- interrupts
void scenario_irq(Bench& b) {
    CsrPort& c = b.csr;
    expect(c.ok(FX1_DDR3_IRQ_TEST_SET, 0x5), "IRQ_TEST_SET");
    expect(c.rd(FX1_DDR3_IRQ_RAW) == 0x5 && c.rd(FX1_DDR3_IRQ_STATUS) == 0 && !b.irq.read(),
           "raw set, masked: status 0, irq low");
    c.ok(FX1_DDR3_IRQ_MASK, 0x1);
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(c.rd(FX1_DDR3_IRQ_STATUS) == 0x1 && b.irq.read(), "status = raw & mask, irq high");
    expect(c.wr(FX1_DDR3_IRQ_CLEAR, 0x1, 0x1) == tlm::TLM_GENERIC_ERROR_RESPONSE && c.rd(FX1_DDR3_IRQ_RAW) == 0x5,
           "partial-strobe IRQ_CLEAR rejected, nothing cleared");
    expect(c.ok(FX1_DDR3_IRQ_CLEAR, 0x1) && c.rd(FX1_DDR3_IRQ_RAW) == 0x4, "W1C clears bit 0");
    // Same-cycle set and clear from two masters: the set wins (HAS §8.5).
    // The actor decides its TEST_SET in the next delta; the clear is decided one delta later
    // at the same simulated time, so the set is ordered first and must survive the clear.
    const sc_time t_set = sc_core::sc_time_stamp();
    b.at(sc_core::SC_ZERO_TIME, [&b] { b.csr2.ok(FX1_DDR3_IRQ_TEST_SET, 0x1); });
    sc_core::wait(sc_core::SC_ZERO_TIME);
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(sc_core::sc_time_stamp() == t_set && (b.ctrl->irq_raw() & 0x1), "test set applied at t");
    expect(c.ok(FX1_DDR3_IRQ_CLEAR, 0x1) && (c.rd(FX1_DDR3_IRQ_RAW) & 0x1), "same-cycle set and clear: set wins");
    expect(c.ok(FX1_DDR3_IRQ_CLEAR, 0x1) && c.rd(FX1_DDR3_IRQ_RAW) == 0x4, "a later clear removes it");
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(!b.irq.read(), "irq low once no enabled source is pending");
    c.ok(FX1_DDR3_IRQ_MASK, 0);
    expect(c.rd(FX1_DDR3_IRQ_RAW) == 0x4, "masking never clears raw state");
    expect(c.ok(FX1_DDR3_IRQ_TEST_SET, 0xFFFFFFFFu) && c.rd(FX1_DDR3_IRQ_RAW) == 0xFFF, "only [11:0] exist");
}

void scenario_irq_test_disabled(Bench& b) {
    expect(b.csr.wr(FX1_DDR3_IRQ_TEST_SET, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE && b.csr.rd(FX1_DDR3_IRQ_RAW) == 0,
           "IRQ_TEST_SET -> SLVERR when the build disables test injection");
    expect(!(b.csr.rd(FX1_DDR3_CAP_FEATURE) & FX1_DDR3_CAP_FEATURE_IRQ_TEST), "CAP_FEATURE[11] = 0");
}

// ---------------------------------------------------------------- live policy, MR, quiesce
void scenario_policy(Bench& b) {
    CsrPort& c = b.csr;
    auto rejected = [&c](std::uint32_t off, std::uint32_t v) {
        const std::uint32_t before = c.rd(off);
        return c.wr(off, v) == tlm::TLM_GENERIC_ERROR_RESPONSE && c.rd(off) == before;
    };
    expect(rejected(FX1_DDR3_WRITE_DRAIN_CFG, 0x00100808), "drain exit >= enter rejected");
    expect(rejected(FX1_DDR3_WRITE_DRAIN_CFG, 0x00100411), "drain enter 17 > REQ_DEPTH rejected");
    expect(rejected(FX1_DDR3_WRITE_DRAIN_CFG, 0x00000408), "drain maximum 0 rejected");
    expect(c.ok(FX1_DDR3_WRITE_DRAIN_CFG, 0x00200A10) && c.rd(FX1_DDR3_WRITE_DRAIN_CFG) == 0x00200A10,
           "legal drain policy accepted");
    expect(rejected(FX1_DDR3_STARVE_LIMIT, 0) && rejected(FX1_DDR3_STARVE_LIMIT, 0x80000000u), "STARVE_LIMIT range");
    expect(rejected(FX1_DDR3_READ_TIMEOUT, 0), "READ_TIMEOUT nonzero");
    expect(rejected(FX1_DDR3_REFRESH_CFG, 0) && rejected(FX1_DDR3_REFRESH_CFG, 9) && c.ok(FX1_DDR3_REFRESH_CFG, 5),
           "REFRESH_CFG limit 1..8");
    expect(rejected(FX1_DDR3_POWER_TIMEOUT, 0) && rejected(FX1_DDR3_TRAIN_TIMEOUT, 0), "timeouts nonzero");
    expect(rejected(FX1_DDR3_CE_THRESHOLD, 0) && c.ok(FX1_DDR3_CE_THRESHOLD, 3), "CE_THRESHOLD nonzero (C-16)");
    // Mode registers (MAS Table 5-20 field checks).
    expect(rejected(FX1_DDR3_MR(1), 0x244), "MR1 Rtt_Nom 111 rejected");
    expect(rejected(FX1_DDR3_MR(1), 0x020), "MR1 drive 10 rejected");
    expect(c.ok(FX1_DDR3_MR(1), 0x040), "MR1 Rtt_Nom RZQ/2 (A6), drive RZQ/6 accepted");
    expect(rejected(FX1_DDR3_MR(2), 0x600), "MR2 Rtt_WR 11 rejected");
    expect(rejected(FX1_DDR3_MR(2), 0x0C0), "MR2 SRT and ASR both set rejected");
    expect(c.ok(FX1_DDR3_MR(2), 0x218), "MR2 legal value accepted");
    expect(rejected(FX1_DDR3_TRAIN_TRAS_WAIT, 0), "TRAIN_TRAS_WAIT nonzero");
    // Global quiesce: exactly one of request/release.
    expect(c.wr(FX1_DDR3_QUIESCE_CTRL, 3) == tlm::TLM_GENERIC_ERROR_RESPONSE, "QUIESCE_CTRL request+release rejected");
    expect(c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST), "quiesce request");
    const std::uint32_t q = c.rd(FX1_DDR3_QUIESCE_STATUS);
    expect((q & FX1_DDR3_QUIESCE_ST_REQUEST) && (q & FX1_DDR3_QUIESCE_ST_ADM_STOPPED) && (q & FX1_DDR3_QUIESCE_ST_COMPLETE) &&
               !(q & FX1_DDR3_QUIESCE_ST_TIMEOUT),
           "QUIESCE_STATUS: request, admission stopped, complete; timeout reads 0");
    expect(c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_RELEASE) &&
               !(c.rd(FX1_DDR3_QUIESCE_STATUS) & FX1_DDR3_QUIESCE_ST_REQUEST),
           "quiesce released");
    expect(c.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE | FX1_DDR3_GLOBAL_CTRL_ADM_STOP) &&
               (c.rd(FX1_DDR3_QUIESCE_STATUS) & FX1_DDR3_QUIESCE_ST_ADM_STOPPED),
           "GLOBAL_CTRL software admission stop is reported");
}

// ---------------------------------------------------------------- FE_PORT_CMD
void scenario_fe_port(Bench& b) {
    CsrPort& c = b.csr;
    Ddr3Controller& d = *b.ctrl;
    expect(c.wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(5, 0)) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_FE_PORT_ST_BLOCKER(c.rd(FX1_DDR3_FE_PORT_CMD_STATUS)) == FX1_DDR3_BLK_BAD_PORT_OR_OP &&
               (c.rd(FX1_DDR3_FE_PORT_CMD_STATUS) & FX1_DDR3_FE_PORT_ST_ERROR),
           "reserved op rejected");
    expect(c.wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 3)) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "port beyond DDR_SYS_N_PORT rejected");
    expect(c.wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_SOFT_RESET, 1)) ==
                   tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_FE_PORT_ST_BLOCKER(c.rd(FX1_DDR3_FE_PORT_CMD_STATUS)) == FX1_DDR3_BLK_NO_QUIESCE_ACK,
           "soft reset without quiesce ack rejected (SOQ-10 proposal)");
    expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0)), "op 1 quiesce port 0");
    std::uint32_t st = c.rd(FX1_DDR3_FE_PORT_CMD_STATUS);
    expect((st & FX1_DDR3_FE_PORT_ST_DONE) && !(st & FX1_DDR3_FE_PORT_ST_ERROR) && FX1_DDR3_FE_PORT_ST_OP(st) == 1 &&
               FX1_DDR3_FE_PORT_ST_PORT(st) == 0,
           "FE_PORT_CMD_STATUS done, op 1, port 0 (new command cleared the error)");
    std::uint32_t state = c.rd(FX1_DDR3_FE_PORT_STATE);
    expect((state & FX1_DDR3_FE_STATE_VALID) && FX1_DDR3_FE_STATE_CSR_QUIESCE(state) == 1 &&
               FX1_DDR3_FE_STATE_EFF_QUIESCE(state) == 1 && FX1_DDR3_FE_STATE_ACK(state) == 1 &&
               FX1_DDR3_FE_STATE_TIMEOUT(state) == 0,
           "FE_PORT_STATE published: CSR quiesce, effective quiesce, ack");
    d.qos_write(0, 7, 2);
    d.qos_set_configured(0, true);
    const sc_time t0 = sc_core::sc_time_stamp();
    expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_SOFT_RESET, 0)), "op 3 soft reset port 0");
    expect(sc_core::sc_time_stamp() - t0 >= sc_time(20, SC_NS), "response waits for the 8-cycle hold");
    expect(d.qos_class(0, 7) == kQosClassNormal && !d.qos_configured(0), "soft reset reset the QoS table");
    expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_RELEASE, 0)) && !d.port_quiesce_ack(0),
           "op 2 releases the CSR quiesce (provisional)");
    // A memory request waits while the CSR quiesce holds the port, and proceeds at release.
    d.qos_set_configured(0, true);
    c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0));
    b.at(sc_time(30, SC_NS), [&b] { b.csr2.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_RELEASE, 0)); });
    const sc_time t1 = sc_core::sc_time_stamp();
    (void)b.mem[0]->read16(0x80000000u);
    expect(sc_core::sc_time_stamp() - t1 >= sc_time(30, SC_NS), "memory request held by the CSR port quiesce");
}

} // namespace

int sc_main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    BuildConfig cfg = BuildConfig::native_r2();
    cfg.n_ports = 2;
    if (which == "inventory") return run_bench(cfg, scenario_inventory, "csr inventory");
    if (which == "commits") return run_bench(cfg, scenario_commits, "csr commits");
    if (which == "mutex_snapshot") return run_bench(cfg, scenario_mutex_snapshot, "csr mutex_snapshot");
    if (which == "irq") return run_bench(cfg, scenario_irq, "csr irq");
    if (which == "irq_test_disabled") {
        cfg.irq_test_enable = false;
        return run_bench(cfg, scenario_irq_test_disabled, "csr irq_test_disabled");
    }
    if (which == "policy") return run_bench(cfg, scenario_policy, "csr policy");
    if (which == "fe_port") return run_bench(cfg, scenario_fe_port, "csr fe_port");
    std::cerr << "unknown case '" << which << "'\n";
    return 2;
}
