// SPDX-License-Identifier: Apache-2.0
// SystemC/TLM skeleton regressions: binding of 1-4 ports in every profile, identity
// CSR words, IP-boundary contract errors, functional PHY handshake/faults and the
// epoch-safe scheduler. One case per process (SystemC elaborates once).
#include "ddr3_check.h"

#include "fx1_ddr3/ddr3_controller.h"

#include <tlm_utils/simple_initiator_socket.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace fx1::ddr3;
using fx1_ddr3_test::expect;
using fx1_ddr3_test::expect_eq;
using sc_core::sc_time;
using sc_core::SC_NS;

namespace {

struct Initiator : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<Initiator> socket;
    explicit Initiator(sc_core::sc_module_name n) : sc_module(n), socket("socket") {}

    tlm::tlm_response_status csr(tlm::tlm_command cmd, std::uint64_t addr, std::uint32_t& value,
                                 unsigned length = 4) {
        unsigned char buf[8]{};
        if (cmd == tlm::TLM_WRITE_COMMAND)
            for (unsigned i = 0; i < 4; ++i) buf[i] = static_cast<unsigned char>(value >> (8 * i));
        tlm::tlm_generic_payload tx;
        tx.set_command(cmd);
        tx.set_address(addr);
        tx.set_data_ptr(buf);
        tx.set_data_length(length);
        tx.set_streaming_width(length);
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_time d = sc_core::SC_ZERO_TIME;
        socket->b_transport(tx, d);
        if (cmd == tlm::TLM_READ_COMMAND)
            value = std::uint32_t{buf[0]} | std::uint32_t{buf[1]} << 8 | std::uint32_t{buf[2]} << 16 |
                    std::uint32_t{buf[3]} << 24;
        return tx.get_response_status();
    }
    std::uint32_t rd(std::uint64_t addr) {
        std::uint32_t v = 0xDEADBEEF;
        expect(csr(tlm::TLM_READ_COMMAND, addr, v) == tlm::TLM_OK_RESPONSE, "CSR read OK @" + std::to_string(addr));
        return v;
    }
};

// ------------------------------------------------------------ bind_profiles / identity
struct Dut {
    std::unique_ptr<Ddr3Controller> ctrl;
    std::unique_ptr<Initiator> csr;
    std::vector<std::unique_ptr<Initiator>> ports;
    std::unique_ptr<sc_core::sc_signal<bool>> irq;
    Dut(const std::string& name, const BuildConfig& cfg) {
        ctrl = std::make_unique<Ddr3Controller>(name.c_str(), cfg);
        csr = std::make_unique<Initiator>((name + "_csr").c_str());
        csr->socket.bind(ctrl->csr_socket);
        for (unsigned p = 0; p < cfg.n_ports; ++p) {
            ports.push_back(std::make_unique<Initiator>((name + "_m" + std::to_string(p)).c_str()));
            ports.back()->socket.bind(ctrl->mem_socket[p]);
        }
        irq = std::make_unique<sc_core::sc_signal<bool>>((name + "_irq").c_str());
        ctrl->irq.bind(*irq);
    }
};

struct IdentityCheck : sc_core::sc_module {
    std::vector<Dut*> duts;
    SC_HAS_PROCESS(IdentityCheck);
    explicit IdentityCheck(sc_core::sc_module_name n) : sc_module(n) { SC_THREAD(run); }
    void run() {
        for (Dut* d : duts) {
            const BuildConfig& c = d->ctrl->config();
            const std::string tag = std::string(to_string(c.profile)) + "/" + std::to_string(c.n_ports) + "p";
            expect_eq(d->csr->rd(0x0000), 0x44445233u, tag + " IP_ID");
            expect_eq(d->csr->rd(0x0004), 0x00010000u, tag + " ABI_VERSION");
            expect_eq(d->csr->rd(0x0008), cap_feature(c), tag + " CAP_FEATURE");
            expect_eq(d->csr->rd(0x000C), 0x04101008u, tag + " CAP_DEPTH0");
            expect_eq(d->csr->rd(0x0010), c.dfi_ratio() == 2 ? 0x00208410u : 0x00204410u, tag + " CAP_WIDTH0");
            expect(!d->ctrl->irq_level(), tag + " IRQ low after reset");
            expect(!d->ctrl->mission_ready(), tag + " mission-ready low after reset");
            expect(d->ctrl->timebase().ratio == c.dfi_ratio(), tag + " timebase ratio follows profile");
        }
        Initiator& i = *duts.front()->csr;
        std::uint32_t v = 0x12345678;
        expect(i.csr(tlm::TLM_WRITE_COMMAND, 0x0000, v) == tlm::TLM_GENERIC_ERROR_RESPONSE, "RO IP_ID write -> SLVERR");
        expect_eq(i.rd(0x0000), 0x44445233u, "IP_ID unchanged after rejected write");
        expect(i.csr(tlm::TLM_READ_COMMAND, 0x0002, v) == tlm::TLM_GENERIC_ERROR_RESPONSE,
               "misaligned CSR access -> SLVERR (HAS §8.2 example 0x0102)");
        expect(i.csr(tlm::TLM_READ_COMMAND, 0x0000, v, 2) == tlm::TLM_GENERIC_ERROR_RESPONSE,
               "non-word CSR access -> SLVERR");
        v = 0;
        expect(i.csr(tlm::TLM_READ_COMMAND, 0x0038, v) == tlm::TLM_ADDRESS_ERROR_RESPONSE,
               "unmapped CSR word -> DECERR");
        fx1_ddr3_test::completed() = true;
        sc_core::sc_stop();
    }
};

void case_bind_profiles() {
    std::vector<std::unique_ptr<Dut>> duts;
    unsigned idx = 0;
    for (BuildConfig c : {BuildConfig::native_r2(), BuildConfig::native_r1(), BuildConfig::legacy()}) {
        for (unsigned p = 1; p <= 4; ++p) {
            c.n_ports = p;
            duts.push_back(std::make_unique<Dut>("ddr" + std::to_string(idx++), c));
        }
    }
    IdentityCheck chk("chk");
    for (auto& d : duts) chk.duts.push_back(d.get());
    sc_core::sc_start();
    expect(duts.size() == 12, "12 profile/port combinations elaborated");
}

void case_identity() {
    Dut d("ddr", BuildConfig::native_r2());
    IdentityCheck chk("chk");
    chk.duts.push_back(&d);
    sc_core::sc_start();
}

// ------------------------------------------------------------ bad_port_binding
void case_bad_port_binding() {
    // Before elaboration: an illegal build configuration is rejected at construction.
    bool bad_cfg = false;
    try {
        BuildConfig b;
        b.n_ports = 0;
        Ddr3Controller x("bad", b);
    } catch (const std::invalid_argument&) {
        bad_cfg = true;
    }
    expect(bad_cfg, "illegal build configuration is rejected at construction");

    BuildConfig c;
    c.n_ports = 1;
    Ddr3Controller ctrl("ddr", c);
    Initiator m0("m0"), m1("m1");
    m0.socket.bind(ctrl.mem_socket[0]);
    m1.socket.bind(ctrl.mem_socket[1]);
    sc_core::sc_signal<bool> irq("irq");
    ctrl.irq.bind(irq);
    Initiator csr("csr");
    csr.socket.bind(ctrl.csr_socket);
    sc_core::sc_report_handler::set_actions(sc_core::SC_ERROR, sc_core::SC_THROW);
    bool threw = false;
    try {
        sc_core::sc_start(sc_time(1, SC_NS));
    } catch (const sc_core::sc_report& r) {
        threw = std::strstr(r.what(), "enables only 1 port") != nullptr;
    }
    expect(threw, "binding a disabled port position is an elaboration error (FE01)");

}

// ------------------------------------------------------------ no_extension
struct NoExtCheck : sc_core::sc_module {
    Initiator* m = nullptr;
    SC_HAS_PROCESS(NoExtCheck);
    explicit NoExtCheck(sc_core::sc_module_name n) : sc_module(n) { SC_THREAD(run); }
    void run() {
        unsigned char buf[16]{};
        tlm::tlm_generic_payload tx;
        tx.set_command(tlm::TLM_READ_COMMAND);
        tx.set_address(0x80000000);
        tx.set_data_ptr(buf);
        tx.set_data_length(16);
        tx.set_streaming_width(16);
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_time d = sc_core::SC_ZERO_TIME;
        m->socket->b_transport(tx, d);
        expect(tx.get_response_status() == tlm::TLM_COMMAND_ERROR_RESPONSE,
               "memory payload without AXI extension -> command error");

        AxiExtension ext;
        ext.req.addr = 0x80000000;
        ext.req.size = 4;
        tx.set_extension(&ext);
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        m->socket->b_transport(tx, d);
        expect(tx.get_response_status() != tlm::TLM_OK_RESPONSE && ext.resp != AxiResp::Okay,
               "skeleton data path never fabricates OKAY");
        tx.clear_extension(&ext);
        fx1_ddr3_test::completed() = true;
        sc_core::sc_stop();
    }
};

void case_no_extension() {
    Dut d("ddr", BuildConfig::native_r2());
    d.ctrl->qos_set_configured(0, true);  // FE05 gate
    NoExtCheck chk("chk");
    chk.m = d.ports.front().get();
    sc_core::sc_start();
}

// ------------------------------------------------------------ csr_robustness
struct RawCsr : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<RawCsr> socket{"socket"};
    SC_HAS_PROCESS(RawCsr);
    explicit RawCsr(sc_core::sc_module_name n) : sc_module(n) { SC_THREAD(run); }
    tlm::tlm_response_status send(tlm::tlm_command cmd, std::uint64_t addr, unsigned char* data,
                                  unsigned length, unsigned char* be = nullptr, unsigned be_len = 0) {
        tlm::tlm_generic_payload tx;
        tx.set_command(cmd);
        tx.set_address(addr);
        tx.set_data_ptr(data);
        tx.set_data_length(length);
        tx.set_streaming_width(length);
        tx.set_byte_enable_ptr(be);
        tx.set_byte_enable_length(be_len);
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_time d = sc_core::SC_ZERO_TIME;
        socket->b_transport(tx, d);
        return tx.get_response_status();
    }
    void run() {
        // Review finding 2: a rejected write must not modify the initiator's buffer.
        unsigned char in[4] = {0x78, 0x56, 0x34, 0x12};
        expect(send(tlm::TLM_WRITE_COMMAND, 0x0038, in, 4) == tlm::TLM_ADDRESS_ERROR_RESPONSE &&
                   in[0] == 0x78 && in[1] == 0x56 && in[2] == 0x34 && in[3] == 0x12,
               "rejected (unmapped) write keeps the write data");
        expect(send(tlm::TLM_WRITE_COMMAND, 0x0034, in, 4) == tlm::TLM_OK_RESPONSE && in[0] == 0x78,
               "accepted SCRATCH write keeps the write data");
        expect(send(tlm::TLM_WRITE_COMMAND, 0x0000, in, 4) == tlm::TLM_GENERIC_ERROR_RESPONSE && in[0] == 0x78,
               "RO write keeps the write data");
        expect(send(tlm::TLM_WRITE_COMMAND, 0x0002, in, 4) == tlm::TLM_GENERIC_ERROR_RESPONSE && in[0] == 0x78,
               "misaligned write keeps the write data");
        unsigned char rd[4] = {1, 2, 3, 4};
        expect(send(tlm::TLM_READ_COMMAND, 0x0002, rd, 4) == tlm::TLM_GENERIC_ERROR_RESPONSE &&
                   rd[0] == 0 && rd[3] == 0,
               "misaligned read returns zero data");
        expect(send(tlm::TLM_READ_COMMAND, 0x0000, nullptr, 4) == tlm::TLM_GENERIC_ERROR_RESPONSE,
               "null data pointer read is rejected without dereferencing");
        expect(send(tlm::TLM_WRITE_COMMAND, 0x0034, nullptr, 4) == tlm::TLM_GENERIC_ERROR_RESPONSE,
               "null data pointer write is rejected without dereferencing");
        unsigned char ig[4] = {9, 9, 9, 9};
        expect(send(tlm::TLM_IGNORE_COMMAND, 0x0000, ig, 4) == tlm::TLM_COMMAND_ERROR_RESPONSE && ig[0] == 9,
               "unsupported command -> command error, buffer untouched");
        unsigned char be[2] = {0xFF, 0xFF};
        expect(send(tlm::TLM_WRITE_COMMAND, 0x0034, in, 4, be, 2) == tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE &&
                   in[0] == 0x78,
               "byte-enable length other than 4 -> byte-enable error");
        unsigned char id[4] = {};
        expect(send(tlm::TLM_READ_COMMAND, 0x0000, id, 4) == tlm::TLM_OK_RESPONSE && id[0] == 0x33 && id[3] == 0x44,
               "IP_ID still readable after the malformed accesses");
        fx1_ddr3_test::completed() = true;
        sc_core::sc_stop();
    }
};

void case_csr_robustness() {
    Ddr3Controller ctrl("ddr", BuildConfig::native_r2());
    sc_core::sc_signal<bool> irq("irq");
    ctrl.irq.bind(irq);
    RawCsr raw("raw");
    raw.socket.bind(ctrl.csr_socket);
    sc_core::sc_start();
}

// ------------------------------------------------------------ reset_contract
// Handshakes are checked without traffic. Traffic scenarios release stop conditions from a
// separate actor thread and check that a blocked call proceeds only once the last
// applicable stop condition is gone (HAS §7.1, §6.1.14; FE-6.1.14-01/05, LC-7.1-02).
struct ResetBench : sc_core::sc_module {
    Dut* d = nullptr;
    unsigned events = 0;
    bool armed = false;
    // Actor: runs scripted actions at absolute times, independent of the blocked caller.
    std::vector<std::pair<sc_time, std::function<void()>>> script;
    sc_core::sc_event script_ev;
    SC_HAS_PROCESS(ResetBench);
    explicit ResetBench(sc_core::sc_module_name n) : sc_module(n) {
        SC_THREAD(run);
        SC_THREAD(actor);
        SC_METHOD(count);
    }
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
            const sc_time when = it->first;
            if (when > sc_core::sc_time_stamp()) {
                wait(when - sc_core::sc_time_stamp(), script_ev);  // new entries may be earlier
                continue;
            }
            auto fn = std::move(it->second);
            script.erase(it);
            fn();
        }
    }
    void count() {  // dynamic sensitivity: the controller is attached after construction
        if (armed) ++events;
        armed = true;
        next_trigger(d->ctrl->lifecycle_event());
    }
    tlm::tlm_response_status mem_read(unsigned port) {
        unsigned char buf[16]{};
        tlm::tlm_generic_payload tx;
        AxiExtension ext;
        ext.req.addr = 0x80000000;
        ext.req.size = 4;
        tx.set_extension(&ext);
        tx.set_command(tlm::TLM_READ_COMMAND);
        tx.set_address(0x80000000);
        tx.set_data_ptr(buf);
        tx.set_data_length(16);
        tx.set_streaming_width(16);
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_time t = sc_core::SC_ZERO_TIME;
        d->ports[port]->socket->b_transport(tx, t);
        tx.clear_extension(&ext);
        return tx.get_response_status();
    }
    // Issues a read on `port` and returns how long the call stayed blocked.
    sc_time blocked_for(unsigned port) {
        const sc_time t0 = sc_core::sc_time_stamp();
        (void)mem_read(port);  // the skeleton data path answers with an explicit error
        return sc_core::sc_time_stamp() - t0;
    }

    void handshakes() {
        Ddr3Controller& c = *d->ctrl;
        expect(!c.core_reset_safe() && !c.csr_reset_safe(), "no reset-safe without a request");
        c.core_reset_request(true);
        c.csr_reset_request(true);
        expect(c.core_reset_safe() && c.csr_reset_safe(), "idle controller acknowledges reset requests");
        c.set_core_reset(true);
        expect(!c.core_reset_safe(), "reset-safe is low while the core reset is asserted");
        c.set_core_reset(false);
        expect(c.core_reset_safe(), "reset-safe returns after release while still requested");
        c.core_reset_request(false);
        c.csr_reset_request(false);
        expect(!c.core_reset_safe() && !c.csr_reset_safe(), "dropping the request clears reset-safe");

        c.set_csr_reset(true);
        std::uint32_t v = 0;
        expect(d->csr->csr(tlm::TLM_READ_COMMAND, 0x0000, v) == tlm::TLM_GENERIC_ERROR_RESPONSE,
               "CSR access during CSR reset is an error");
        c.set_csr_reset(false);
        expect_eq(d->csr->rd(0x0000), 0x44445233u, "CSR usable after CSR reset release");

        c.port_quiesce_request(1, true);
        expect(c.port_quiesce_ack(1) && !c.port_quiesce_ack(0), "quiesce ack is per port");
        c.port_quiesce_request(1, false);
        expect(!c.port_quiesce_ack(1), "dropping quiesce releases the port");

        c.port_soft_reset_request(1, true);
        c.port_soft_reset_request(1, false);
        c.port_quiesce_request(1, true);
        expect(!c.port_soft_reset_active(1), "a request dropped before the ack never starts a reset");
        c.port_quiesce_request(1, false);
    }

    void traffic() {
        Ddr3Controller& c = *d->ctrl;
        c.qos_set_configured(0, true);  // FE05: no admission until the QoS table is configured
        c.qos_set_configured(1, true);
        // T1: physical core reset alone; released at +10 ns.
        c.set_core_reset(true);
        at(sc_time(10, SC_NS), [&c] { c.set_core_reset(false); });
        expect(blocked_for(0) == sc_time(10, SC_NS), "T1 core reset stalls the request until release");

        // T2: core reset request outlives the physical reset: blocked until the request drops.
        c.core_reset_request(true);
        c.set_core_reset(true);
        at(sc_time(10, SC_NS), [&c] { c.set_core_reset(false); });
        at(sc_time(25, SC_NS), [&c] { c.core_reset_request(false); });
        sc_time t0 = sc_core::sc_time_stamp();
        expect(blocked_for(0) == sc_time(25, SC_NS),
               "T2 still blocked after physical reset release while the core reset request is asserted");
        expect(sc_core::sc_time_stamp() - t0 == sc_time(25, SC_NS), "T2 resumed exactly at request release");

        // T3: core reset request alone stops every port (port 1 here).
        c.core_reset_request(true);
        expect(c.core_reset_safe(), "T3 a blocked, unaccepted call does not hold off reset-safe");
        at(sc_time(7, SC_NS), [&c] { c.core_reset_request(false); });
        expect(blocked_for(1) == sc_time(7, SC_NS), "T3 core reset request stops admission on every port");

        // T4: quiesce request alone; ack is not delayed by the blocked call.
        c.port_quiesce_request(0, true);
        at(sc_time(3, SC_NS), [this, &c] {
            expect(c.port_quiesce_ack(0), "T4 quiesce ack stays high while a call waits for admission");
        });
        at(sc_time(10, SC_NS), [&c] { c.port_quiesce_request(0, false); });
        expect(blocked_for(0) == sc_time(10, SC_NS), "T4 quiesce request stalls the request until release");

        // T5: quiesce outlives the soft-reset hold (hold = 8 controller cycles = 20 ns).
        c.qos_write(0, 3, 2);
        c.qos_set_configured(0, true);
        c.port_quiesce_request(0, true);
        c.port_soft_reset_request(0, true);
        expect(c.port_soft_reset_active(0), "T5 soft reset starts with quiesce acked");
        c.port_soft_reset_request(0, false);
        at(sc_time(30, SC_NS), [&c] { c.qos_set_configured(0, true); });  // table reset by the soft reset
        at(sc_time(35, SC_NS), [&c] { c.port_quiesce_request(0, false); });
        t0 = sc_core::sc_time_stamp();
        at(sc_time(21, SC_NS), [this, &c] {
            expect(!c.port_soft_reset_active(0), "T5 hold ended after 20 ns despite the early request drop");
            expect(!c.qos_configured(0), "T5 soft reset cleared QoS configured");
        });
        expect(blocked_for(0) == sc_time(35, SC_NS),
               "T5 still blocked after the hold while quiesce is asserted; resumes at quiesce release");
        expect(c.qos_class(0, 3) == kQosClassNormal, "T5 port soft reset resets the QoS table (MAS T6-64)");

        // T6: quiesce released during the hold: the hold is the last stop condition.
        c.port_quiesce_request(0, true);
        c.port_soft_reset_request(0, true);
        c.port_soft_reset_request(0, false);
        at(sc_time(5, SC_NS), [&c] { c.port_quiesce_request(0, false); });
        at(sc_time(25, SC_NS), [&c] { c.qos_set_configured(0, true); });
        expect(blocked_for(0) == sc_time(25, SC_NS),
               "T6 after the hold the reset QoS table is the last stop condition (FE05)");

        // T7: a blocked call is aborted when its own port domain enters reset.
        c.port_quiesce_request(1, true);
        at(sc_time(4, SC_NS), [&c] { c.set_port_reset(1, true); });
        t0 = sc_core::sc_time_stamp();
        expect(mem_read(1) != tlm::TLM_OK_RESPONSE && sc_core::sc_time_stamp() - t0 == sc_time(4, SC_NS),
               "T7 blocked call aborted with an error when the port domain enters reset");
        c.port_quiesce_request(1, false);
        expect(mem_read(1) != tlm::TLM_OK_RESPONSE, "T7 access on a port held in reset is an error");
        c.set_port_reset(1, false);
    }

    void run() {
        Ddr3Controller& c = *d->ctrl;
        handshakes();
        traffic();
        c.set_cold_reset(true);
        std::uint32_t v = 0;
        expect(d->csr->csr(tlm::TLM_READ_COMMAND, 0x0000, v) == tlm::TLM_GENERIC_ERROR_RESPONSE,
               "CSR access during cold reset is an error");
        c.set_cold_reset(false);
        expect_eq(d->csr->rd(0x0000), 0x44445233u, "CSR usable after cold reset release");
        wait(1, SC_NS);
        expect(events > 0, "lifecycle_event notified on level changes");
        bool threw = false;
        try { c.port_quiesce_request(4, true); } catch (const std::out_of_range&) { threw = true; }
        expect(threw, "port index outside the four bundles is rejected");
        threw = false;
        try { c.qos_write(0, 0, 4); } catch (const std::out_of_range&) { threw = true; }
        expect(threw, "QoS class is a 2-bit code");
        fx1_ddr3_test::completed() = true;
        sc_core::sc_stop();
    }
};

void case_reset_contract() {
    BuildConfig cfg;
    cfg.n_ports = 2;
    Dut d("ddr", cfg);
    ResetBench b("b");
    b.d = &d;
    sc_core::sc_report_handler::set_actions("/IEEE_Std_1666/deprecated", sc_core::SC_DO_NOTHING);
    sc_core::sc_report_handler::set_actions(sc_core::SC_WARNING, sc_core::SC_DO_NOTHING);
    sc_core::sc_start();
}

// ------------------------------------------------------------ PHY provider
struct PhyBench : sc_core::sc_module, PhyClient {
    FunctionalPhy phy{"phy"};
    std::vector<PhyObservation> obs;
    std::vector<std::pair<sc_time, PhyLevels>> level_log;
    SC_HAS_PROCESS(PhyBench);
    explicit PhyBench(sc_core::sc_module_name n) : sc_module(n) { phy.attach(this); }
    void phy_observation(const PhyObservation& o) override { obs.push_back(o); }
    void phy_levels_changed(const PhyLevels& l) override { level_log.emplace_back(sc_core::sc_time_stamp(), l); }
    PhyLoadRequest req(unsigned rank, std::uint16_t epoch, std::uint8_t tag, std::uint32_t payload) {
        PhyLoadRequest r;
        r.rank = rank;
        r.epoch = epoch;
        r.tag = tag;
        r.payload = payload;
        r.cls = PhyLoadClass::GateLevel;
        return r;
    }
};

struct HandshakeBench : PhyBench {
    SC_HAS_PROCESS(HandshakeBench);
    explicit HandshakeBench(sc_core::sc_module_name n) : PhyBench(n) { SC_THREAD(run); }
    void run() {
        expect(!phy.levels().reset_done && !phy.levels().ready, "PHY held in reset at start");
        wait(10, SC_NS);
        phy.set_phy_reset_n(true);
        wait(99, SC_NS);
        expect(!phy.levels().reset_done, "reset_done not before 100 ns");
        wait(2, SC_NS);
        expect(phy.levels().reset_done && !phy.levels().ready, "reset_done at 100 ns, ready later");
        wait(100, SC_NS);
        expect(phy.levels().ready && phy.levels().timing_wait_ok, "ready at 200 ns after release");
        expect(level_log.size() == 2, "two level-change notifications");
        phy.load(req(1, 7, 0, 0xA55A));
        expect(phy.load_outstanding() && phy.loads_received() == 1, "load outstanding");
        wait(49, SC_NS);
        expect(obs.empty(), "no observation before load latency");
        wait(2, SC_NS);
        expect(obs.size() == 1 && obs[0].rank == 1 && obs[0].epoch == 7 && obs[0].tag == 0 && obs[0].success &&
                   obs[0].rdlvl_resp == 0x5A && obs[0].wrlvl_resp == 0xA5 && obs[0].gate_mode == 1,
               "observation carries rank/epoch/tag and payload-derived responses");
        expect(!phy.load_outstanding(), "load retired after observation");
        phy.set_init_complete(true);
        phy.set_clock_disable(true);
        expect(phy.init_complete() && phy.clock_disabled(), "init_complete / clock_disable levels reach the PHY");
        // Provider words are opaque: no VP-defined layout (SOQ-12), returned unchanged.
        expect(phy.provider_status_word() == 0 && phy.provider_cap_word() == 0, "provider words default to 0");
        phy.set_status_word(0xCAFEF00Du);
        expect(phy.provider_status_word() == 0xCAFEF00Du, "provider status word is passed through unchanged");
        phy.set_phy_reset_n(false);
        expect(!phy.levels().ready && !phy.levels().reset_done, "PHY reset drops readiness immediately");
        fx1_ddr3_test::completed() = true;
        sc_core::sc_stop();
    }
};

void case_phy_handshake() {
    HandshakeBench b("b");
    sc_core::sc_start();
}

struct FaultBench : PhyBench {
    SC_HAS_PROCESS(FaultBench);
    explicit FaultBench(sc_core::sc_module_name n) : PhyBench(n) { SC_THREAD(run); }
    void run() {
        phy.set_phy_reset_n(true);
        wait(300, SC_NS);
        phy.script_load_faults({PhyLoadFault::Fail, PhyLoadFault::StaleEpoch, PhyLoadFault::WrongRank,
                                PhyLoadFault::WrongTag, PhyLoadFault::Delay, PhyLoadFault::Lose});
        phy.load(req(0, 5, 3, 1));
        wait(60, SC_NS);
        expect(obs.size() == 1 && !obs.back().success, "Fail: acknowledged with success = 0");
        phy.load(req(0, 5, 3, 1));
        wait(60, SC_NS);
        expect(obs.size() == 2 && obs.back().epoch == 4, "StaleEpoch: epoch - 1");
        phy.load(req(0, 5, 3, 1));
        wait(60, SC_NS);
        expect(obs.size() == 3 && obs.back().rank == 1, "WrongRank: other rank");
        phy.load(req(0, 5, 15, 1));
        wait(60, SC_NS);
        expect(obs.size() == 4 && obs.back().tag == 0, "WrongTag: tag + 1 wraps in 4 bits");
        phy.load(req(0, 5, 3, 1));
        wait(60, SC_NS);
        expect(obs.size() == 4, "Delay: not answered at the normal latency");
        wait(10, sc_core::SC_US);
        expect(obs.size() == 5 && obs.back().success, "Delay: answered after delayed latency");
        phy.load(req(0, 5, 3, 1));
        wait(20, sc_core::SC_US);
        expect(obs.size() == 5 && phy.load_outstanding(), "Lose: never answered, stays outstanding");
        phy.set_phy_reset_n(false);
        expect(!phy.load_outstanding(), "PHY reset discards the lost load");
        phy.set_phy_reset_n(true);
        wait(300, SC_NS);
        phy.load(req(0, 6, 1, 2));
        phy.set_phy_reset_n(false);
        wait(100, SC_NS);
        expect(obs.size() == 5, "reset during a pending load suppresses its observation");
        phy.set_phy_reset_n(true);
        wait(300, SC_NS);
        phy.force_failure(true);
        expect(phy.levels().failure, "forced failure level");
        phy.force_failure(false);
        phy.force_ready(false);
        expect(!phy.levels().ready && phy.levels().reset_done, "forced readiness loss");
        phy.force_ready(true);
        expect(phy.levels().ready, "readiness restored");
        fx1_ddr3_test::completed() = true;
        sc_core::sc_stop();
    }
};

void case_phy_faults() {
    FaultBench b("b");
    sc_core::sc_start();
}

// ------------------------------------------------------------ epoch scheduler
struct EpochBench : sc_core::sc_module {
    EpochScheduler s{"s"};
    std::vector<std::string> log;
    SC_HAS_PROCESS(EpochBench);
    explicit EpochBench(sc_core::sc_module_name n) : sc_module(n) { SC_THREAD(run); }
    void note(const std::string& what) {
        log.push_back(what + "@" + std::to_string(static_cast<long long>(sc_core::sc_time_stamp().to_seconds() * 1e9 + 0.5)));
    }
    void run() {
        s.schedule(sc_time(10, SC_NS), 0, [this] { note("a"); });
        s.schedule(sc_time(20, SC_NS), 0, [this] { note("stale"); });
        s.schedule(sc_time(20, SC_NS), 1, [this] { note("other"); });
        s.schedule(sc_time(30, SC_NS), 1, [this] { note("x"); });
        s.schedule(sc_time(30, SC_NS), 1, [this] { note("y"); });
        const auto h = s.schedule(sc_time(25, SC_NS), 1, [this] { note("cancelled"); });
        s.schedule(sc_time(5, SC_NS), 2, [this] {
            note("chain");
            s.schedule(sc_time(3, SC_NS), 2, [this] { note("chained"); });
        });
        wait(15, SC_NS);
        expect(s.bump(0) == 1 && s.epoch(0) == 1 && s.epoch(1) == 0, "bump advances only its domain");
        s.cancel(h);
        wait(50, SC_NS);
        const std::vector<std::string> want{"chain@5", "chained@8", "a@10", "other@20", "x@30", "y@30"};
        expect(log == want, "stale epoch and cancelled actions never run; same-time order is FIFO");
        expect(s.executed() == 6 && s.discarded() == 2, "executed/discarded counters");
        fx1_ddr3_test::completed() = true;
        sc_core::sc_stop();
    }
};

void case_epoch() {
    EpochBench b("b");
    sc_core::sc_start();
}

} // namespace

int sc_main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    if (which == "bind_profiles") case_bind_profiles();
    else if (which == "identity") case_identity();
    else if (which == "bad_port_binding") case_bad_port_binding();
    else if (which == "no_extension") case_no_extension();
    else if (which == "csr_robustness") case_csr_robustness();
    else if (which == "reset_contract") case_reset_contract();
    else if (which == "phy_handshake") case_phy_handshake();
    else if (which == "phy_faults") case_phy_faults();
    else if (which == "epoch") case_epoch();
    else {
        std::cerr << "unknown case '" << which << "'\n";
        return 2;
    }
    if (which == "bad_port_binding") fx1_ddr3_test::completed() = true;  // no bench thread
    expect(fx1_ddr3_test::completed(), "scenario ran to completion (no hung thread)");
    return fx1_ddr3_test::summary("fx1_ddr3 tlm " + which);
}
