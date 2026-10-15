// SPDX-License-Identifier: Apache-2.0
// Lifecycle regressions (subtask 2): firmware-style boot with and without ECC,
// initialization/training failures and recovery, runtime training, the commit quiesce
// interlock, P-channel power transitions and refresh debt.
#include "ddr3_bench.h"

using namespace fx1_ddr3_test;
using namespace fx1::ddr3;

namespace {

constexpr std::uint32_t kTopo2R = FX1_DDR3_TOPO_VAL(2, 16, FX1_DDR3_TOPO_PAGE_1024, FX1_DDR3_TOPO_MAP_LINEAR);

bool wait_posedge(sc_core::sc_signal<bool>& s, sc_time limit) {
    if (s.read()) return true;
    sc_core::wait(limit, s.posedge_event());
    return s.read();
}

bool boot(Bench& b, sc_time limit = sc_time(100, SC_US)) {
    const BuildConfig& cfg = b.ctrl->config();
    expect(program_and_commit(b, cfg, cfg.max_ranks(), 16, FX1_DDR3_TOPO_PAGE_1024), "image programmed and committed");
    return wait_posedge(b.initc, limit);
}

// ---------------------------------------------------------------- boot with ECC
void scenario_boot_ecc(Bench& b) {
    CsrPort& c = b.csr;
    FunctionalPhy& phy = *b.ctrl->functional_phy();
    expect(c.rd(FX1_DDR3_INIT_STATUS) == 0 && !b.initc.read(), "nothing initialized after cold reset");
    // INIT_RW words are locked while initialization runs (GAP-LC-06).
    tlm::tlm_response_status locked = tlm::TLM_INCOMPLETE_RESPONSE;
    b.at(sc_time(400, SC_NS), [&b, &locked] { locked = b.csr2.wr(FX1_DDR3_MR(0), 0x1); });
    expect(boot(b), "init_complete rises after DRAM init and boot training");
    const sc_time t_done = sc_core::sc_time_stamp();  // before any CSR access (each takes a cycle)
    expect(locked == tlm::TLM_GENERIC_ERROR_RESPONSE && (c.rd(FX1_DDR3_COMMAND_ERROR) & FX1_DDR3_CMD_ERR_COMMIT),
           "INIT_RW write during init -> SLVERR and COMMAND_ERROR[2]");
    // Boot training: three loads per active rank, tag 0 (DEC-06).
    expect(phy.loads_received() == 6 && phy.last_load().tag == 0 && phy.last_load().rank == 1 &&
               phy.last_load().cls == PhyLoadClass::WriteLevel && phy.last_load().payload == 0x7,
           "6 boot loads; last = rank 1 write-level, thermometer(3) = 0x07, tag 0");
    expect(phy.init_complete(), "init_complete notification reached the PHY (BE08)");
    const std::uint32_t st = b.snap(FX1_DDR3_INIT_STATUS);
    expect((st & FX1_DDR3_INIT_ST_DRAM_INIT) && !(st & FX1_DDR3_INIT_ST_FAILED) &&
               FX1_DDR3_INIT_ST_STAGE(st) == FX1_DDR3_INIT_STAGE_DONE && (st & FX1_DDR3_INIT_ST_ADM_BLOCK),
           "INIT_STATUS: initialized, stage DONE, admission still blocked (scrub)");
    expect(!b.mission.read() && b.ctrl->scrub_total() == 134217720u, "scrub of U + U/8 = 134217720 granules pending");
    expect(c.ok(FX1_DDR3_MR(0), 0x1), "INIT_RW unlocked after DONE");
    expect(wait_posedge(b.mission, sc_time(1, sc_core::SC_SEC)), "mission ready after scrub");
    expect(sc_core::sc_time_stamp() - t_done == sc_time(671088600, SC_NS),
           "mission ready exactly 134217720 x 4 tCK (5 ns) after init done");
    expect(!(b.snap(FX1_DDR3_INIT_STATUS) & FX1_DDR3_INIT_ST_ADM_BLOCK), "admission block cleared");
    expect(!(c.rd(FX1_DDR3_TOPO_COMMIT_STATUS) & FX1_DDR3_COMMIT_ST_REINIT), "reinit-required cleared at DONE");
    expect((b.snap(FX1_DDR3_PHY_STATUS) & 0xF) == 0xB, "PHY_STATUS: reset_done, ready, timing_wait_ok (provisional)");
}

// ---------------------------------------------------------------- boot without ECC
void scenario_boot_no_ecc(Bench& b) {
    CsrPort& c = b.csr;
    expect(boot(b), "init done");
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(b.mission.read(), "no scrub without ECC: mission ready at init done");
    expect(b.snap(FX1_DDR3_USABLE_GRANULES_LO) == 0x08000000u && c.rd(FX1_DDR3_ECC_BASE_LO) == 0,
           "U0 = P = 2^27, ECC_BASE = 0");
    expect(c.rd(FX1_DDR3_ECC_CAP_STATUS) == 0 && c.rd(FX1_DDR3_CE_THRESHOLD) == 0, "ECC words read 0");
    expect(c.wr(FX1_DDR3_CE_THRESHOLD, 5) == tlm::TLM_GENERIC_ERROR_RESPONSE, "ECC policy write -> SLVERR");
}

void scenario_boot_legacy(Bench& b) {
    expect(boot(b), "legacy (ratio 1, one rank) init done");
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(b.mission.read() && b.ctrl->functional_phy()->loads_received() == 3, "3 boot loads for one rank");
    expect(b.snap(FX1_DDR3_PHYSICAL_GRANULES_LO) == 0x04000000u, "one rank: P = 2^26");
}

// ---------------------------------------------------------------- init failures
void check_training_failure(Bench& b, std::uint32_t bridge_bit, const std::string& what) {
    CsrPort& c = b.csr;
    const std::uint32_t st = b.snap(FX1_DDR3_INIT_STATUS);
    expect((st & FX1_DDR3_INIT_ST_FAILED) && FX1_DDR3_INIT_ST_STAGE(st) == FX1_DDR3_INIT_STAGE_FAILED &&
               !(st & FX1_DDR3_INIT_ST_DRAM_INIT),
           what + ": INIT_STATUS failed");
    expect(c.rd(FX1_DDR3_INIT_ERROR) == FX1_DDR3_INIT_ERROR_STAGE(FX1_DDR3_INIT_STAGE_TRAINING),
           what + ": INIT_ERROR one-hot training stage");
    expect((c.rd(FX1_DDR3_IRQ_RAW) & (FX1_DDR3_IRQ_INIT_FAIL | FX1_DDR3_IRQ_TRAIN_FAIL)) ==
               (FX1_DDR3_IRQ_INIT_FAIL | FX1_DDR3_IRQ_TRAIN_FAIL),
           what + ": IRQ_RAW[6] and [7]");
    expect(c.rd(FX1_DDR3_PHY_BRIDGE_ERROR) & bridge_bit, what + ": PHY_BRIDGE_ERROR bit");
    expect(!b.initc.read() && !b.mission.read(), what + ": init_complete and mission ready stay low");
}

void recover(Bench& b, const std::string& what) {
    CsrPort& c = b.csr;
    c.ok(FX1_DDR3_INIT_ERROR, 0xFFFF);
    c.ok(FX1_DDR3_PHY_BRIDGE_ERROR, 0xFF);
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
    expect(c.ok(FX1_DDR3_TIMING_COMMIT, 1), what + ": recommit accepted (admission never opened)");
    expect(wait_posedge(b.initc, sc_time(100, SC_US)), what + ": reinitialization succeeds");
}

void scenario_init_fail(Bench& b) {
    FunctionalPhy& phy = *b.ctrl->functional_phy();
    phy.script_load_faults({PhyLoadFault::Fail});
    expect(!boot(b, sc_time(20, SC_US)), "provider failure: init does not complete");
    check_training_failure(b, FX1_DDR3_PHY_BRIDGE_ERR_PROVIDER_FAIL, "provider fail");
    expect((b.snap(FX1_DDR3_TRAIN_OBSERVATION0) & 0xFFu) == 0x33, "failed observation captured (rdlvl payload 0x33)");
    recover(b, "provider fail");
    // Admission opened after the recovery (ECC off): further commits need a quiesce (BE05).
    expect(b.csr.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST), "global quiesce for the next phases");

    phy.script_load_faults({PhyLoadFault::Lose});
    expect(b.csr.ok(FX1_DDR3_TIMING_COMMIT, 1), "reinit with a lost observation");
    sc_core::wait(20, SC_US);
    check_training_failure(b, FX1_DDR3_PHY_BRIDGE_ERR_TIMEOUT, "lost observation (TRAIN_TIMEOUT)");
    recover(b, "timeout");

    phy.script_load_faults({PhyLoadFault::StaleEpoch});
    expect(b.csr.ok(FX1_DDR3_TIMING_COMMIT, 1), "reinit with a stale-epoch observation");
    sc_core::wait(20, SC_US);
    check_training_failure(b, FX1_DDR3_PHY_BRIDGE_ERR_STALE | FX1_DDR3_PHY_BRIDGE_ERR_TIMEOUT,
                           "stale epoch discarded, then timeout");
    recover(b, "stale epoch");

    phy.script_load_faults({PhyLoadFault::WrongTag});
    expect(b.csr.ok(FX1_DDR3_TIMING_COMMIT, 1), "reinit with a wrong-tag observation");
    sc_core::wait(20, SC_US);
    check_training_failure(b, FX1_DDR3_PHY_BRIDGE_ERR_STALE, "identity mismatch");
    recover(b, "wrong tag");

    phy.script_load_faults({PhyLoadFault::WrongRank});
    expect(b.csr.ok(FX1_DDR3_TIMING_COMMIT, 1), "reinit with a wrong-rank observation");
    sc_core::wait(20, SC_US);
    check_training_failure(b, FX1_DDR3_PHY_BRIDGE_ERR_STALE, "rank mismatch");
    recover(b, "wrong rank");

    // A late observation (10 us) exceeds TRAIN_TIMEOUT (1024 cycles = 2.56 us): timeout.
    phy.script_load_faults({PhyLoadFault::Delay});
    expect(b.csr.ok(FX1_DDR3_TIMING_COMMIT, 1), "reinit with a late observation");
    sc_core::wait(20, SC_US);
    check_training_failure(b, FX1_DDR3_PHY_BRIDGE_ERR_TIMEOUT, "late observation beyond TRAIN_TIMEOUT");
    recover(b, "late observation");
    // Within a longer TRAIN_TIMEOUT the same late observation completes training.
    expect(b.csr.ok(FX1_DDR3_TRAIN_TIMEOUT, 0x10000u), "TRAIN_TIMEOUT = 65536 cycles (164 us)");
    phy.script_load_faults({PhyLoadFault::Delay});
    expect(b.csr.ok(FX1_DDR3_TIMING_COMMIT, 1), "reinit with a late observation inside the timeout");
    expect(wait_posedge(b.initc, sc_time(200, SC_US)) && !(b.snap(FX1_DDR3_INIT_STATUS) & FX1_DDR3_INIT_ST_FAILED),
           "late observation inside TRAIN_TIMEOUT: initialization completes");
}

void scenario_phy_failure(Bench& b) {
    CsrPort& c = b.csr;
    FunctionalPhy& phy = *b.ctrl->functional_phy();
    b.at(sc_time(500, SC_NS), [&phy] { phy.force_failure(true); });
    expect(!boot(b, sc_time(20, SC_US)), "PHY failure during init: no completion");
    const std::uint32_t err = c.rd(FX1_DDR3_INIT_ERROR);
    expect(err != 0 && (err & (err - 1)) == 0 && err != FX1_DDR3_INIT_ERROR_STAGE(FX1_DDR3_INIT_STAGE_TRAINING),
           "INIT_ERROR one-hot of the stage in progress");
    expect((c.rd(FX1_DDR3_IRQ_RAW) & (FX1_DDR3_IRQ_PHY | FX1_DDR3_IRQ_INIT_FAIL)) ==
               (FX1_DDR3_IRQ_PHY | FX1_DDR3_IRQ_INIT_FAIL),
           "IRQ_RAW[9] PHY failure and [6] init failure");
    expect(b.snap(FX1_DDR3_PHY_STATUS) & FX1_DDR3_PHY_STATUS_FAILURE, "PHY_STATUS failure (provisional)");
    phy.force_failure(false);
    recover(b, "PHY failure cleared");
}

// ---------------------------------------------------------------- runtime training
void scenario_runtime_training(Bench& b) {
    CsrPort& c = b.csr;
    FunctionalPhy& phy = *b.ctrl->functional_phy();
    const std::uint32_t cmd = FX1_DDR3_TRAIN_CMD_VAL(FX1_DDR3_TRAIN_PRIM_LOAD_GATE, 1, 0xF, 0x1234);
    expect(c.wr(FX1_DDR3_TRAIN_CMD, cmd) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_TRAIN_ST_CODE(b.snap(FX1_DDR3_TRAIN_STATUS)) == FX1_DDR3_TRAIN_CODE_REJECTED,
           "TRAIN_CMD before initialization rejected (code 4)");
    expect(boot(b) && (sc_core::wait(sc_core::SC_ZERO_TIME), b.mission.read()), "booted (ECC off)");
    bool ready_during = true;
    b.at(sc_time(10, SC_NS), [&b, &ready_during] { ready_during = b.mission.read(); });
    expect(c.ok(FX1_DDR3_TRAIN_CMD, cmd), "runtime LOAD_GATE on rank 1 succeeds");
    expect(!ready_during, "mission ready drops while training is busy");
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(b.mission.read(), "mission ready restored after training");
    expect(phy.last_load().tag == 1 && phy.last_load().rank == 1 && phy.last_load().payload == 0x445566,
           "runtime load: tag 1 (boot uses 0), rank 1, gate payload");
    expect(b.snap(FX1_DDR3_TRAIN_STATUS) == FX1_DDR3_TRAIN_STATUS_DONE, "TRAIN_STATUS done, code 0");
    expect(c.rd(FX1_DDR3_TRAIN_OBSERVATION0) == 0x01045566u, "TRAIN_OBSERVATION0 packing (provisional)");
    expect(c.wr(FX1_DDR3_TRAIN_CMD, FX1_DDR3_TRAIN_CMD_VAL(5, 0, 0, 0)) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "reserved primitive rejected");
    phy.script_load_faults({PhyLoadFault::Fail});
    expect(c.wr(FX1_DDR3_TRAIN_CMD, cmd) == tlm::TLM_GENERIC_ERROR_RESPONSE, "failing runtime training -> SLVERR");
    const std::uint32_t st = b.snap(FX1_DDR3_TRAIN_STATUS);
    expect((st & FX1_DDR3_TRAIN_STATUS_ERROR) && FX1_DDR3_TRAIN_ST_CODE(st) == FX1_DDR3_TRAIN_CODE_PHY_FAIL,
           "TRAIN_STATUS error, code PHY_FAIL");
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect((c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_TRAIN_FAIL) && !b.mission.read(),
           "IRQ_RAW[7]; admission stays closed after a training failure");
    expect(c.wr(FX1_DDR3_TRAIN_CMD, cmd) == tlm::TLM_GENERIC_ERROR_RESPONSE, "further training rejected until recovery");
    expect(c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST) && c.ok(FX1_DDR3_TIMING_COMMIT, 1),
           "recovery: quiesce and recommit");
    expect(wait_posedge(b.mission, sc_time(100, SC_US)), "mission ready after reinitialization");
}

// ---------------------------------------------------------------- commit interlock
void scenario_commit_quiesce(Bench& b) {
    CsrPort& c = b.csr;
    expect(boot(b) && (sc_core::wait(sc_core::SC_ZERO_TIME), b.mission.read()), "booted (ECC off)");
    expect(c.wr(FX1_DDR3_TIMING_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_COMMIT_ST_IMM_BLOCKER(c.rd(FX1_DDR3_TIMING_COMMIT_STATUS)) == FX1_DDR3_BLK_NOT_QUIESCED,
           "commit after admission opened needs global quiesce (BE05) -> blocker 0xE0 (provisional)");
    expect(c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST), "global quiesce");
    expect(c.ok(FX1_DDR3_TIMING_COMMIT, 1), "commit accepted once quiesced");
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(!b.mission.read() && !b.initc.read(), "publication cleared readiness and init_complete");
    expect(wait_posedge(b.mission, sc_time(100, SC_US)), "full initialization reran");
    c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_RELEASE);
}

// ---------------------------------------------------------------- power (P-channel)
bool pch_wait(Bench& b, bool want_accept) {
    const sc_time end = sc_core::sc_time_stamp() + sc_time(100, SC_US);
    while (!(b.ctrl->pchannel().paccept || b.ctrl->pchannel().pdeny) && sc_core::sc_time_stamp() < end)
        sc_core::wait(sc_time(100, SC_NS), b.ctrl->pchannel_event());
    return want_accept ? b.ctrl->pchannel().paccept : b.ctrl->pchannel().pdeny;
}

void pch_release(Bench& b) {
    b.ctrl->pchannel_request(false, b.ctrl->pchannel().pstate);
    expect(!b.ctrl->pchannel().paccept && !b.ctrl->pchannel().pdeny, "responses drop after PREQ falls (4-phase)");
}

void scenario_power(Bench& b) {
    CsrPort& c = b.csr;
    Ddr3Controller& d = *b.ctrl;
    d.pchannel_request(true, FX1_DDR3_PSTATE_ACTIVE_POWER_DOWN);
    expect(pch_wait(b, false) && (c.rd(FX1_DDR3_POWER_ERROR) & FX1_DDR3_POWER_ERR_DENIED_BUSY),
           "power request before initialization denied (busy)");
    pch_release(b);
    c.ok(FX1_DDR3_POWER_ERROR, 0xFFFFFF);
    expect(boot(b) && (sc_core::wait(sc_core::SC_ZERO_TIME), b.mission.read()), "booted (ECC off)");
    // Active power-down.
    d.pchannel_request(true, FX1_DDR3_PSTATE_ACTIVE_POWER_DOWN);
    expect(pch_wait(b, true), "APD accepted");
    std::uint32_t ps = b.snap(FX1_DDR3_POWER_STATUS);
    expect(FX1_DDR3_POWER_ST_RESIDENT(ps) == FX1_DDR3_PSTATE_ACTIVE_POWER_DOWN && (ps & FX1_DDR3_POWER_ST_ACCEPT) &&
               !(ps & FX1_DDR3_POWER_ST_ACTIVE),
           "POWER_STATUS resident APD, accept");
    expect(!b.mission.read() && !d.pchannel().pactive, "admission closed in power-down; PACTIVE low");
    pch_release(b);
    d.pchannel_request(true, FX1_DDR3_PSTATE_SELF_REFRESH);
    expect(pch_wait(b, false) && (c.rd(FX1_DDR3_POWER_ERROR) & FX1_DDR3_POWER_ERR_DENIED_ILLEGAL),
           "APD -> SR denied: active first (MAS-adopted)");
    pch_release(b);
    // Refresh still falls due in power-down: PACTIVE rises at high urgency (4 x tREFI).
    sc_core::wait(sc_time(32, SC_US));
    expect(d.pchannel().pactive && FX1_DDR3_REFRESH_RANK_URGENCY(b.snap(FX1_DDR3_REFRESH_RANK(0))) ==
                                       FX1_DDR3_REFRESH_URG_HIGH,
           "refresh urgency high after 4 intervals in APD -> PACTIVE");
    d.pchannel_request(true, FX1_DDR3_PSTATE_ACTIVE);
    expect(pch_wait(b, true), "wake to ACTIVE accepted");
    pch_release(b);
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(b.mission.read() && d.pchannel().pactive, "mission ready after wake");
    // Self-refresh holds the refresh debt.
    d.pchannel_request(true, FX1_DDR3_PSTATE_SELF_REFRESH);
    expect(pch_wait(b, true) && FX1_DDR3_POWER_ST_RESIDENT(b.snap(FX1_DDR3_POWER_STATUS)) == FX1_DDR3_PSTATE_SELF_REFRESH,
           "SR accepted and resident");
    pch_release(b);
    const int held = d.refresh_debt(0);
    sc_core::wait(sc_time(50, SC_US));
    expect(d.refresh_debt(0) == held, "self-refresh holds ordinary debt");
    // Wake failure: the PHY does not become ready -> post-cutoff timeout -> recovery.
    d.functional_phy()->force_ready(false);
    d.pchannel_request(true, FX1_DDR3_PSTATE_ACTIVE);
    expect(pch_wait(b, false), "wake without PHY ready -> PDENY after POWER_TIMEOUT");
    ps = b.snap(FX1_DDR3_POWER_STATUS);
    expect((ps & FX1_DDR3_POWER_ST_FATAL_RECOVERY) && (ps & FX1_DDR3_POWER_ST_DENY) &&
               (c.rd(FX1_DDR3_POWER_ERROR) & (FX1_DDR3_POWER_ERR_TIMEOUT_POST_CUTOFF | FX1_DDR3_POWER_ERR_WAKE_PHY_NOT_READY)) ==
                   (FX1_DDR3_POWER_ERR_TIMEOUT_POST_CUTOFF | FX1_DDR3_POWER_ERR_WAKE_PHY_NOT_READY),
           "POWER_STATUS[5] recovery required; POWER_ERROR post-cutoff timeout, PHY not ready");
    expect(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_POWER_FAIL, "IRQ_RAW[8]");
    pch_release(b);
    expect(c.wr(FX1_DDR3_TOPO_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_COMMIT_ST_IMM_BLOCKER(c.rd(FX1_DDR3_TOPO_COMMIT_STATUS)) == FX1_DDR3_BLK_RECOVERY_REQUIRED,
           "commits rejected while recovery is required");
    expect(!b.mission.read() && d.recovery_required(), "admission stays closed");
}

// ---------------------------------------------------------------- refresh debt
void scenario_refresh(Bench& b) {
    CsrPort& c = b.csr;
    Ddr3Controller& d = *b.ctrl;
    d.fault_block_refresh(0, true);  // rank 0 misses every deadline from init done
    expect(boot(b), "booted (ECC off)");
    const sc_time t0 = sc_core::sc_time_stamp();
    const sc_time refi(7800, SC_NS);  // 6240 tCK at 1.25 ns
    auto sample_at = [&](double intervals) {
        sc_core::wait(t0 + refi * intervals - sc_core::sc_time_stamp());
        return b.snap(FX1_DDR3_REFRESH_RANK(0));
    };
    expect(sample_at(3.1) == (3u | (FX1_DDR3_REFRESH_URG_NORMAL << 5)), "3 missed: debt 3, normal urgency");
    expect(sample_at(4.1) == (4u | (FX1_DDR3_REFRESH_URG_HIGH << 5)), "4 missed: debt 4, high (BE04: ceil(8/2))");
    expect(c.rd(FX1_DDR3_REFRESH_STATUS) == FX1_DDR3_REFRESH_STATUS_ANY_HIGH, "REFRESH_STATUS any-high");
    expect(sample_at(8.1) == (8u | (FX1_DDR3_REFRESH_URG_MANDATORY << 5)), "8 missed: debt 8 = limit, mandatory");
    expect(c.rd(FX1_DDR3_REFRESH_STATUS) == (FX1_DDR3_REFRESH_STATUS_R0_MANDATORY | FX1_DDR3_REFRESH_STATUS_ANY_HIGH |
                                             FX1_DDR3_REFRESH_STATUS_ANY_MANDATORY),
           "REFRESH_STATUS rank-0 mandatory");
    expect(!(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_REFRESH), "no emergency before saturation");
    sc_core::wait(t0 + refi * 9.0 - sc_core::sc_time_stamp());
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_REFRESH, "9th missed interval saturates: IRQ_RAW[5] at 70.2 us");
    expect(b.snap(FX1_DDR3_REFRESH_RANK(0)) == (8u | (FX1_DDR3_REFRESH_URG_MANDATORY << 5)) &&
               (c.rd(FX1_DDR3_REFRESH_STATUS) & FX1_DDR3_REFRESH_STATUS_SATURATION),
           "debt clamped at the limit, saturation reported");
    expect(b.snap(FX1_DDR3_REFRESH_RANK(1)) == 0, "rank 1 unaffected");
    d.fault_block_refresh(0, false);
    sc_core::wait(sc_time(1100, SC_NS));  // 4 refreshes of tRFC = 260 ns
    expect((b.snap(FX1_DDR3_REFRESH_RANK(0)) & 0x1F) == 4, "debt repaid one refresh per tRFC");
    sc_core::wait(sc_time(1100, SC_NS));
    expect(b.snap(FX1_DDR3_REFRESH_RANK(0)) == 0, "debt fully repaid");
}

// ---------------------------------------------------------------- reset/failure retirement (S2 review)
bool ready_now(Bench& b) { return b.ctrl->mission_ready(); }
bool boot_ready(Bench& b) {
    return boot(b) && b.wait_until([&b] { return ready_now(b); }, sc_time(10, SC_US), sc_time(10, SC_NS));
}
bool reprogram_ready(Bench& b) {
    const BuildConfig& cfg = b.ctrl->config();
    return program_and_commit(b, cfg, cfg.max_ranks(), 16, FX1_DDR3_TOPO_PAGE_1024) &&
           b.wait_until([&b] { return ready_now(b); }, sc_time(100, SC_US), sc_time(10, SC_NS));
}
const std::uint32_t kGate = FX1_DDR3_TRAIN_CMD_VAL(FX1_DDR3_TRAIN_PRIM_LOAD_GATE, 0, 0xF, 0);

void scenario_cancel_training(Bench& b) {
    CsrPort& c = b.csr;
    FunctionalPhy& phy = *b.ctrl->functional_phy();
    expect(boot_ready(b), "booted");
    // R1: CSR reset while a runtime TRAIN_CMD waits for the PHY (load latency 50 ns).
    b.at(sc_time(10, SC_NS), [&b] { b.ctrl->set_csr_reset(true); });
    b.at(sc_time(20, SC_NS), [&b] { b.ctrl->set_csr_reset(false); });
    expect(c.wr(FX1_DDR3_TRAIN_CMD, kGate) == tlm::TLM_GENERIC_ERROR_RESPONSE, "CSR reset: TRAIN_CMD ends with SLVERR");
    sc_core::wait(10, SC_US);
    const std::uint32_t st = b.snap(FX1_DDR3_TRAIN_STATUS);
    expect((st & FX1_DDR3_TRAIN_STATUS_ERROR) && !(st & FX1_DDR3_TRAIN_STATUS_BUSY) &&
               FX1_DDR3_TRAIN_ST_CODE(st) == FX1_DDR3_TRAIN_CODE_CANCELLED,
           "TRAIN_STATUS: not busy, error, cancelled (provisional code 5)");
    expect(c.ok(FX1_DDR3_MR(0), 0), "INIT_RW unlocked after the cancelled command");
    expect(!ready_now(b), "rank delay state unknown: readiness stays closed until recovery");
    // The CSR reset returned every CSR-side word (shadows included) to reset: firmware
    // quiesces and reprograms the full image (VPL-07).
    expect(c.rd(FX1_DDR3_TIMING_SHADOW(FX1_DDR3_T_CL)) == 0, "CSR reset cleared the shadows");
    expect(c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST) && reprogram_ready(b),
           "recovery: quiesce + reprogram restores mission readiness");
    expect(c.ok(FX1_DDR3_TRAIN_CMD, kGate), "runtime training works again");
    // CSR reset while the PHY never answers (lost observation).
    phy.script_load_faults({PhyLoadFault::Lose});
    b.at(sc_time(30, SC_NS), [&b] { b.ctrl->set_csr_reset(true); });
    b.at(sc_time(40, SC_NS), [&b] { b.ctrl->set_csr_reset(false); });
    expect(c.wr(FX1_DDR3_TRAIN_CMD, kGate) == tlm::TLM_GENERIC_ERROR_RESPONSE, "lost response + CSR reset: SLVERR");
    sc_core::wait(10, SC_US);  // the retired episode's TRAIN_TIMEOUT must not fire into later state
    expect(!(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_TRAIN_FAIL), "no late training IRQ from the retired episode");
    b.ctrl->set_cold_reset(true);
    b.ctrl->set_cold_reset(false);
    expect(reprogram_ready(b), "cold reset + reprogramming restores mission readiness");
    // R1 (core reset): the mutex is released and the shadows can be recommitted.
    b.at(sc_time(10, SC_NS), [&b] { b.ctrl->set_core_reset(true); });
    b.at(sc_time(11, SC_NS), [&b] { b.ctrl->set_core_reset(false); });
    expect(c.wr(FX1_DDR3_TRAIN_CMD, kGate) == tlm::TLM_GENERIC_ERROR_RESPONSE, "core reset: TRAIN_CMD ends with SLVERR");
    expect(!(c.rd(FX1_DDR3_COMMAND_ERROR) & FX1_DDR3_CMD_ERR_BUSY), "...without stranding the mutex");
    expect(c.ok(FX1_DDR3_TOPO_COMMIT, 1) && c.ok(FX1_DDR3_TIMING_COMMIT, 1) && c.ok(FX1_DDR3_DFI_TIMING_COMMIT, 1) &&
               b.wait_until([&b] { return ready_now(b); }, sc_time(100, SC_US), sc_time(10, SC_NS)),
           "retained shadows recommitted after core reset; mission ready");
    expect(c.ok(FX1_DDR3_TRAIN_CMD, kGate), "runtime training after core-reset recovery");
}

void scenario_cancel_power(Bench& b) {
    CsrPort& c = b.csr;
    Ddr3Controller& d = *b.ctrl;
    expect(boot_ready(b), "booted");
    // R2: core reset before (1 ns) and after (300 ns > tRP + tRFC = 275 ns) the cutoff.
    // SR entry: prepare tRP + tRFC = 275 ns (cutoff), entry tCKSRE + tCKESR = 7.5 ns more.
    for (const double at_ns : {1.0, 279.0}) {
        const std::string tag = at_ns < 10 ? "pre-cutoff: " : "post-cutoff: ";
        d.pchannel_request(true, FX1_DDR3_PSTATE_SELF_REFRESH);
        sc_core::wait(sc_time(at_ns, SC_NS));
        d.set_core_reset(true);
        sc_core::wait(1, SC_NS);
        d.set_core_reset(false);
        sc_core::wait(sc_core::SC_ZERO_TIME);
        expect(d.pchannel().pdeny && !d.pchannel().paccept, tag + "pending P-channel request answered with PDENY");
        pch_release(b);
        expect(c.ok(FX1_DDR3_TOPO_COMMIT, 1), tag + "mutex released: commit accepted");
        expect(!(c.rd(FX1_DDR3_COMMAND_ERROR) & FX1_DDR3_CMD_ERR_BUSY), tag + "no busy collision");
        expect(c.ok(FX1_DDR3_TIMING_COMMIT, 1) && c.ok(FX1_DDR3_DFI_TIMING_COMMIT, 1) &&
                   b.wait_until([&b] { return ready_now(b); }, sc_time(100, SC_US), sc_time(10, SC_NS)),
               tag + "reinitialized");
        d.pchannel_request(true, FX1_DDR3_PSTATE_ACTIVE_POWER_DOWN);
        expect(pch_wait(b, true), tag + "a new power request is accepted");
        pch_release(b);
        d.pchannel_request(true, FX1_DDR3_PSTATE_ACTIVE);
        expect(pch_wait(b, true), tag + "wake accepted");
        pch_release(b);
    }
    // CSR reset during a power transition: the core-owned mutex stays with the transaction.
    d.pchannel_request(true, FX1_DDR3_PSTATE_SELF_REFRESH);
    sc_core::wait(1, SC_NS);
    d.set_csr_reset(true);
    d.set_csr_reset(false);
    expect(c.wr(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "CSR reset does not free a mutex owned by the power transaction");
    expect(pch_wait(b, true), "the transition still completes");
    pch_release(b);
    expect(c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST), "mutex free after the transition");
}

void scenario_phy_failure_training(Bench& b) {
    CsrPort& c = b.csr;
    FunctionalPhy& phy = *b.ctrl->functional_phy();
    // R3 boot: failure (ready stays high) during the first boot-training load.
    const BuildConfig& cfg = b.ctrl->config();
    expect(program_and_commit(b, cfg, 2, 16, FX1_DDR3_TOPO_PAGE_1024), "programmed");
    expect(b.wait_until([&phy] { return phy.loads_received() > 0; }, sc_time(100, SC_US), sc_time(1, SC_NS)),
           "boot training started");
    phy.force_failure(true);
    expect(phy.levels().ready, "failure asserted while ready stays high");
    sc_core::wait(10, SC_US);
    expect(b.ctrl->init_failed() && b.ctrl->init_stage() == FX1_DDR3_INIT_STAGE_FAILED && !b.ctrl->init_complete() &&
               phy.loads_received() == 1,
           "boot stays FAILED: no later success, init_complete low, no further loads");
    expect(c.rd(FX1_DDR3_INIT_ERROR) == FX1_DDR3_INIT_ERROR_STAGE(FX1_DDR3_INIT_STAGE_TRAINING) &&
               (c.rd(FX1_DDR3_IRQ_RAW) & (FX1_DDR3_IRQ_PHY | FX1_DDR3_IRQ_INIT_FAIL | FX1_DDR3_IRQ_TRAIN_FAIL)) ==
                   (FX1_DDR3_IRQ_PHY | FX1_DDR3_IRQ_INIT_FAIL | FX1_DDR3_IRQ_TRAIN_FAIL),
           "training-stage failure reported with IRQ_RAW[9], [6], [7]");
    phy.force_failure(false);
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
    expect(c.ok(FX1_DDR3_TIMING_COMMIT, 1) &&
               b.wait_until([&b] { return ready_now(b); }, sc_time(100, SC_US), sc_time(10, SC_NS)),
           "recovery after the failure clears");
    // R3 runtime: failure 10 ns into LOAD_GATE.
    b.at(sc_time(10, SC_NS), [&phy] { phy.force_failure(true); });
    expect(c.wr(FX1_DDR3_TRAIN_CMD, kGate) == tlm::TLM_GENERIC_ERROR_RESPONSE, "runtime training fails with SLVERR");
    const std::uint32_t st = b.snap(FX1_DDR3_TRAIN_STATUS);
    expect((st & FX1_DDR3_TRAIN_STATUS_ERROR) && FX1_DDR3_TRAIN_ST_CODE(st) == FX1_DDR3_TRAIN_CODE_PHY_FAIL,
           "TRAIN_STATUS error, PHY_FAIL");
    expect((c.rd(FX1_DDR3_IRQ_RAW) & (FX1_DDR3_IRQ_PHY | FX1_DDR3_IRQ_TRAIN_FAIL)) ==
               (FX1_DDR3_IRQ_PHY | FX1_DDR3_IRQ_TRAIN_FAIL) && !ready_now(b),
           "IRQ_RAW[7] and [9]; readiness closed");
    sc_core::wait(1, SC_US);
    expect(FX1_DDR3_TRAIN_ST_CODE(b.snap(FX1_DDR3_TRAIN_STATUS)) == FX1_DDR3_TRAIN_CODE_PHY_FAIL,
           "the late PHY response does not turn the failure into success");
}

void scenario_cancel_fe_port(Bench& b) {
    CsrPort& c = b.csr;
    Ddr3Controller& d = *b.ctrl;
    expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0)), "quiesce port 0");
    // R4: core reset after the endpoint wait (10 ns), during the 20 ns hold.
    b.at(sc_time(15, SC_NS), [&d] { d.set_core_reset(true); });
    b.at(sc_time(16, SC_NS), [&d] { d.set_core_reset(false); });
    expect(c.wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_SOFT_RESET, 0)) ==
               tlm::TLM_GENERIC_ERROR_RESPONSE,
           "soft reset cancelled during the hold -> SLVERR");
    std::uint32_t st = c.rd(FX1_DDR3_FE_PORT_CMD_STATUS);
    expect((st & FX1_DDR3_FE_PORT_ST_ERROR) && !(st & FX1_DDR3_FE_PORT_ST_DONE) &&
               FX1_DDR3_FE_PORT_ST_BLOCKER(st) == FX1_DDR3_BLK_CANCELLED,
           "FE_PORT_CMD_STATUS error, blocker cancelled (provisional 0xE2)");
    // Core reset during the endpoint wait itself.
    expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0)), "quiesce again");
    b.at(sc_time(5, SC_NS), [&d] { d.set_core_reset(true); });
    b.at(sc_time(6, SC_NS), [&d] { d.set_core_reset(false); });
    expect(c.wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_SOFT_RESET, 0)) ==
               tlm::TLM_GENERIC_ERROR_RESPONSE,
           "soft reset cancelled during the endpoint wait -> SLVERR");
    // Port reset during the hold.
    expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0)), "quiesce again");
    b.at(sc_time(15, SC_NS), [&d] { d.set_port_reset(0, true); });
    b.at(sc_time(16, SC_NS), [&d] { d.set_port_reset(0, false); });
    expect(c.wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_SOFT_RESET, 0)) ==
               tlm::TLM_GENERIC_ERROR_RESPONSE,
           "soft reset cancelled by a port reset -> SLVERR");
    // CSR reset during the hold: the command fails and the mutex is free afterwards.
    expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0)), "quiesce again");
    b.at(sc_time(15, SC_NS), [&d] { d.set_csr_reset(true); });
    b.at(sc_time(16, SC_NS), [&d] { d.set_csr_reset(false); });
    expect(c.wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_SOFT_RESET, 0)) ==
               tlm::TLM_GENERIC_ERROR_RESPONSE,
           "soft reset cancelled by a CSR reset -> SLVERR");
    expect(c.rd(FX1_DDR3_FE_PORT_CMD_STATUS) == 0, "CSR reset values kept (status not written after the reset)");
    // S2-R4a: every FE_PORT op is cancelled by a port reset pulse entirely inside the
    // endpoint wait (+5/+6 ns of 10 ns) and by a port reset held through the wait.
    struct Op { unsigned op; const char* name; };
    const Op ops[] = {{FX1_DDR3_FE_OP_QUIESCE, "QUIESCE"}, {FX1_DDR3_FE_OP_RELEASE, "RELEASE"},
                      {FX1_DDR3_FE_OP_SOFT_RESET, "SOFT_RESET"}};
    for (const Op& o : ops) {
        for (const bool pulse : {true, false}) {
            const std::string what = std::string(o.name) + (pulse ? " with a reset pulse inside the wait"
                                                                   : " with a reset held through the wait");
            // Soft reset needs the quiesce ack; release needs something to release.
            if (o.op != FX1_DDR3_FE_OP_QUIESCE)
                c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0));
            b.at(sc_time(5, SC_NS), [&d] { d.set_port_reset(0, true); });
            b.at(sc_time(pulse ? 6.0 : 40.0, SC_NS), [&d] { d.set_port_reset(0, false); });
            expect(c.wr(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(o.op, 0)) == tlm::TLM_GENERIC_ERROR_RESPONSE,
                   what + " -> SLVERR");
            st = c.rd(FX1_DDR3_FE_PORT_CMD_STATUS);
            expect((st & FX1_DDR3_FE_PORT_ST_ERROR) && !(st & FX1_DDR3_FE_PORT_ST_DONE) &&
                       FX1_DDR3_FE_PORT_ST_OP(st) == o.op && FX1_DDR3_FE_PORT_ST_BLOCKER(st) == FX1_DDR3_BLK_CANCELLED,
                   what + ": status error, op, blocker cancelled");
            expect(!(c.rd(FX1_DDR3_COMMAND_ERROR) & FX1_DDR3_CMD_ERR_BUSY), what + ": mutex not stranded");
            sc_core::wait(sc_time(50, SC_NS));  // the held reset is released by now
        }
        // Uninterrupted success for the same op.
        if (o.op != FX1_DDR3_FE_OP_QUIESCE)
            c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0));
        expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(o.op, 0)) &&
                   (c.rd(FX1_DDR3_FE_PORT_CMD_STATUS) & FX1_DDR3_FE_PORT_ST_DONE),
               std::string(o.name) + " uninterrupted -> DONE");
    }
    // An uninterrupted soft reset still succeeds.
    expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0)) &&
               c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_SOFT_RESET, 0)) &&
               (c.rd(FX1_DDR3_FE_PORT_CMD_STATUS) & FX1_DDR3_FE_PORT_ST_DONE),
           "uninterrupted soft reset completes with DONE");
}

void scenario_cancel_snapshot(Bench& b) {
    CsrPort& c = b.csr;
    Ddr3Controller& d = *b.ctrl;
    // A capture cancelled by a CSR reset must not clear the busy flag of a newer capture.
    b.at(sc_time(3, SC_NS), [&d] { d.set_csr_reset(true); });
    b.at(sc_time(4, SC_NS), [&d] { d.set_csr_reset(false); });
    tlm::tlm_response_status second = tlm::TLM_INCOMPLETE_RESPONSE;
    b.at(sc_time(5, SC_NS), [&b, &second] { second = b.csr2.wr(FX1_DDR3_SNAPSHOT_CMD, 1); });
    expect(c.wr(FX1_DDR3_SNAPSHOT_CMD, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE, "capture cancelled by CSR reset");
    // The old command woke at 10 ns and returned at 12.5 ns; the newer capture (5..15 ns)
    // must still be busy (side-effect-free debug read).
    unsigned char buf[4]{};
    tlm::tlm_generic_payload tx;
    tx.set_command(tlm::TLM_READ_COMMAND);
    tx.set_address(FX1_DDR3_SNAPSHOT_STATUS);
    tx.set_data_ptr(buf);
    tx.set_data_length(4);
    expect(sc_core::sc_time_stamp() == sc_time(12.5, SC_NS) && b.mux_out->transport_dbg(tx) == 4 &&
               (buf[0] & FX1_DDR3_SNAPSHOT_ST_BUSY),
           "the newer capture stays busy after the old command wakes up");
    sc_core::wait(20, SC_NS);
    expect(second == tlm::TLM_OK_RESPONSE && FX1_DDR3_SNAPSHOT_ST_SEQ(c.rd(FX1_DDR3_SNAPSHOT_STATUS)) == 1 &&
               (c.rd(FX1_DDR3_SNAPSHOT_STATUS) & FX1_DDR3_SNAPSHOT_ST_VALID),
           "the newer capture completes: valid, sequence 1");
}

void scenario_train_fields(Bench& b) {
    CsrPort& c = b.csr;
    FunctionalPhy& phy = *b.ctrl->functional_phy();
    expect(boot_ready(b), "booted");
    expect(phy.last_load().lane_mask == 0xF && phy.last_load().options == 0 && phy.last_load().tag == 0,
           "boot training loads all byte lanes with no options");
    // R5: lane mask and options reach the provider.
    expect(c.ok(FX1_DDR3_TRAIN_CMD, FX1_DDR3_TRAIN_CMD_VAL(FX1_DDR3_TRAIN_PRIM_LOAD_GATE, 0, 0x1, 0)), "lanes 0x1");
    const PhyLoadRequest a = phy.last_load();
    expect(c.ok(FX1_DDR3_TRAIN_CMD, FX1_DDR3_TRAIN_CMD_VAL(FX1_DDR3_TRAIN_PRIM_LOAD_GATE, 0, 0x8, 0x1234)),
           "lanes 0x8, options 0x1234");
    const PhyLoadRequest z = phy.last_load();
    expect(a.lane_mask == 0x1 && a.options == 0 && z.lane_mask == 0x8 && z.options == 0x1234,
           "provider receives the selected lanes and opaque options");
    expect(c.wr(FX1_DDR3_TRAIN_CMD, FX1_DDR3_TRAIN_CMD_VAL(FX1_DDR3_TRAIN_PRIM_LOAD_GATE, 0, 0x0, 0)) ==
                   tlm::TLM_GENERIC_ERROR_RESPONSE &&
               FX1_DDR3_TRAIN_ST_CODE(b.snap(FX1_DDR3_TRAIN_STATUS)) == FX1_DDR3_TRAIN_CODE_REJECTED,
           "no lane selected -> rejected, not acknowledged");
    expect(phy.last_load().lane_mask == 0x8, "the rejected command issued no load");
}

} // namespace

int sc_main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    BuildConfig cfg = BuildConfig::native_r2();
    cfg.n_ports = 1;
    BuildConfig no_ecc = cfg;
    no_ecc.ecc_enable = false;
    if (which == "boot_ecc") return run_bench(cfg, scenario_boot_ecc, "lifecycle boot_ecc");
    if (which == "boot_no_ecc") return run_bench(no_ecc, scenario_boot_no_ecc, "lifecycle boot_no_ecc");
    if (which == "boot_legacy") {
        BuildConfig leg = BuildConfig::legacy();
        leg.n_ports = 1;
        leg.ecc_enable = false;
        return run_bench(leg, scenario_boot_legacy, "lifecycle boot_legacy");
    }
    if (which == "init_fail") return run_bench(no_ecc, scenario_init_fail, "lifecycle init_fail");
    if (which == "phy_failure") return run_bench(no_ecc, scenario_phy_failure, "lifecycle phy_failure");
    if (which == "runtime_training") return run_bench(no_ecc, scenario_runtime_training, "lifecycle runtime_training");
    if (which == "commit_quiesce") return run_bench(no_ecc, scenario_commit_quiesce, "lifecycle commit_quiesce");
    if (which == "power") return run_bench(no_ecc, scenario_power, "lifecycle power");
    if (which == "refresh") return run_bench(no_ecc, scenario_refresh, "lifecycle refresh");
    if (which == "cancel_training") return run_bench(no_ecc, scenario_cancel_training, "lifecycle cancel_training");
    if (which == "cancel_power") return run_bench(no_ecc, scenario_cancel_power, "lifecycle cancel_power");
    if (which == "phy_failure_training")
        return run_bench(no_ecc, scenario_phy_failure_training, "lifecycle phy_failure_training");
    if (which == "cancel_fe_port") return run_bench(no_ecc, scenario_cancel_fe_port, "lifecycle cancel_fe_port");
    if (which == "cancel_snapshot") return run_bench(no_ecc, scenario_cancel_snapshot, "lifecycle cancel_snapshot");
    if (which == "train_fields") return run_bench(no_ecc, scenario_train_fields, "lifecycle train_fields");
    std::cerr << "unknown case '" << which << "'\n";
    return 2;
}
