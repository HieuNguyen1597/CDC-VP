// SPDX-License-Identifier: Apache-2.0
// Subtask 4 conformance regressions: requirements left partial by earlier subtasks, a seeded
// randomized soak (reproducible: the seed and a digest of every response are printed), and
// efficiency checks (no per-cycle activity while idle, storage proportional to touched data).
#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "ddr3_bench.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <set>

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
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i * 13u + seed * 7u + 5u);
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

AxiOut long_read(MemPort& m, std::uint32_t off, unsigned id = 0) {
    return m.axi(make(kBase + off, AxiBurst::Incr, 255, 4, false, id));
}

bool pchannel(Bench& b, std::uint8_t state) {
    Ddr3Controller& d = *b.ctrl;
    d.pchannel_request(true, state);
    const bool ok = b.wait_until([&] { return d.pchannel().paccept || d.pchannel().pdeny; }, sc_time(500, SC_US),
                                 sc_time(5, SC_NS)) &&
                    d.pchannel().paccept;
    d.pchannel_request(false, state);
    sc_core::wait(5, SC_NS);
    return ok;
}

// IF-1.5-02: a CSR read of another master completes while a commit is blocked on its drain.
void scenario_csr_during_commit(Bench& b) {
    expect(boot_memory(b), "boot");
    Join j;
    AxiOut r;
    tlm::tlm_response_status commit = tlm::TLM_INCOMPLETE_RESPONSE;
    sc_time commit_end;
    j.spawn([&] { r = long_read(*b.mem[0], 0x1000); });
    j.spawn([&] {
        sc_core::wait(2, SC_NS);
        b.csr2.ok(FX1_DDR3_FE_REGION_SHADOW(0), kBase);
        b.csr2.ok(FX1_DDR3_FE_REGION_SHADOW(1), 0x02000000u);
        commit = b.csr2.wr(FX1_DDR3_FE_REGION_COMMIT, 1);
        commit_end = sc_core::sc_time_stamp();
    });
    sc_core::wait(20, SC_NS);
    const sc_time t0 = sc_core::sc_time_stamp();
    const std::uint32_t id = b.csr.rd(FX1_DDR3_IP_ID);
    const sc_time read_time = sc_core::sc_time_stamp() - t0;
    j.wait();
    expect(id == caps::kIpId && read_time <= sc_time(5, SC_NS) && commit == tlm::TLM_OK_RESPONSE &&
               commit_end >= r.end && t0 < r.end,
           "CSR read answered in one cycle while the other master's commit waits for the drain");
}

// IF-1.5-08: global_idle and fe_drained levels on the output ports follow the traffic.
void scenario_levels(Bench& b) {
    expect(boot_memory(b), "boot");
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(b.idle.read() && b.drained.read(), "idle and drained when no transaction is owned");
    Join j;
    j.spawn([&] { (void)long_read(*b.mem[0], 0x2000); });
    sc_core::wait(5, SC_NS);
    expect(!b.idle.read() && !b.drained.read() && !b.ctrl->global_idle(), "levels drop while a transaction is owned");
    j.wait();
    sc_core::wait(sc_core::SC_ZERO_TIME);
    sc_core::wait(sc_core::SC_ZERO_TIME);
    expect(b.idle.read() && b.drained.read(), "levels return after the response");
}

// IF-1.5-10: the PHY clock is disabled only after a completed self-refresh entry.
void scenario_clock_gating(Bench& b) {
    expect(boot_memory(b), "boot");
    FunctionalPhy& phy = *b.ctrl->functional_phy();
    expect(!phy.clock_disabled(), "clock enabled through init, training and traffic");
    expect(pchannel(b, FX1_DDR3_PSTATE_ACTIVE_POWER_DOWN) && !phy.clock_disabled(), "active power-down keeps the clock");
    expect(pchannel(b, FX1_DDR3_PSTATE_ACTIVE), "wake from APD");
    expect(pchannel(b, FX1_DDR3_PSTATE_SELF_REFRESH) && phy.clock_disabled(), "self-refresh entry disables the clock");
    expect(pchannel(b, FX1_DDR3_PSTATE_ACTIVE) && !phy.clock_disabled() && b.ctrl->mission_ready(),
           "the wake enables it again before the exit timing");
    b.ctrl->pchannel_request(true, FX1_DDR3_PSTATE_SELF_REFRESH);
    b.wait_until([&] { return b.ctrl->pchannel().paccept; }, sc_time(500, SC_US), sc_time(5, SC_NS));
    b.ctrl->set_core_reset(true);
    b.ctrl->set_core_reset(false);
    expect(!phy.clock_disabled(), "a core reset re-enables the clock (reinitialization)");
}

// LC-7.1-04: a port soft reset never deletes accepted work; it starts after the ack.
void scenario_soft_reset_backend(Bench& b) {
    expect(boot_memory(b), "boot");
    Ddr3Controller& d = *b.ctrl;
    Join j;
    AxiOut w;
    const std::vector<std::uint8_t> v = pattern(4096, 1);
    j.spawn([&] { w = b.mem[0]->axi(make(kBase + 0x3000, AxiBurst::Incr, 255, 4, true), v); });
    sc_core::wait(2, SC_NS);
    d.port_quiesce_request(0, true);
    d.port_soft_reset_request(0, true);
    sc_core::wait(5, SC_NS);
    expect(d.port_soft_reset_pending(0) && !d.port_soft_reset_active(0), "soft reset pending while the write is in flight");
    j.wait();
    sc_core::wait(sc_core::SC_ZERO_TIME);
    std::vector<std::uint8_t> got(4096);
    d.debug_read(0x3000, got.data(), got.size());
    expect(w.resp == AxiResp::Okay && got == v && d.port_soft_reset_active(0), "the write completed; the hold starts after it");
    d.port_soft_reset_request(0, false);
    sc_core::wait(30, SC_NS);
    d.port_quiesce_request(0, false);
    d.qos_set_configured(0, true);
    expect(b.mem[0]->read(kBase + 0x3000, got, 16) == AxiResp::Okay, "port usable after the hold and QoS reprogramming");
}

// CSR-8.1-10: a commit needs a quiesced and idle controller; owned work rejects it.
void scenario_commit_not_idle(Bench& b) {
    expect(boot_memory(b), "boot");
    CsrPort& c = b.csr;
    Join j;
    AxiOut r;
    j.spawn([&] { r = long_read(*b.mem[0], 0x4000); });
    sc_core::wait(2, SC_NS);
    expect(c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST), "global quiesce requested");
    expect(!(c.rd(FX1_DDR3_QUIESCE_STATUS) & FX1_DDR3_QUIESCE_ST_COMPLETE), "not complete while the read is owned");
    expect(c.wr(FX1_DDR3_TOPO_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
               ((c.rd(FX1_DDR3_TOPO_COMMIT_STATUS) >> 16) & 0xFFu) == FX1_DDR3_BLK_NOT_QUIESCED,
           "TOPO_COMMIT with owned work: SLVERR, immediate blocker not-quiesced");
    j.wait();
    expect(c.rd(FX1_DDR3_QUIESCE_STATUS) & FX1_DDR3_QUIESCE_ST_COMPLETE, "complete after the drain");
    expect(c.ok(FX1_DDR3_TOPO_COMMIT, 1), "the same commit is accepted once idle");
    c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_RELEASE);
    expect(wait_mission(b, sc_time(100, SC_MS)), "reinitialized");
}

// CSR-8.1-13: a failure on the acknowledged path reports the blocker in [15:8].
void scenario_ack_blocker(Bench& b) {
    expect(boot_memory(b), "boot");
    CsrPort& c = b.csr;
    Join j;
    AxiOut r;
    tlm::tlm_response_status commit = tlm::TLM_INCOMPLETE_RESPONSE;
    j.spawn([&] { r = long_read(*b.mem[0], 0x5000); });
    j.spawn([&] {
        sc_core::wait(2, SC_NS);
        b.csr2.ok(FX1_DDR3_FE_REGION_SHADOW(0), kBase);
        b.csr2.ok(FX1_DDR3_FE_REGION_SHADOW(1), 0x02000000u);
        commit = b.csr2.wr(FX1_DDR3_FE_REGION_COMMIT, 1);
    });
    b.at(sc_time(30, SC_NS), [&b] { b.ctrl->set_core_reset(true); });
    b.at(sc_time(31, SC_NS), [&b] { b.ctrl->set_core_reset(false); });
    j.wait();
    const std::uint32_t st = c.rd(FX1_DDR3_FE_REGION_STATUS);
    expect(commit == tlm::TLM_GENERIC_ERROR_RESPONSE && (st & FX1_DDR3_FE_REGION_ST_ERROR) &&
               ((st >> 8) & 0xFFu) == FX1_DDR3_BLK_CANCELLED && !(st & FX1_DDR3_FE_REGION_ST_VALID),
           "publication cancelled during its drain: SLVERR, blocker 0xE2 in [15:8], no image");
    expect(c.rd(FX1_DDR3_COMMAND_ERROR) & FX1_DDR3_CMD_ERR_BLOCKED, "COMMAND_ERROR blocked");
}

// CSR-8.1-22: core offline clears FE_REGION_ACTIVE; decode returns to the bootstrap map.
void scenario_core_offline_map(Bench& b) {
    expect(boot_memory(b), "boot");
    CsrPort& c = b.csr;
    MemPort& m = *b.mem[0];
    c.ok(FX1_DDR3_FE_REGION_SHADOW(0), 0x90000000u);
    c.ok(FX1_DDR3_FE_REGION_SHADOW(1), 0x01000000u);
    expect(c.ok(FX1_DDR3_FE_REGION_COMMIT, 1), "region image committed");
    std::vector<std::uint8_t> g;
    expect(m.read(0x90000000u, g, 16) == AxiResp::Okay && m.read(kBase, g, 16) == AxiResp::DecErr, "committed map in use");
    b.ctrl->set_core_reset(true);
    b.ctrl->set_core_reset(false);
    expect(c.rd(FX1_DDR3_FE_REGION_ACTIVE(0)) == 0 && !(c.rd(FX1_DDR3_FE_REGION_STATUS) & FX1_DDR3_FE_REGION_ST_VALID),
           "core offline cleared FE_REGION_ACTIVE and STATUS[16]");
    expect(program_and_commit(b, b.ctrl->config(), 2, 12, 0) && c.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE) &&
               wait_mission(b, sc_time(100, SC_MS)),
           "reinitialized");
    expect(m.read(kBase, g, 16) == AxiResp::Okay && m.read(0x90000000u, g, 16) == AxiResp::DecErr,
           "decode fell back to the bootstrap map (HAS T6-29)");
}

// CSR-8.6-01: a snapshot is an observation boundary; it does not pause traffic or counters.
void scenario_snapshot_no_pause(Bench& b) {
    expect(boot_memory(b), "boot");
    MemPort& m = *b.mem[0];
    // Refresh windows would land on one run and not the other: hold them off for the comparison.
    b.ctrl->fault_block_refresh(0, true);
    b.ctrl->fault_block_refresh(1, true);
    (void)long_read(m, 0x6000);  // open the rows
    const AxiOut ref = long_read(m, 0x6000);
    const std::uint32_t before = b.snap(FX1_DDR3_SERVICE_COUNT_BE);
    Join j;
    AxiOut r;
    j.spawn([&] { r = long_read(m, 0x6000); });
    sc_core::wait(5, SC_NS);
    const std::uint32_t during = b.snap(FX1_DDR3_SERVICE_COUNT_BE);
    j.wait();
    const std::uint32_t after = b.snap(FX1_DDR3_SERVICE_COUNT_BE);
    expect(r.end - r.start == ref.end - ref.start, "the in-flight read takes the same time with a snapshot");
    b.ctrl->fault_block_refresh(0, false);
    b.ctrl->fault_block_refresh(1, false);
    expect(during == before + 128 && after == during, "the capture observed the accepted atoms; nothing paused");
}

// CSR-8.5-05: clearing an interrupt does not clear the sticky cause or the result.
void scenario_irq_vs_sticky(Bench& b) {
    expect(boot_memory(b), "boot");
    CsrPort& c = b.csr;
    c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST);
    c.ok(FX1_DDR3_TOPO_SHADOW, 0);  // illegal geometry
    expect(c.wr(FX1_DDR3_TOPO_COMMIT, 1) == tlm::TLM_GENERIC_ERROR_RESPONSE, "rejected commit");
    expect((c.rd(FX1_DDR3_IRQ_RAW) & (FX1_DDR3_IRQ_MAPPING | FX1_DDR3_IRQ_CSR_CMD)) ==
               (FX1_DDR3_IRQ_MAPPING | FX1_DDR3_IRQ_CSR_CMD),
           "IRQ_RAW[4] and [10] raised");
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
    sc_core::wait(5, SC_NS);
    expect(c.rd(FX1_DDR3_IRQ_RAW) == 0 && (c.rd(FX1_DDR3_COMMAND_ERROR) & FX1_DDR3_CMD_ERR_COMMIT) &&
               (c.rd(FX1_DDR3_TOPO_COMMIT_STATUS) & FX1_DDR3_COMMIT_ST_ERROR),
           "IRQ cleared; COMMAND_ERROR and TOPO_COMMIT_STATUS keep the result");
    // ECC: clearing IRQ_RAW[0] keeps CE_COUNT.
    MemPort& m = *b.mem[0];
    c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_RELEASE);
    m.write(kBase + 0x40, pattern(32, 2));
    b.ctrl->fault_flip_data_bit(2, 3);
    std::vector<std::uint8_t> g;
    m.read(kBase + 0x40, g, 32);
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_CE_THRESHOLD);
    sc_core::wait(5, SC_NS);
    expect(!(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_CE_THRESHOLD) && b.snap(FX1_DDR3_CE_COUNT) == 1,
           "CE IRQ cleared; CE_COUNT kept");
    c.ok(FX1_DDR3_COMMAND_ERROR, FX1_DDR3_CMD_ERR_COMMIT);
    sc_core::wait(5, SC_NS);
    expect(!(c.rd(FX1_DDR3_COMMAND_ERROR) & FX1_DDR3_CMD_ERR_COMMIT), "the sticky word clears only by its own W1C");
}

// LC-9.4-01: admission_block = not mission_ready OR software stop OR fatal (INIT_STATUS[15]).
void scenario_admission_block(Bench& b) {
    CsrPort& c = b.csr;
    auto blocked = [&] { return (b.snap(FX1_DDR3_INIT_STATUS) & FX1_DDR3_INIT_ST_ADM_BLOCK) != 0; };
    expect(blocked(), "blocked before readiness");
    expect(boot_memory(b), "boot");
    expect(!blocked(), "open when mission ready");
    c.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE | FX1_DDR3_GLOBAL_CTRL_ADM_STOP);
    expect(blocked(), "software admission stop blocks");
    c.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE);
    expect(!blocked(), "stop released");
    b.ctrl->fault_fatal(Ddr3Controller::FatalFault::WriteResultUnmatched);
    expect(blocked(), "fatal blocks");
}

// LC-9.3-05: configuration and training context survive a low-power round trip; no retraining.
void scenario_wake_no_retrain(Bench& b) {
    expect(boot_memory(b), "boot");
    Ddr3Controller& d = *b.ctrl;
    CsrPort& c = b.csr;
    const std::vector<std::uint8_t> v = pattern(64, 9);
    b.mem[0]->write(kBase + 0x7000, v);
    const std::uint32_t topo = c.rd(FX1_DDR3_TOPO_ACTIVE), cl = c.rd(FX1_DDR3_TIMING_ACTIVE(kCl));
    const std::uint64_t loads = d.functional_phy()->loads_received();
    for (std::uint8_t s : {std::uint8_t{FX1_DDR3_PSTATE_PRECHARGE_POWER_DOWN}, std::uint8_t{FX1_DDR3_PSTATE_SELF_REFRESH}}) {
        expect(pchannel(b, s) && pchannel(b, FX1_DDR3_PSTATE_ACTIVE), "low-power round trip");
    }
    std::vector<std::uint8_t> got;
    expect(d.functional_phy()->loads_received() == loads && c.rd(FX1_DDR3_TOPO_ACTIVE) == topo &&
               c.rd(FX1_DDR3_TIMING_ACTIVE(kCl)) == cl && d.init_complete() && d.mission_ready() && d.scrub_complete(),
           "no retraining, images and readiness retained");
    expect(b.mem[0]->read(kBase + 0x7000, got, 64) == AxiResp::Okay && got == v, "data retained");
}

// ------------------------------------------------------------ seeded randomized soak
unsigned g_seed = 1;

void scenario_soak(Bench& b) {
    Ddr3Controller& d = *b.ctrl;
    const BuildConfig& cfg = d.config();
    std::cout << "soak seed=" << g_seed << " profile=" << to_string(cfg.profile) << " ecc=" << cfg.ecc_enable
              << " ports=" << cfg.n_ports << '\n';
    expect(boot_memory(b), "boot");
    std::mt19937 rng(g_seed);
    const unsigned ports = cfg.n_ports, bus = cfg.axi_beat_bytes();
    std::vector<std::map<std::uint32_t, std::uint8_t>> sb(ports);  // per-port window scoreboard
    std::uint64_t digest = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) { digest = (digest ^ v) * 1099511628211ull; };
    unsigned bad = 0, events = 0;
    std::set<std::uint32_t> flipped;
    for (unsigned iter = 0; iter < 120; ++iter) {
        Join j;
        const unsigned k = 1 + rng() % ports;
        for (unsigned t = 0; t < k; ++t) {
            const unsigned p = (iter + t) % ports;
            const bool write = rng() % 2;
            const unsigned size = rng() % (bus == 16 ? 5 : 4);
            const unsigned len = rng() % 16;
            const std::uint32_t lo = 0x100000u * (p + 1);
            std::uint32_t off = lo + static_cast<std::uint32_t>(rng() % 0x6000);
            off &= ~((1u << size) - 1u);
            if ((off & 0xFFFu) + (len + 1u) * (1u << size) > 4096u) off &= ~0xFFFu;
            std::vector<std::uint8_t> img((len + 1u) * bus), be(img.size());
            for (auto& x : img) x = static_cast<std::uint8_t>(rng());
            for (auto& e : be) e = rng() % 4 ? TLM_BYTE_ENABLED : TLM_BYTE_DISABLED;
            const unsigned id = rng() % 4;
            j.spawn([&, p, write, size, len, off, img, be, id] {
                AxiRequest r = make(kBase + off, AxiBurst::Incr, len, size, write, id);
                const AxiOut o = write ? b.mem[p]->axi(r, img, be) : b.mem[p]->axi(r);
                if (o.resp != AxiResp::Okay) ++bad;
                const std::uint64_t sz = std::uint64_t{1} << size;
                const std::uint64_t aligned = r.addr / sz * sz;
                for (unsigned n = 0; n <= len; ++n) {
                    const std::uint64_t a0 = n == 0 ? r.addr : aligned + n * sz;
                    for (std::uint64_t a = a0; a < a0 / sz * sz + sz; ++a) {
                        const std::size_t idx = n * bus + a % bus;
                        const auto key = static_cast<std::uint32_t>(a - kBase);
                        if (write) {
                            if (be[idx] == TLM_BYTE_ENABLED) sb[p][key] = img[idx];
                        } else {
                            const auto it = sb[p].find(key);
                            if (o.data[idx] != (it == sb[p].end() ? 0 : it->second)) ++bad;
                            mix(o.data[idx]);
                        }
                    }
                }
                mix(static_cast<std::uint64_t>(o.end.value()));
            });
        }
        // Lifecycle events while the traffic runs.
        const unsigned ev = rng() % 10;
        sc_core::wait(static_cast<double>(rng() % 20), SC_NS);
        if (ev == 0) {
            const unsigned p = rng() % ports;
            d.port_quiesce_request(p, true);
            b.at(sc_time(static_cast<double>(50 + rng() % 200), SC_NS), [&d, p] { d.port_quiesce_request(p, false); });
            ++events;
        } else if (ev == 1) {
            b.csr2.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST);
            b.at(sc_time(static_cast<double>(100 + rng() % 300), SC_NS),
                 [&b] { b.csr2.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_RELEASE); });
            ++events;
        } else if (ev == 2) {
            (void)b.snap(FX1_DDR3_SERVICE_COUNT_BE);
            ++events;
        }
        j.wait();
        if (ev == 3) {
            d.pchannel_request(true, rng() % 2 ? FX1_DDR3_PSTATE_SELF_REFRESH : FX1_DDR3_PSTATE_PRECHARGE_POWER_DOWN);
            b.wait_until([&] { return d.pchannel().paccept || d.pchannel().pdeny; }, sc_time(500, SC_US), sc_time(5, SC_NS));
            d.pchannel_request(false, 0);
            sc_core::wait(5, SC_NS);
            pchannel(b, FX1_DDR3_PSTATE_ACTIVE);
            ++events;
        } else if (ev == 4 && cfg.ecc_enable && !sb[0].empty()) {
            // A single-bit error on a written granule is corrected on every read. A CE is not
            // written back, so a granule is corrupted at most once (two flips in a lane = UE).
            const std::uint32_t key = std::next(sb[0].begin(), static_cast<long>(rng() % sb[0].size()))->first;
            const unsigned bit = rng() % 256;
            if (flipped.insert(key / 32).second) {
                d.fault_flip_data_bit(key / 32, bit);
                ++events;
            }
        }
        if (!d.mission_ready()) wait_mission(b, sc_time(10, SC_MS));
    }
    // Final read-back of every window through another port.
    for (unsigned p = 0; p < ports; ++p) {
        std::vector<std::uint8_t> got;
        const std::uint32_t lo = 0x100000u * (p + 1);
        b.mem[(p + 1) % ports]->read(kBase + lo, got, 0x7000);
        for (std::uint32_t i = 0; i < 0x7000; ++i) {
            const auto it = sb[p].find(lo + i);
            if (got[i] != (it == sb[p].end() ? 0 : it->second)) ++bad;
        }
    }
    mix(d.ce_count());
    expect(bad == 0, "soak: every response OKAY and every byte matches the scoreboard");
    expect(events > 0 && d.ue_count() == 0, "soak: lifecycle events exercised, no uncorrectable error");
    std::cout << "soak digest=" << std::hex << digest << std::dec << " events=" << events
              << " sim_time=" << sc_core::sc_time_stamp() << '\n';
}

// ------------------------------------------------------------ efficiency
std::size_t rss_kib() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) return static_cast<std::size_t>(std::strtoul(line.c_str() + 6, nullptr, 10));
    return 0;
}

// No per-cycle process while idle: refresh, readiness and timeouts are evaluated lazily.
void scenario_idle_events(Bench& b) {
    expect(boot_memory(b), "boot");
    const std::uint64_t e0 = b.ctrl->scheduler().executed();
    sc_core::wait(100, SC_MS);  // 100 ms = 40 M controller cycles, ~12 800 tREFI intervals
    const std::uint64_t e1 = b.ctrl->scheduler().executed();
    std::cout << "idle 100 ms: scheduled actions executed " << (e1 - e0) << '\n';
    expect(e1 - e0 < 16, "idle simulation runs (almost) no scheduled actions");
    expect(b.ctrl->refresh_debt(0) == 0 && b.ctrl->mission_ready(), "refresh debt stays zero, readiness kept");
}

// Wall time and RSS of the reference geometry (4 GiB physical, ECC): startup scrub of
// 134 M writes plus 8 MiB of traffic. Reported, with storage checked against touched data.
void scenario_perf(Bench& b) {
    const auto w0 = std::chrono::steady_clock::now();
    const std::size_t r0 = rss_kib();
    expect(boot_memory(b, 16, 2, FX1_DDR3_TOPO_MAP_LINEAR, 0x0, 0xF0000000u), "boot 2 x 4 Gb x8 with ECC");
    const auto w1 = std::chrono::steady_clock::now();
    MemPort& m = *b.mem[0];
    const std::vector<std::uint8_t> blk = pattern(4096, 4);
    for (std::uint32_t off = 0; off < 0x800000u; off += 4096u) m.write(0x10000000u + off, blk);
    const auto w2 = std::chrono::steady_clock::now();
    const std::size_t pages = b.ctrl->resident_pages();
    const std::size_t r1 = rss_kib();
    using ms = std::chrono::duration<double, std::milli>;
    std::cout << "perf: scrub writes=" << b.ctrl->scrub_total() << " boot_wall_ms=" << ms(w1 - w0).count()
              << " traffic_8MiB_wall_ms=" << ms(w2 - w1).count() << " sim_time=" << sc_core::sc_time_stamp()
              << " resident_pages=" << pages << " rss_kib_before=" << r0 << " rss_kib_after=" << r1 << '\n';
    // 8 MiB of data = 2048 pages, plus their companion records (1/8 of that).
    expect(pages >= 2048 && pages <= 2048 + 256 + 4, "storage proportional to the touched data and its records");
}

} // namespace

int sc_main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    if (argc > 2) g_seed = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 0));
    else if (const char* s = std::getenv("DDR3_SEED")) g_seed = static_cast<unsigned>(std::strtoul(s, nullptr, 0));
    BuildConfig cfg = BuildConfig::native_r2();
    cfg.n_ports = 2;
    if (which == "csr_during_commit") return run_bench(cfg, scenario_csr_during_commit, "verify csr_during_commit");
    if (which == "levels") return run_bench(cfg, scenario_levels, "verify levels");
    if (which == "clock_gating") return run_bench(cfg, scenario_clock_gating, "verify clock_gating");
    if (which == "soft_reset_backend") return run_bench(cfg, scenario_soft_reset_backend, "verify soft_reset_backend");
    if (which == "commit_not_idle") return run_bench(cfg, scenario_commit_not_idle, "verify commit_not_idle");
    if (which == "ack_blocker") return run_bench(cfg, scenario_ack_blocker, "verify ack_blocker");
    if (which == "core_offline_map") return run_bench(cfg, scenario_core_offline_map, "verify core_offline_map");
    if (which == "snapshot_no_pause") return run_bench(cfg, scenario_snapshot_no_pause, "verify snapshot_no_pause");
    if (which == "irq_vs_sticky") return run_bench(cfg, scenario_irq_vs_sticky, "verify irq_vs_sticky");
    if (which == "admission_block") return run_bench(cfg, scenario_admission_block, "verify admission_block");
    if (which == "wake_no_retrain") return run_bench(cfg, scenario_wake_no_retrain, "verify wake_no_retrain");
    if (which == "soak") return run_bench(cfg, scenario_soak, "verify soak");
    if (which == "soak_legacy") {
        BuildConfig c = BuildConfig::legacy();
        c.n_ports = 3;
        return run_bench(c, scenario_soak, "verify soak_legacy");
    }
    if (which == "soak_noecc") {
        cfg.ecc_enable = false;
        cfg.n_ports = 4;
        return run_bench(cfg, scenario_soak, "verify soak_noecc");
    }
    if (which == "idle_events") return run_bench(cfg, scenario_idle_events, "verify idle_events");
    if (which == "perf") {
        cfg.n_ports = 1;
        return run_bench(cfg, scenario_perf, "verify perf");
    }
    std::cerr << "unknown case '" << which << "'\n";
    return 2;
}
