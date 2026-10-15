// DDR3 subtask 5: the FX1 DDR3 subsystem glue without CPUs.
//   sysctrl   DDR_SYSCTRL register map, access rules, readiness mirror, QoS sideband
//   power     P-channel handshakes through PWR_REQ, automatic wake on PACTIVE
//   reset     RST_CTRL cold and coordinated core resets, fatal hold recovery
//   preinit   scrub -> preload -> CPU release order, ECC-consistent preload, dump
//   firmware  firmware boot: no staging, nothing initialized, traffic held
//   parser    --ddr3-fault syntax
//   wake_recovery  an automatic wake denied for ever (PHY not ready -> recovery required)
//             is retried with a back-off: time advances, a cold reset is served (S5-R1)
//   wake_busy  an automatic wake denied while a CSR command owns the mutex: the same; a
//             core reset cannot reach reset-safe (owned work), a cold reset ends it
//   canonical storage identity for the CPU exclusive monitor (region aliases, boundaries,
//             undecoded addresses) and the storage-change listener (S5-R2/R3)
//   refimage  the firmware reference image (fw/fx1_soc/ddr3/ddr3_ref_1600.h) equals the
//             model's reference for the FX1 build, and the platform preinit installs it
// Usage: test_fx1_ddr3_subsystem <case>

#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>

#include <fx1/fx1_ddr_sysctrl.h>
#include <fx1/fx1_memory_map.h>
#include <fx1_ddr3/fx1_ddr3_regs.h>

#include <fx1_ddr3/address_map.h>
#include <fx1_ddr3/functional_phy.h>
#include <fx1_ddr3/profile.h>
#include <fx1_ddr3/speed_bin.h>

#include "ddr3_ref_1600.h"
#include "ddr3_subsystem.h"

using cdc::platforms::fx1_soc::Ddr3Fault;
using cdc::platforms::fx1_soc::Ddr3Subsystem;
using cdc::platforms::fx1_soc::DdrBoot;
using sc_core::sc_time;
using sc_core::SC_NS;
using sc_core::SC_US;
using sc_core::SC_MS;

namespace {

int failures = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) {
        ++failures;
        std::cout << "FAIL: " << what << " (t=" << sc_core::sc_time_stamp() << ")\n";
    }
}

struct Harness : sc_core::sc_module {
    Ddr3Subsystem ddr;
    tlm_utils::simple_initiator_socket<Harness> sysctrl{"sysctrl"}, csr{"csr"}, mem{"mem"};
    sc_core::sc_signal<bool> irq{"irq"};
    std::function<void(Harness&)> scenario;
    bool done = false;

    SC_HAS_PROCESS(Harness);
    Harness(sc_core::sc_module_name name, const Ddr3Subsystem::Options& options)
        : sc_module(name), ddr("ddr3_subsystem", options)
    {
        sysctrl.bind(ddr.sysctrl_target());
        csr.bind(ddr.csr_target());
        mem.bind(ddr.mem_target());
        ddr.irq()(irq);
        SC_THREAD(run);
    }
    void run()
    {
        if (!ddr.released()) wait(ddr.release_event());
        if (scenario) scenario(*this);
        done = true;
        sc_core::sc_stop();
    }

    static tlm::tlm_response_status access(tlm_utils::simple_initiator_socket<Harness>& s, tlm::tlm_command cmd,
                                           std::uint64_t address, unsigned char* data, unsigned length,
                                           unsigned char* be = nullptr)
    {
        tlm::tlm_generic_payload tx;
        tx.set_command(cmd);
        tx.set_address(address);
        tx.set_data_ptr(data);
        tx.set_data_length(length);
        tx.set_streaming_width(length);
        if (be) {
            tx.set_byte_enable_ptr(be);
            tx.set_byte_enable_length(length);
        }
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_time delay = sc_core::SC_ZERO_TIME;
        s->b_transport(tx, delay);
        return tx.get_response_status();
    }
    static tlm::tlm_response_status rd32(tlm_utils::simple_initiator_socket<Harness>& s, std::uint64_t a,
                                         std::uint32_t& v)
    {
        unsigned char b[4] = {};
        const auto st = access(s, tlm::TLM_READ_COMMAND, a, b, 4);
        v = std::uint32_t{b[0]} | std::uint32_t{b[1]} << 8 | std::uint32_t{b[2]} << 16 | std::uint32_t{b[3]} << 24;
        return st;
    }
    static tlm::tlm_response_status wr32(tlm_utils::simple_initiator_socket<Harness>& s, std::uint64_t a,
                                         std::uint32_t v)
    {
        unsigned char b[4];
        for (unsigned i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>(v >> (8 * i));
        return access(s, tlm::TLM_WRITE_COMMAND, a, b, 4);
    }
    std::uint32_t sc_rd(std::uint32_t off)
    {
        std::uint32_t v = 0;
        expect(rd32(sysctrl, off, v) == tlm::TLM_OK_RESPONSE, "DDR_SYSCTRL read OK @" + std::to_string(off));
        return v;
    }
    bool sc_ok(std::uint32_t off, std::uint32_t v) { return wr32(sysctrl, off, v) == tlm::TLM_OK_RESPONSE; }
    // Polls `pred` every microsecond for at most `limit`.
    template <typename P>
    bool wait_for(P pred, sc_time limit)
    {
        const sc_time end = sc_core::sc_time_stamp() + limit;
        while (!pred()) {
            if (sc_core::sc_time_stamp() >= end) return false;
            wait(sc_time(1, SC_US));
        }
        return true;
    }
    bool pwr_idle() { return !(sc_rd(FX1_DDRSC_PWR_STATUS) & FX1_DDRSC_PWR_BUSY); }
    bool rst_idle() { return !(sc_rd(FX1_DDRSC_RST_STATUS) & FX1_DDRSC_RST_BUSY); }
    std::uint32_t ddr_sysctrl_denials() { return ddr.sysctrl().wake_denials(); }
};

// A memory read issued from its own process; `finished` and `status` report it.
struct BackgroundRead {
    bool finished = false;
    tlm::tlm_response_status status = tlm::TLM_INCOMPLETE_RESPONSE;
    std::uint32_t value = 0;
    void start(Harness& h, std::uint64_t address)
    {
        sc_core::sc_spawn([this, &h, address] {
            status = Harness::rd32(h.mem, address, value);
            finished = true;
        });
    }
};

int run_case(const Ddr3Subsystem::Options& options, std::function<void(Harness&)> scenario,
             std::function<void(Harness&)> before = {})
{
    sc_core::sc_report_handler::set_actions(sc_core::SC_WARNING, sc_core::SC_DO_NOTHING);
    Harness h("h", options);
    h.scenario = std::move(scenario);
    if (before) before(h);
    // The preinit sequence pauses the simulation when it releases the CPUs.
    while (sc_core::sc_get_status() != sc_core::SC_STOPPED && !h.done) {
        sc_core::sc_start();
        if (!h.done && !h.ddr.startup_error().empty()) break;
    }
    expect(h.ddr.startup_error().empty(), "no preinit error: " + h.ddr.startup_error());
    expect(h.done, "scenario ran to completion");
    std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}

// ---- sysctrl -------------------------------------------------------------------------
void scenario_sysctrl(Harness& h)
{
    expect(h.sc_rd(FX1_DDRSC_ID) == FX1_DDRSC_ID_VALUE, "ID");
    const std::uint32_t st = h.sc_rd(FX1_DDRSC_STATUS);
    expect((st & FX1_DDRSC_ST_MISSION_READY) && (st & FX1_DDRSC_ST_INIT_COMPLETE) && (st & FX1_DDRSC_ST_PREINIT),
           "STATUS: mission ready, init complete, preinit");
    expect(!(st & FX1_DDRSC_ST_IRQ), "STATUS: no interrupt");
    expect((st & FX1_DDRSC_ST_GLOBAL_IDLE) && (st & FX1_DDRSC_ST_FE_DRAINED), "STATUS: idle and drained");

    // Access rules.
    std::uint32_t v = 0;
    expect(Harness::rd32(h.sysctrl, 0x100, v) == tlm::TLM_ADDRESS_ERROR_RESPONSE && v == 0,
           "unmapped read: address error, data 0");
    expect(Harness::wr32(h.sysctrl, 0x100, 1) == tlm::TLM_ADDRESS_ERROR_RESPONSE, "unmapped write: address error");
    expect(Harness::wr32(h.sysctrl, FX1_DDRSC_ID, 0) == tlm::TLM_GENERIC_ERROR_RESPONSE, "RO write: slave error");
    expect(Harness::wr32(h.sysctrl, FX1_DDRSC_STATUS, 0) == tlm::TLM_GENERIC_ERROR_RESPONSE, "RO STATUS write");
    unsigned char b2[2] = {};
    expect(Harness::access(h.sysctrl, tlm::TLM_READ_COMMAND, FX1_DDRSC_ID, b2, 2) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "16-bit read: slave error");
    unsigned char b4[4] = {}, be[4] = {0xFF, 0, 0xFF, 0xFF};
    expect(Harness::access(h.sysctrl, tlm::TLM_READ_COMMAND, FX1_DDRSC_ID + 2, b4, 4) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "misaligned read: slave error");
    expect(Harness::access(h.sysctrl, tlm::TLM_WRITE_COMMAND, FX1_DDRSC_QOS_CFG, b4, 4, be) ==
               tlm::TLM_GENERIC_ERROR_RESPONSE,
           "byte-enabled write: slave error");

    // Debug read: no side effect.
    tlm::tlm_generic_payload dbg;
    dbg.set_command(tlm::TLM_READ_COMMAND);
    dbg.set_address(FX1_DDRSC_ID);
    dbg.set_data_ptr(b4);
    dbg.set_data_length(4);
    expect(h.sysctrl->transport_dbg(dbg) == 4 && b4[0] == 0x43, "debug read of ID");

    // QoS sideband.
    auto& c = h.ddr.controller();
    expect(h.sc_rd(FX1_DDRSC_QOS_LUT(0)) == FX1_DDRSC_QOS_LUT_ALL_NORMAL, "QOS_LUT(0) reset: all NORMAL");
    expect(h.sc_rd(FX1_DDRSC_QOS_CFG) == 1u, "QOS_CFG: port 0 configured by the platform");
    expect(h.sc_ok(FX1_DDRSC_QOS_LUT(0), 0xE4E4E4E4u), "QOS_LUT write");
    expect(h.sc_rd(FX1_DDRSC_QOS_LUT(0)) == 0xE4E4E4E4u, "QOS_LUT read back");
    expect(c.qos_class(0, 0) == 0 && c.qos_class(0, 1) == 1 && c.qos_class(0, 2) == 2 && c.qos_class(0, 3) == 3,
           "QOS_LUT reaches the controller table");
    expect(h.sc_ok(FX1_DDRSC_QOS_LUT(0), FX1_DDRSC_QOS_LUT_ALL_NORMAL), "QOS_LUT restore");
    expect(Harness::wr32(h.sysctrl, FX1_DDRSC_QOS_CFG, 0x10) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "QOS_CFG reserved bit: slave error");

    // QOS_CFG drives FE05: an unconfigured port admits nothing.
    expect(h.sc_ok(FX1_DDRSC_QOS_CFG, 0), "QOS_CFG clear");
    expect(!c.qos_configured(0), "port 0 unconfigured");
    BackgroundRead r;
    r.start(h, 0x100);
    sc_core::wait(sc_time(20, SC_US));
    expect(!r.finished, "read stalls while the QoS table is unconfigured");
    expect(h.sc_ok(FX1_DDRSC_QOS_CFG, 1), "QOS_CFG set");
    expect(h.wait_for([&] { return r.finished; }, sc_time(10, SC_US)) && r.status == tlm::TLM_OK_RESPONSE,
           "read admitted once configured");

    // Reserved rate: stored, 20-bit range.
    expect(h.sc_ok(FX1_DDRSC_RSV_RATE(1), 0x12345) && h.sc_ok(FX1_DDRSC_RSV_DEPTH(1), 0xFFFFF), "RSV write");
    expect(h.sc_rd(FX1_DDRSC_RSV_RATE(1)) == 0x12345 && h.sc_rd(FX1_DDRSC_RSV_DEPTH(1)) == 0xFFFFF, "RSV read back");
    expect(Harness::wr32(h.sysctrl, FX1_DDRSC_RSV_RATE(1), 1u << 20) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "RSV out of range");

    // STATUS[4] mirrors the controller's interrupt line.
    expect(Harness::wr32(h.csr, FX1_DDR3_IRQ_MASK, 1u << 10) == tlm::TLM_OK_RESPONSE, "IRQ_MASK");
    expect(Harness::wr32(h.csr, FX1_DDR3_IRQ_TEST_SET, 1u << 10) == tlm::TLM_OK_RESPONSE, "IRQ_TEST_SET");
    sc_core::wait(sc_time(1, SC_US));
    expect((h.sc_rd(FX1_DDRSC_STATUS) & FX1_DDRSC_ST_IRQ) && h.irq.read(), "STATUS[4] and the IRQ line follow");
    expect(Harness::wr32(h.csr, FX1_DDR3_IRQ_CLEAR, 1u << 10) == tlm::TLM_OK_RESPONSE, "IRQ_CLEAR");
    sc_core::wait(sc_time(1, SC_US));
    expect(!(h.sc_rd(FX1_DDRSC_STATUS) & FX1_DDRSC_ST_IRQ) && !h.irq.read(), "interrupt cleared");
}

// ---- power ---------------------------------------------------------------------------
void scenario_power(Harness& h)
{
    std::uint32_t ps = h.sc_rd(FX1_DDRSC_PWR_STATUS);
    expect(FX1_DDRSC_PWR_RESIDENT(ps) == FX1_DDR3_PSTATE_ACTIVE, "resident ACTIVE after preinit");
    expect(h.sc_rd(FX1_DDRSC_PWR_CTRL) == FX1_DDRSC_PWR_AUTO_WAKE, "AUTO_WAKE reset 1");
    expect(Harness::wr32(h.sysctrl, FX1_DDRSC_PWR_REQ, 1u << 9) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "PWR_REQ reserved bit: slave error");

    // Self-refresh entry through the 4-phase handshake.
    expect(h.sc_ok(FX1_DDRSC_PWR_REQ, FX1_DDRSC_PWR_REQ_VAL(FX1_DDR3_PSTATE_SELF_REFRESH)), "PWR_REQ SR");
    expect(h.wait_for([&] { return h.pwr_idle(); }, sc_time(1, SC_MS)), "SR handshake ends");
    ps = h.sc_rd(FX1_DDRSC_PWR_STATUS);
    expect((ps & FX1_DDRSC_PWR_ACCEPTED) && !(ps & FX1_DDRSC_PWR_DENIED), "SR accepted");
    expect(FX1_DDRSC_PWR_LAST_REQ(ps) == FX1_DDR3_PSTATE_SELF_REFRESH, "last request SR");
    expect(FX1_DDRSC_PWR_RESIDENT(ps) == FX1_DDR3_PSTATE_SELF_REFRESH, "resident SR");
    expect(!(ps & FX1_DDRSC_PWR_PACTIVE), "PACTIVE low in SR without traffic");
    expect(FX1_DDRSC_PWR_ENTRIES(h.sc_rd(FX1_DDRSC_PWR_COUNT)) == 1, "one entry counted");
    expect(!h.ddr.controller().pchannel().preq, "PREQ dropped after the handshake");

    // Traffic in SR raises PACTIVE: DDR_SYSCTRL wakes the controller by itself.
    BackgroundRead r;
    r.start(h, 0x200);
    expect(h.wait_for([&] { return r.finished; }, sc_time(1, SC_MS)) && r.status == tlm::TLM_OK_RESPONSE,
           "read completes after the automatic wake");
    std::uint32_t cnt = h.sc_rd(FX1_DDRSC_PWR_COUNT);
    expect(FX1_DDRSC_PWR_AUTO_WAKES(cnt) == 1 && FX1_DDRSC_PWR_ENTRIES(cnt) == 1, "one automatic wake");
    expect(FX1_DDRSC_PWR_RESIDENT(h.sc_rd(FX1_DDRSC_PWR_STATUS)) == FX1_DDR3_PSTATE_ACTIVE, "ACTIVE again");

    // Without AUTO_WAKE the read waits until firmware requests ACTIVE.
    expect(h.sc_ok(FX1_DDRSC_PWR_CTRL, 0), "AUTO_WAKE off");
    expect(h.sc_ok(FX1_DDRSC_PWR_REQ, FX1_DDRSC_PWR_REQ_VAL(FX1_DDR3_PSTATE_PRECHARGE_POWER_DOWN)), "PWR_REQ PPD");
    expect(h.wait_for([&] { return h.pwr_idle(); }, sc_time(1, SC_MS)), "PPD handshake ends");
    expect(FX1_DDRSC_PWR_RESIDENT(h.sc_rd(FX1_DDRSC_PWR_STATUS)) == FX1_DDR3_PSTATE_PRECHARGE_POWER_DOWN,
           "resident PPD");
    BackgroundRead r2;
    r2.start(h, 0x300);
    sc_core::wait(sc_time(100, SC_US));
    ps = h.sc_rd(FX1_DDRSC_PWR_STATUS);
    expect(!r2.finished && (ps & FX1_DDRSC_PWR_PACTIVE), "read held in PPD, PACTIVE visible");
    expect(h.sc_ok(FX1_DDRSC_PWR_REQ, FX1_DDRSC_PWR_REQ_VAL(FX1_DDR3_PSTATE_ACTIVE)), "PWR_REQ ACTIVE");
    expect(h.wait_for([&] { return r2.finished; }, sc_time(1, SC_MS)) && r2.status == tlm::TLM_OK_RESPONSE,
           "read completes after the requested wake");
    cnt = h.sc_rd(FX1_DDRSC_PWR_COUNT);
    expect(FX1_DDRSC_PWR_ENTRIES(cnt) == 2 && FX1_DDRSC_PWR_AUTO_WAKES(cnt) == 1, "counts: 2 entries, 1 auto wake");
    expect(h.ddr.controller().power_resident() == FX1_DDR3_PSTATE_ACTIVE, "controller ACTIVE");
}

// ---- reset ---------------------------------------------------------------------------
void scenario_reset(Harness& h)
{
    auto& c = h.ddr.controller();
    expect(Harness::wr32(h.sysctrl, FX1_DDRSC_RST_CTRL, 3) == tlm::TLM_GENERIC_ERROR_RESPONSE, "two bits: error");
    expect(Harness::wr32(h.sysctrl, FX1_DDRSC_RST_CTRL, 0) == tlm::TLM_GENERIC_ERROR_RESPONSE, "no bit: error");
    expect(h.sc_rd(FX1_DDRSC_RST_CTRL) == 0, "RST_CTRL reads 0");

    // A coordinated core reset waits for reset-safe: a launched read completes first
    // (HAS §7.1). The read is issued in the same instant as the RST_CTRL write and is in
    // backend service when the reset request rises.
    BackgroundRead inflight;
    inflight.start(h, 0x500);
    expect(h.sc_ok(FX1_DDRSC_RST_CTRL, FX1_DDRSC_RST_CORE), "RST_CTRL CORE with a read in flight");
    expect(h.wait_for([&] { return h.rst_idle(); }, sc_time(2, SC_MS)), "core reset sequence ends");
    expect(inflight.finished && inflight.status == tlm::TLM_OK_RESPONSE, "the launched read completed with OKAY");
    expect(FX1_DDRSC_RST_CORE_COUNT(h.sc_rd(FX1_DDRSC_RST_STATUS)) == 1, "first core reset counted");
    expect(!c.mission_ready(), "core reset closes readiness (re-initialization needed)");
    // The CSR shadows survive a core reset: committing them again re-initializes.
    for (std::uint32_t commit : {FX1_DDR3_TOPO_COMMIT, FX1_DDR3_TIMING_COMMIT, FX1_DDR3_DFI_TIMING_COMMIT})
        expect(Harness::wr32(h.csr, commit, 1) == tlm::TLM_OK_RESPONSE, "recommit after the core reset");
    expect(h.wait_for([&] { return c.mission_ready(); }, sc_time(2000, SC_MS)), "ready again after recommit");

    // A fatal event holds new traffic; a coordinated core reset aborts it (SLVERR).
    c.fault_fatal(fx1::ddr3::Ddr3Controller::FatalFault::DfiProtocol);
    sc_core::wait(sc_time(1, SC_US));
    expect(!(h.sc_rd(FX1_DDRSC_STATUS) & FX1_DDRSC_ST_MISSION_READY), "fatal: mission readiness drops");
    BackgroundRead held;
    held.start(h, 0x400);
    sc_core::wait(sc_time(50, SC_US));
    expect(!held.finished, "fatal: the read is held, not answered");
    expect(h.sc_ok(FX1_DDRSC_RST_CTRL, FX1_DDRSC_RST_CORE), "RST_CTRL CORE");
    expect(Harness::wr32(h.sysctrl, FX1_DDRSC_RST_CTRL, FX1_DDRSC_RST_COLD) == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "RST_CTRL while busy: error");
    expect(h.wait_for([&] { return h.rst_idle(); }, sc_time(2, SC_MS)), "core reset sequence ends");
    std::uint32_t rs = h.sc_rd(FX1_DDRSC_RST_STATUS);
    expect((rs & FX1_DDRSC_RST_DONE) && !(rs & FX1_DDRSC_RST_SAFE_TIMEOUT) && FX1_DDRSC_RST_CORE_COUNT(rs) == 2,
           "core reset done (reset-safe reached with held work)");
    expect(held.finished && held.status == tlm::TLM_GENERIC_ERROR_RESPONSE, "held read aborted with SLVERR");
    expect(c.fatal_status() != 0, "FATAL_STATUS survives a core reset");

    // Cold reset clears the fatal record, the readiness and the QoS tables.
    expect(h.sc_ok(FX1_DDRSC_RST_CTRL, FX1_DDRSC_RST_COLD), "RST_CTRL COLD");
    expect(h.wait_for([&] { return h.rst_idle(); }, sc_time(1, SC_MS)), "cold reset sequence ends");
    rs = h.sc_rd(FX1_DDRSC_RST_STATUS);
    expect((rs & FX1_DDRSC_RST_DONE) && FX1_DDRSC_RST_COLD_COUNT(rs) == 1, "cold reset done, counted");
    expect(c.fatal_status() == 0 && !c.fatal(), "cold reset clears FATAL_STATUS");
    const std::uint32_t st = h.sc_rd(FX1_DDRSC_STATUS);
    expect(!(st & FX1_DDRSC_ST_MISSION_READY) && !(st & FX1_DDRSC_ST_INIT_COMPLETE), "not initialized after cold reset");
    expect(h.sc_rd(FX1_DDRSC_QOS_CFG) == 0, "cold reset clears the QoS configuration");
    std::uint32_t id = 0;
    expect(Harness::rd32(h.csr, FX1_DDR3_IP_ID, id) == tlm::TLM_OK_RESPONSE && id == 0x44445233u,
           "controller CSRs reachable after the cold reset");
}

// ---- preinit -------------------------------------------------------------------------
std::vector<std::uint8_t> pattern(std::size_t n, unsigned seed)
{
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i * 7 + seed * 13 + (i >> 8));
    return v;
}
const std::uint64_t kLow = FX1_DDR_BASE + 0x1003;  // unaligned start, partial granules
const std::uint64_t kHigh = std::uint64_t{FX1_DDR_BASE} + FX1_DDR_SIZE - 96;

void stage_images(Harness& h)
{
    const auto a = pattern(300, 1), b = pattern(96, 2);
    h.ddr.stage(kLow, a.data(), a.size(), "image A");
    h.ddr.stage(kHigh, b.data(), b.size(), "image B");
    bool threw = false;
    try {
        h.ddr.stage(kHigh + 8, b.data(), b.size(), "past the end");
    } catch (const std::out_of_range&) {
        threw = true;
    }
    expect(threw, "staging past the aperture end throws");
}

void scenario_preinit(Harness& h)
{
    auto& c = h.ddr.controller();
    // The release came after the full ECC scrub of 4 GiB (DEC-10: about 0.67 s).
    expect(h.ddr.release_time() > sc_time(600, SC_MS), "CPU release after the startup scrub");
    expect(c.scrub_complete() && c.mission_ready(), "scrub complete, mission ready at release");
    expect(h.ddr.release_time() == sc_core::sc_time_stamp(), "scenario starts at the release");

    // Preloaded images read back through the bus path with clean ECC.
    const auto a = pattern(300, 1), b = pattern(96, 2);
    std::vector<std::uint8_t> got(300);
    expect(Harness::access(h.mem, tlm::TLM_READ_COMMAND, kLow - FX1_DDR_BASE, got.data(), 300) ==
               tlm::TLM_OK_RESPONSE && got == a,
           "image A over the bus");
    got.assign(96, 0);
    expect(Harness::access(h.mem, tlm::TLM_READ_COMMAND, kHigh - FX1_DDR_BASE, got.data(), 96) ==
               tlm::TLM_OK_RESPONSE && got == b,
           "image B (aperture end) over the bus");
    expect(c.ce_count() == 0 && c.ue_count() == 0, "preload ECC consistent: no CE/UE");
    // Bytes around the images: scrubbed zero.
    std::uint32_t v = 1;
    expect(Harness::rd32(h.mem, 0x1000, v) == tlm::TLM_OK_RESPONSE && (v & 0x00FFFFFFu) == 0, "scrubbed bytes zero");
    // Host dump equals the image.
    got.assign(300, 0);
    h.ddr.dump(kLow, got.data(), got.size());
    expect(got == a, "dump of image A");
    bool threw = false;
    try {
        h.ddr.dump(FX1_DDR_BASE - 4, got.data(), 8);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    expect(threw, "dump outside the aperture throws");
    threw = false;
    try {
        h.ddr.stage(kLow, a.data(), 4, "late");
    } catch (const std::logic_error&) {
        threw = true;
    }
    expect(threw, "staging after the start throws");
    std::uint32_t init = 0;
    expect(Harness::rd32(h.csr, FX1_DDR3_GLOBAL_CTRL, init) == tlm::TLM_OK_RESPONSE &&
               (init & FX1_DDR3_GLOBAL_CTRL_ENABLE),
           "GLOBAL_CTRL.enable programmed by preinit");
}

// ---- firmware ------------------------------------------------------------------------
void scenario_firmware(Harness& h)
{
    expect(h.ddr.released() && h.ddr.release_time() == sc_core::SC_ZERO_TIME, "firmware boot: released at 0");
    const std::uint32_t st = h.sc_rd(FX1_DDRSC_STATUS);
    expect(!(st & FX1_DDRSC_ST_PREINIT) && !(st & FX1_DDRSC_ST_MISSION_READY), "STATUS: not preinit, not ready");
    std::uint32_t g = 1;
    expect(Harness::rd32(h.csr, FX1_DDR3_GLOBAL_CTRL, g) == tlm::TLM_OK_RESPONSE && g == 0, "GLOBAL_CTRL reset");
    BackgroundRead r;
    r.start(h, 0x0);
    sc_core::wait(sc_time(200, SC_US));
    expect(!r.finished, "DDR access stalls before firmware initialization");
    std::uint8_t b[4];
    bool threw = false;
    try {
        h.ddr.dump(FX1_DDR_BASE, b, 4);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "dump without a committed topology throws");
}

void stage_refused(Harness& h)
{
    const std::uint8_t b[4] = {1, 2, 3, 4};
    bool threw = false;
    try {
        h.ddr.stage(FX1_DDR_BASE, b, 4, "ELF segment");
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("--ddr-boot firmware") != std::string::npos;
    }
    expect(threw, "firmware boot refuses DDR staging with an explanation");
}

// ---- parser --------------------------------------------------------------------------
int parser()
{
    Ddr3Fault f;
    std::string why;
    expect(cdc::platforms::fx1_soc::parse_ddr3_fault("100:ce:0x80001000", f, why) && f.kind == "ce" &&
               f.address == 0x80001000u && f.at == sc_time(100, SC_US),
           "ce");
    expect(cdc::platforms::fx1_soc::parse_ddr3_fault("0:read-timeout:3", f, why) && f.count == 3, "read-timeout:3");
    expect(cdc::platforms::fx1_soc::parse_ddr3_fault("5:write-result", f, why) && f.count == 1, "write-result");
    expect(cdc::platforms::fx1_soc::parse_ddr3_fault("5:fatal:wdata", f, why), "fatal:wdata");
    expect(cdc::platforms::fx1_soc::parse_ddr3_fault("5:refresh-block:1", f, why) && f.count == 1, "refresh-block");
    expect(cdc::platforms::fx1_soc::parse_ddr3_fault("5:phy-fail", f, why), "phy-fail");
    const char* bad[] = {"ce:0x80000000", "x:ce:0x80000000", "1:ce:0x7ffffffc", "1:ce:0xa0000000", "1:ue",
                         "1:read-timeout:0", "1:fatal:nope", "1:refresh-block:2", "1:phy-fail:1", "1:bogus"};
    for (const char* s : bad) expect(!cdc::platforms::fx1_soc::parse_ddr3_fault(s, f, why) && !why.empty(), s);
    std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}

// ---- wake_recovery / wake_busy (review S5-R1) -------------------------------------------
// Self-refresh, then a read: PACTIVE rises and DDR_SYSCTRL requests ACTIVE.
void enter_sr_with_held_read(Harness& h, BackgroundRead& r)
{
    expect(h.sc_ok(FX1_DDRSC_PWR_REQ, FX1_DDRSC_PWR_REQ_VAL(FX1_DDR3_PSTATE_SELF_REFRESH)), "PWR_REQ SR");
    expect(h.wait_for([&] { return h.pwr_idle(); }, sc_time(1, SC_MS)), "SR entry ends");
    expect(h.ddr.controller().power_resident() == FX1_DDR3_PSTATE_SELF_REFRESH, "resident SR");
    r.start(h, 0x600);
}

// The automatic wake keeps being denied: it must be retried with a growing back-off (no
// zero-time loop), stay visible, and leave the sequencer free to run a reset.
void expect_backoff(Harness& h, const BackgroundRead& r, const char* what)
{
    const sc_time t0 = sc_core::sc_time_stamp();
    sc_core::wait(sc_time(5, SC_MS));  // returns only if the kernel advances
    expect(sc_core::sc_time_stamp() == t0 + sc_time(5, SC_MS), std::string(what) + ": simulated time advances");
    expect(!r.finished, std::string(what) + ": the read is still held");
    const std::uint32_t ps = h.sc_rd(FX1_DDRSC_PWR_STATUS);
    expect(ps & FX1_DDRSC_PWR_AUTO_WAKE_DENIED, std::string(what) + ": PWR_STATUS.AUTO_WAKE_DENIED");
    expect(ps & FX1_DDRSC_PWR_PACTIVE, std::string(what) + ": PACTIVE still high");
    const std::uint32_t n = h.ddr_sysctrl_denials();
    // Back-off 1 us doubling to 1 ms: about 15 attempts in 5 ms; a spin would be millions.
    expect(n >= 5 && n <= 25, std::string(what) + ": retried with a back-off (" + std::to_string(n) + " denials)");
    expect(FX1_DDRSC_PWR_AUTO_WAKES(h.sc_rd(FX1_DDRSC_PWR_COUNT)) == 0, std::string(what) + ": no accepted wake");
}

void scenario_wake_recovery(Harness& h)
{
    BackgroundRead r;
    enter_sr_with_held_read(h, r);
    h.ddr.controller().functional_phy()->force_ready(false);  // the wake cannot complete
    expect_backoff(h, r, "wake_recovery");
    expect(h.ddr.controller().recovery_required(), "the failed wake left recovery required");
    // Recovery: a cold reset is served at once and aborts the held read.
    h.ddr.controller().functional_phy()->force_ready(true);
    expect(h.sc_ok(FX1_DDRSC_RST_CTRL, FX1_DDRSC_RST_COLD), "RST_CTRL COLD accepted");
    expect(h.wait_for([&] { return h.rst_idle(); }, sc_time(10, SC_US)), "cold reset served within 10 us");
    expect(h.wait_for([&] { return r.finished; }, sc_time(10, SC_US)) && r.status == tlm::TLM_GENERIC_ERROR_RESPONSE,
           "held read aborted with SLVERR");
    expect(!(h.sc_rd(FX1_DDRSC_PWR_STATUS) & FX1_DDRSC_PWR_AUTO_WAKE_DENIED), "denial state cleared by the reset");
    expect(!h.ddr.controller().recovery_required(), "recovery cleared by the cold reset");
}

void scenario_wake_busy(Harness& h)
{
    BackgroundRead r;
    enter_sr_with_held_read(h, r);
    // A port quiesce owns the command mutex until port 0 drains, which needs the wake:
    // every automatic wake is denied as busy (a transient cause that never clears here).
    bool cmd_done = false;
    tlm::tlm_response_status cmd_status = tlm::TLM_INCOMPLETE_RESPONSE;
    sc_core::sc_spawn([&] {
        cmd_status = Harness::wr32(h.csr, FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0));
        cmd_done = true;
    });
    expect_backoff(h, r, "wake_busy");
    expect(!cmd_done, "the quiesce command still waits for the drain");
    // A coordinated core reset is served, but cannot complete: the held read is owned work
    // (not fatal-contained), so reset-safe never comes and the request is dropped after 1 ms.
    expect(h.sc_ok(FX1_DDRSC_RST_CTRL, FX1_DDRSC_RST_CORE), "RST_CTRL CORE accepted");
    expect(h.wait_for([&] { return h.rst_idle(); }, sc_time(2, SC_MS)), "core reset sequence ends");
    std::uint32_t rs = h.sc_rd(FX1_DDRSC_RST_STATUS);
    expect((rs & FX1_DDRSC_RST_SAFE_TIMEOUT) && FX1_DDRSC_RST_CORE_COUNT(rs) == 0,
           "core reset: no reset-safe while the read is owned (RST_STATUS.SAFE_TIMEOUT)");
    expect(!r.finished && !cmd_done, "read and command still waiting");
    // A cold reset ends the command and the held read.
    expect(h.sc_ok(FX1_DDRSC_RST_CTRL, FX1_DDRSC_RST_COLD), "RST_CTRL COLD accepted");
    expect(h.wait_for([&] { return h.rst_idle(); }, sc_time(10, SC_US)), "cold reset served");
    rs = h.sc_rd(FX1_DDRSC_RST_STATUS);
    expect((rs & FX1_DDRSC_RST_DONE) && FX1_DDRSC_RST_COLD_COUNT(rs) == 1, "cold reset done");
    expect(h.wait_for([&] { return r.finished && cmd_done; }, sc_time(100, SC_US)), "read and command ended");
    expect(r.status == tlm::TLM_GENERIC_ERROR_RESPONSE, "held read aborted with SLVERR");
    expect(cmd_status == tlm::TLM_GENERIC_ERROR_RESPONSE, "the quiesce command ended with SLVERR");
}

// ---- canonical (review S5-R2/R3) ------------------------------------------------------
using RangeList = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

RangeList canon(Harness& h, std::uint64_t lo, std::uint64_t hi)
{
    RangeList out;
    h.ddr.canonical_ranges(lo, hi, out);
    return out;
}

bool commit_map(Harness& h, std::uint32_t size0, std::uint32_t base1, std::uint32_t size1)
{
    return Harness::wr32(h.csr, FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 0)), FX1_DDR_BASE) ==
               tlm::TLM_OK_RESPONSE &&
           Harness::wr32(h.csr, FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 1)), size0) ==
               tlm::TLM_OK_RESPONSE &&
           Harness::wr32(h.csr, FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 1, 0)), base1) ==
               tlm::TLM_OK_RESPONSE &&
           Harness::wr32(h.csr, FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 1, 1)), size1) ==
               tlm::TLM_OK_RESPONSE &&
           Harness::wr32(h.csr, FX1_DDR3_FE_REGION_COMMIT, 1) == tlm::TLM_OK_RESPONSE;
}

void scenario_canonical(Harness& h)
{
    const std::uint64_t K = Ddr3Subsystem::kCanonicalBase;
    unsigned changes = 0;
    h.ddr.controller().set_storage_listener([&] { ++changes; });
    // Outside the DDR aperture: the bus address.
    expect(canon(h, 0x10000000, 0x10000004) == RangeList{{0x10000000, 0x10000004}}, "non-DDR address is its own name");
    // Bootstrap map: user offset = address - 0x8000_0000.
    expect(canon(h, 0x80000010, 0x80000018) == RangeList{{K + 0x10, K + 0x18}}, "bootstrap map");
    // Two regions: region 1 aliases the bottom of region 0 (L-13).
    expect(commit_map(h, 0x18000000u, 0x98000000u, 0x08000000u), "alias map committed");
    expect(changes == 1, "a region commit calls the storage listener once");
    expect(canon(h, 0x84000000, 0x84000004) == canon(h, 0x9C000000, 0x9C000004) &&
               canon(h, 0x9C000000, 0x9C000004) == RangeList{{K + 0x04000000, K + 0x04000004}},
           "both names of one location get one canonical name");
    expect(canon(h, 0x97FFFFF8, 0x98000008) == RangeList{{K + 0x17FFFFF8, K + 0x18000000}, {K, K + 8}},
           "a range across the region boundary is split at the boundary");
    // A smaller region 0 leaves part of the aperture undecoded: those bytes keep their name.
    expect(commit_map(h, 0x10000000u, 0x98000000u, 0x08000000u), "map with a hole committed");
    expect(canon(h, 0x8FFFFFFC, 0x90000004) == RangeList{{K + 0x0FFFFFFC, K + 0x10000000}, {0x90000000, 0x90000004}},
           "a range into an undecoded hole: the decoded part canonical, the hole its own name");
    expect(canon(h, 0x97FFFFFC, 0x98000004) == RangeList{{0x97FFFFFC, 0x98000000}, {K, K + 4}},
           "a range from a hole into region 1 is split at the region start");
    // Every reset or reinitialization calls the listener (the monitor drops DDR reservations).
    const unsigned before = changes;
    expect(h.sc_ok(FX1_DDRSC_RST_CTRL, FX1_DDRSC_RST_COLD), "RST_CTRL COLD");
    expect(h.wait_for([&] { return h.rst_idle(); }, sc_time(10, SC_US)), "cold reset done");
    expect(changes > before, "a cold reset calls the storage listener");
    expect(canon(h, 0x9C000000, 0x9C000004) == RangeList{{K + 0x1C000000, K + 0x1C000004}},
           "after the reset the bootstrap map names the address again");
}

// ---- refimage ------------------------------------------------------------------------
void scenario_refimage(Harness& h)
{
    // What the preinit sequence installed (TIMING_ACTIVE, TOPO_ACTIVE) and what the model
    // derives for the build must both equal the firmware reference image.
    const auto& cfg = h.ddr.controller().config();
    const fx1::ddr3::TimingImage t = fx1::ddr3::timing_image(cfg);
    for (unsigned i = 0; i < FX1_DDR3_TIMING_WORDS; ++i) {
        expect(t[i] == ddr3_ref_timing[i], "timing word " + std::to_string(i) + " = model reference");
        std::uint32_t v = 0;
        expect(Harness::rd32(h.csr, FX1_DDR3_TIMING_ACTIVE(i), v) == tlm::TLM_OK_RESPONSE && v == ddr3_ref_timing[i],
               "TIMING_ACTIVE " + std::to_string(i) + " after preinit");
    }
    const fx1::ddr3::DfiImage d = fx1::ddr3::dfi_image(cfg);
    expect(d.wr_enable_delay == DDR3_REF_DFI_WR_ENABLE_DELAY && d.rd_enable_delay == DDR3_REF_DFI_RD_ENABLE_DELAY &&
               d.max_read_return == DDR3_REF_PHY_MAX_READ_RETURN && d.assembly_margin == DDR3_REF_DFI_ASSEMBLY_MARGIN,
           "DFI image = model reference");
    // INIT waits: the model lists tCK (MAS); the CSR counts controller cycles of R tCK.
    const fx1::ddr3::InitWaitImage w = fx1::ddr3::init_wait_image(cfg);
    const unsigned r = cfg.dfi_ratio();
    auto cycles = [r](std::uint32_t tck) { return (tck + r - 1) / r; };
    expect(cycles(w.reset_release) == DDR3_REF_INIT_RESET_RELEASE && cycles(w.cke_low) == DDR3_REF_INIT_CKE_LOW &&
               cycles(w.cke_high) == DDR3_REF_INIT_CKE_HIGH && cycles(w.dll) == DDR3_REF_INIT_DLL &&
               cycles(w.zq) == DDR3_REF_INIT_ZQ,
           "INIT waits = ceil(model tCK / R)");
    std::uint32_t topo = 0;
    expect(Harness::rd32(h.csr, FX1_DDR3_TOPO_ACTIVE, topo) == tlm::TLM_OK_RESPONSE && topo == DDR3_REF_TOPO,
           "TOPO_ACTIVE after preinit = reference topology");
    fx1::ddr3::DeviceGeometry g{};
    expect(fx1::ddr3::device_geometry(cfg.device_width, cfg.density, g) && g.row_bits == DDR3_REF_ROW_BITS &&
               g.page_words == 1024 && cfg.max_ranks() == DDR3_REF_RANKS,
           "reference topology = device organisation of the build");
}

} // namespace

int sc_main(int argc, char* argv[])
{
    const std::string which = argc > 1 ? argv[1] : "";
    Ddr3Subsystem::Options preinit;
    Ddr3Subsystem::Options firmware;
    firmware.boot = DdrBoot::firmware;
    try {
        if (which == "sysctrl") return run_case(preinit, scenario_sysctrl);
        if (which == "power") return run_case(preinit, scenario_power);
        if (which == "reset") return run_case(preinit, scenario_reset);
        if (which == "preinit") return run_case(preinit, scenario_preinit, stage_images);
        if (which == "firmware") return run_case(firmware, scenario_firmware, stage_refused);
        if (which == "parser") return parser();
        if (which == "refimage") return run_case(preinit, scenario_refimage);
        if (which == "wake_recovery") return run_case(preinit, scenario_wake_recovery);
        if (which == "wake_busy") return run_case(preinit, scenario_wake_busy);
        if (which == "canonical") return run_case(preinit, scenario_canonical);
    } catch (const std::exception& e) {
        std::cout << "FAIL: exception " << e.what() << '\n';
        return 1;
    }
    std::cerr << "usage: test_fx1_ddr3_subsystem sysctrl|power|reset|preinit|firmware|parser|refimage|wake_recovery|wake_busy|canonical\n";
    return 2;
}
