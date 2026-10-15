// SPDX-License-Identifier: Apache-2.0
#pragma once
// Shared SystemC bench for the CSR and lifecycle regressions: one controller, two CSR
// initiators (mutex collisions need two masters), memory-port initiators, an actor thread
// for timed stimulus, and a firmware-like boot helper.
#include "ddr3_check.h"

#include "fx1_ddr3/ddr3_controller.h"
#include "fx1_ddr3/fx1_ddr3_regs.h"

#include <tlm_utils/simple_initiator_socket.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fx1_ddr3_test {

using fx1::ddr3::BuildConfig;
using fx1::ddr3::Ddr3Controller;
using sc_core::sc_time;
using sc_core::SC_NS;
using sc_core::SC_US;
using sc_core::SC_MS;

struct CsrPort : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<CsrPort> socket{"socket"};
    explicit CsrPort(sc_core::sc_module_name n) : sc_module(n) {}
    tlm::tlm_response_status access(tlm::tlm_command cmd, std::uint32_t addr, std::uint32_t& value,
                                    std::uint8_t strobes = 0xF) {
        unsigned char buf[4];
        unsigned char be[4];
        for (unsigned i = 0; i < 4; ++i) {
            buf[i] = static_cast<unsigned char>(value >> (8 * i));
            be[i] = ((strobes >> i) & 1u) ? TLM_BYTE_ENABLED : TLM_BYTE_DISABLED;
        }
        tlm::tlm_generic_payload tx;
        tx.set_command(cmd);
        tx.set_address(addr);
        tx.set_data_ptr(buf);
        tx.set_data_length(4);
        tx.set_streaming_width(4);
        if (cmd == tlm::TLM_WRITE_COMMAND && strobes != 0xF) {
            tx.set_byte_enable_ptr(be);
            tx.set_byte_enable_length(4);
        }
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_time d = sc_core::SC_ZERO_TIME;
        socket->b_transport(tx, d);
        if (cmd == tlm::TLM_READ_COMMAND)
            value = std::uint32_t{buf[0]} | std::uint32_t{buf[1]} << 8 | std::uint32_t{buf[2]} << 16 |
                    std::uint32_t{buf[3]} << 24;
        return tx.get_response_status();
    }
    std::uint32_t rd(std::uint32_t addr) {
        std::uint32_t v = 0;
        const auto s = access(tlm::TLM_READ_COMMAND, addr, v);
        if (s != tlm::TLM_OK_RESPONSE) expect(false, "CSR read OK @0x" + hex(addr));
        return v;
    }
    tlm::tlm_response_status wr(std::uint32_t addr, std::uint32_t value, std::uint8_t strobes = 0xF) {
        return access(tlm::TLM_WRITE_COMMAND, addr, value, strobes);
    }
    bool ok(std::uint32_t addr, std::uint32_t value) { return wr(addr, value) == tlm::TLM_OK_RESPONSE; }
    static std::string hex(std::uint32_t v) {
        static const char* d = "0123456789ABCDEF";
        std::string s;
        for (int i = 7; i >= 0; --i) s += d[(v >> (4 * i)) & 0xFu];
        return s;
    }
};

// Result of one AXI transaction on a memory port.
struct AxiOut {
    tlm::tlm_response_status status = tlm::TLM_INCOMPLETE_RESPONSE;
    fx1::ddr3::AxiResp resp = fx1::ddr3::AxiResp::Okay;
    std::vector<fx1::ddr3::AxiResp> beats;
    std::vector<std::uint8_t> data;  // lane image (reads)
    sc_time start, end;
};

struct MemPort : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<MemPort> socket{"socket"};
    unsigned bus = 16;  // AXI data bytes of the profile
    explicit MemPort(sc_core::sc_module_name n) : sc_module(n) {}
    // One AXI transaction: `image` is the write lane image ((len+1) x bus bytes), `strobes`
    // the per-byte WSTRB (empty = all enabled).
    AxiOut axi(const fx1::ddr3::AxiRequest& r, const std::vector<std::uint8_t>& image = {},
               const std::vector<std::uint8_t>& strobes = {}) {
        AxiOut o;
        const std::size_t len = std::size_t{r.beats()} * bus;
        o.data = image;
        o.data.resize(len, 0);
        std::vector<std::uint8_t> be = strobes;
        tlm::tlm_generic_payload tx;
        fx1::ddr3::AxiExtension ext;
        ext.req = r;
        tx.set_extension(&ext);
        tx.set_command(r.write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        tx.set_address(r.addr);
        tx.set_data_ptr(o.data.data());
        tx.set_data_length(static_cast<unsigned>(len));
        tx.set_streaming_width(static_cast<unsigned>(len));
        if (r.write && !be.empty()) {
            be.resize(len, TLM_BYTE_DISABLED);
            tx.set_byte_enable_ptr(be.data());
            tx.set_byte_enable_length(static_cast<unsigned>(len));
        }
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_time d = sc_core::SC_ZERO_TIME;
        o.start = sc_core::sc_time_stamp();
        socket->b_transport(tx, d);
        o.end = sc_core::sc_time_stamp();
        tx.clear_extension(&ext);
        o.status = tx.get_response_status();
        o.resp = ext.resp;
        o.beats = ext.beat_resp;
        return o;
    }
    // Byte helpers: full-width INCR bursts (plan_generic_access), worst response.
    fx1::ddr3::AxiResp write(std::uint32_t addr, const std::vector<std::uint8_t>& bytes, std::uint8_t id = 0) {
        fx1::ddr3::AxiResp worst = fx1::ddr3::AxiResp::Okay;
        for (const auto& pb : fx1::ddr3::plan_generic_access(addr, bytes.size(), bus, true)) {
            fx1::ddr3::AxiRequest r = pb.req;
            r.id = id;
            std::vector<std::uint8_t> img(std::size_t{r.beats()} * bus, 0), be(img.size(), TLM_BYTE_DISABLED);
            for (std::uint32_t k = 0; k < pb.byte_count; ++k) {
                const std::uint64_t a = r.addr + k;
                const std::size_t pos = static_cast<std::size_t>((a / bus - r.addr / bus) * bus + a % bus);
                img[pos] = bytes[pb.byte_offset + k];
                be[pos] = TLM_BYTE_ENABLED;
            }
            const AxiOut o = axi(r, img, be);
            if (static_cast<int>(o.resp) > static_cast<int>(worst)) worst = o.resp;
        }
        return worst;
    }
    fx1::ddr3::AxiResp read(std::uint32_t addr, std::vector<std::uint8_t>& bytes, std::size_t n, std::uint8_t id = 0) {
        bytes.assign(n, 0);
        fx1::ddr3::AxiResp worst = fx1::ddr3::AxiResp::Okay;
        for (const auto& pb : fx1::ddr3::plan_generic_access(addr, n, bus, false)) {
            fx1::ddr3::AxiRequest r = pb.req;
            r.id = id;
            const AxiOut o = axi(r);
            for (std::uint32_t k = 0; k < pb.byte_count; ++k) {
                const std::uint64_t a = r.addr + k;
                bytes[pb.byte_offset + k] = o.data[static_cast<std::size_t>((a / bus - r.addr / bus) * bus + a % bus)];
            }
            if (static_cast<int>(o.resp) > static_cast<int>(worst)) worst = o.resp;
        }
        return worst;
    }
    tlm::tlm_response_status read16(std::uint32_t addr) {
        unsigned char buf[16]{};
        tlm::tlm_generic_payload tx;
        fx1::ddr3::AxiExtension ext;
        ext.req.addr = addr;
        ext.req.size = 4;
        tx.set_extension(&ext);
        tx.set_command(tlm::TLM_READ_COMMAND);
        tx.set_address(addr);
        tx.set_data_ptr(buf);
        tx.set_data_length(16);
        tx.set_streaming_width(16);
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_time d = sc_core::SC_ZERO_TIME;
        socket->b_transport(tx, d);
        tx.clear_extension(&ext);
        return tx.get_response_status();
    }
};

// One controller plus initiators. `scenario` runs in an SC_THREAD; the actor runs timed
// stimulus independently of a blocked scenario thread.
struct Bench : sc_core::sc_module {
    std::unique_ptr<Ddr3Controller> ctrl;
    CsrPort csr{"csr"}, csr2{"csr2"};
    std::vector<std::unique_ptr<MemPort>> mem;
    sc_core::sc_signal<bool> irq{"irq"}, mission{"mission"}, initc{"initc"}, idle{"idle"}, drained{"drained"};
    std::function<void(Bench&)> scenario;
    std::vector<std::pair<sc_time, std::function<void()>>> script;
    sc_core::sc_event script_ev;
    // Two CSR masters share the AXI4-Lite port: one forwarding target socket per master.
    tlm_utils::simple_target_socket<Bench> csr_in1{"csr_in1"}, csr_in2{"csr_in2"};

    SC_HAS_PROCESS(Bench);
    // `bound_ports` memory initiators are bound (default: every enabled port); the rest
    // are left for the test to bind (for example a platform bridge).
    Bench(sc_core::sc_module_name n, const BuildConfig& cfg, int bound_ports = -1) : sc_module(n) {
        ctrl = std::make_unique<Ddr3Controller>("ddr", cfg);
        csr.socket.bind(csr_in1);
        csr2.socket.bind(csr_in2);
        csr_in1.register_b_transport(this, &Bench::forward);
        csr_in2.register_b_transport(this, &Bench::forward);
        const unsigned bound = bound_ports < 0 ? cfg.n_ports : static_cast<unsigned>(bound_ports);
        for (unsigned p = 0; p < bound; ++p) {
            mem.push_back(std::make_unique<MemPort>(("m" + std::to_string(p)).c_str()));
            mem.back()->bus = cfg.axi_beat_bytes();
            mem.back()->socket.bind(ctrl->mem_socket[p]);
        }
        mux_out.bind(ctrl->csr_socket);
        ctrl->irq.bind(irq);
        ctrl->mission_ready_out.bind(mission);
        ctrl->init_complete_out.bind(initc);
        ctrl->global_idle_out.bind(idle);
        ctrl->fe_drained_out.bind(drained);
        SC_THREAD(run);
        SC_THREAD(actor);
    }
    tlm_utils::simple_initiator_socket<Bench> mux_out{"mux_out"};
    void forward(tlm::tlm_generic_payload& tx, sc_time& d) { mux_out->b_transport(tx, d); }

    void at(const sc_time& delay, std::function<void()> fn) {
        script.emplace_back(sc_core::sc_time_stamp() + delay, std::move(fn));
        script_ev.notify(sc_core::SC_ZERO_TIME);
    }
    void actor() {
        for (;;) {
            if (script.empty()) {
                wait(script_ev);
                continue;
            }
            auto it = std::min_element(script.begin(), script.end(),
                                       [](const auto& a, const auto& b) { return a.first < b.first; });
            if (it->first > sc_core::sc_time_stamp()) {
                wait(it->first - sc_core::sc_time_stamp(), script_ev);
                continue;
            }
            auto fn = std::move(it->second);
            script.erase(it);
            fn();
        }
    }
    void run() {
        if (scenario) scenario(*this);
        completed() = true;
        sc_core::sc_stop();
    }
    // Waits until `pred` holds, polling every `step`, at most `limit`. Returns success.
    template <typename P>
    bool wait_until(P pred, sc_time limit, sc_time step = sc_time(100, SC_NS)) {
        const sc_time end = sc_core::sc_time_stamp() + limit;
        while (!pred()) {
            if (sc_core::sc_time_stamp() >= end) return false;
            wait(step);
        }
        return true;
    }
    std::uint32_t snap(std::uint32_t addr) {
        expect(csr.ok(FX1_DDR3_SNAPSHOT_CMD, 1), "SNAPSHOT_CMD accepted");
        return csr.rd(addr);
    }
};

// Firmware-style programming of a legal image for `cfg` (MAS speed-bin values) and the
// three commits. Returns false if any write or commit is rejected.
inline bool program_and_commit(Bench& b, const BuildConfig& cfg, unsigned ranks, unsigned row_bits,
                               unsigned page_code, bool short_waits = true, unsigned map = FX1_DDR3_TOPO_MAP_LINEAR) {
    using namespace fx1::ddr3;
    CsrPort& c = b.csr;
    bool ok = true;
    const TimingImage t = timing_image(cfg);
    for (unsigned i = 0; i < kTimingWords; ++i) ok &= c.ok(FX1_DDR3_TIMING_SHADOW(i), t[i]);
    const DfiImage dfi = dfi_image(cfg);
    ok &= c.ok(FX1_DDR3_DFI_WR_ENABLE_DELAY, dfi.wr_enable_delay);
    ok &= c.ok(FX1_DDR3_DFI_RD_ENABLE_DELAY, dfi.rd_enable_delay);
    ok &= c.ok(FX1_DDR3_PHY_MAX_READ_RETURN, dfi.max_read_return);
    ok &= c.ok(FX1_DDR3_DFI_ASSEMBLY_MARGIN, dfi.assembly_margin);
    const InitWaitImage w = init_wait_image(cfg);
    // INIT_* waits count controller cycles (HAS, SOQ-24). Short waits keep tests readable.
    ok &= c.ok(FX1_DDR3_INIT_RESET_RELEASE_WAIT, short_waits ? 100u : w.reset_release);
    ok &= c.ok(FX1_DDR3_INIT_CKE_LOW_WAIT, w.cke_low);
    ok &= c.ok(FX1_DDR3_INIT_CKE_HIGH_WAIT, w.cke_high);
    ok &= c.ok(FX1_DDR3_INIT_DLL_WAIT, w.dll);
    ok &= c.ok(FX1_DDR3_INIT_ZQ_WAIT, w.zq);
    ok &= c.ok(FX1_DDR3_PHY_RDLVL_DELAY, 0x112233);
    ok &= c.ok(FX1_DDR3_PHY_GATE_DELAY, 0x445566);
    ok &= c.ok(FX1_DDR3_PHY_WRLVL_DELAY, 3);
    ok &= c.ok(FX1_DDR3_TOPO_SHADOW, FX1_DDR3_TOPO_VAL(ranks, row_bits, page_code, map));
    ok &= c.ok(FX1_DDR3_TOPO_COMMIT, 1);
    ok &= c.ok(FX1_DDR3_TIMING_COMMIT, 1);
    ok &= c.ok(FX1_DDR3_DFI_TIMING_COMMIT, 1);
    return ok;
}

// Waits on lifecycle notifications (not polling) until mission readiness, at most `limit`.
inline bool wait_mission(Bench& b, sc_time limit) {
    const sc_time end = sc_core::sc_time_stamp() + limit;
    while (!b.ctrl->mission_ready()) {
        const sc_time now = sc_core::sc_time_stamp();
        if (now >= end) return false;
        sc_core::wait(end - now, b.ctrl->lifecycle_event());
    }
    return true;
}

// Memory-ready boot: bootstrap region [base, base + size) on every port, QoS configured,
// firmware programming and commits, GLOBAL_CTRL.enable, then mission readiness (startup
// scrub included). The default geometry is small (2^20 granules per rank) so the scrub
// lasts a few simulated milliseconds.
inline bool boot_memory(Bench& b, unsigned row_bits = 12, unsigned page_code = 0,
                        unsigned map = FX1_DDR3_TOPO_MAP_LINEAR, std::uint32_t base = 0x80000000u,
                        std::uint32_t size = 0x02000000u) {
    const BuildConfig& cfg = b.ctrl->config();
    for (unsigned p = 0; p < cfg.n_ports; ++p) {
        b.ctrl->set_bootstrap_region(p, 0, {base, size});
        b.ctrl->qos_set_configured(p, true);
    }
    bool ok = program_and_commit(b, cfg, cfg.max_ranks(), row_bits, page_code, true, map);
    ok &= b.csr.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE);
    return ok && wait_mission(b, sc_time(2000, sc_core::SC_MS));
}

inline int run_bench(const BuildConfig& cfg, std::function<void(Bench&)> scenario, const std::string& name) {
    sc_core::sc_report_handler::set_actions(sc_core::SC_WARNING, sc_core::SC_DO_NOTHING);
    Bench b("b", cfg);
    b.scenario = std::move(scenario);
    sc_core::sc_start();
    expect(completed(), "scenario ran to completion (no hung thread)");
    return summary(name);
}

} // namespace fx1_ddr3_test
