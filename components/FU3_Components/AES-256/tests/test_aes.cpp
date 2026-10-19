#include "fu3/aes/aes_controller.h"
#include <tlm_utils/simple_initiator_socket.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include "vector_file.h"
#ifdef FU3_AES_BUS_TEST
#include "fu3_bus/bus_system.h"
#endif

using namespace sc_core;
using namespace fu3::aes;
using Words = std::array<std::uint32_t, 4>;
// FIPS 197 (2001), Appendix C.1/C.3. Ciphertexts are independent constants.
const Words plain{0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff};
const Words cipher128{0x69c4e0d8, 0x6a7b0430, 0xd8cdb780, 0x70b4c55a};
const Words cipher256{0x8ea2b7ca, 0x516745bf, 0xeafc4990, 0x4b496089};
const std::array<std::uint32_t, 8> key_words{
    0x00010203, 0x04050607, 0x08090a0b, 0x0c0d0e0f,
    0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f};

struct Bench : sc_module {
    tlm_utils::simple_initiator_socket<Bench> cpu{"cpu"};
    sc_signal<bool> clk{"clk"}, rst{"rst"};
    bool clock_enabled = true, passed = false;
    unsigned checks = 0;
    std::string scenario;
    std::string vector_path;
    sc_time period{10, SC_NS};
    sc_time reset_hold{20, SC_NS};
    sc_event reset_request, reset_done;
    sc_event workers_done;
    struct Worker {
        sc_event start;
        unsigned offset = 0;
        std::uint32_t value = 0;
        bool done = true;
        tlm::tlm_response_status response = tlm::TLM_INCOMPLETE_RESPONSE;
        sc_time finish = SC_ZERO_TIME;
        std::string error;
    } workers[3];
    Aes aes;
#ifdef FU3_AES_BUS_TEST
    static fu3_bus::BusConfig bus_config() {
        auto c = fu3_bus::BusConfig::fu3();
        for (auto& t : c.targets) t.enabled = t.name == "AES";
        return c;
    }
    fu3_bus::BusSystem fabric{"fabric", bus_config()};
    static constexpr std::uint64_t base = 0x40000000;
#else
    static constexpr std::uint64_t base = 0;
#endif
    static Config config(const std::string& s) {
        Config c;
        if (s == "result_mux") c.result_readback = ResultReadback::ObservedRtlMux;
        if (s == "latency_1") { c.init_cycles = 1; c.block_cycles = 1; }
        if (s == "latency_2") { c.init_cycles = 2; c.block_cycles = 3; }
        if (s == "latency_17") { c.init_cycles = 17; c.block_cycles = 65; }
        if (s == "latency_257") { c.init_cycles = 257; c.block_cycles = 513; }
        if (s == "busy_matrix") { c.init_cycles = 256; c.block_cycles = 256; }
        return c;
    }
    SC_HAS_PROCESS(Bench);
    Bench(sc_module_name name, const std::string& s) : sc_module(name), scenario(s), aes("aes", config(s)) {
#ifdef FU3_AES_BUS_TEST
        cpu.bind(fabric.initiator("CVA6"));
        fabric.target("AES").bind(aes.socket);
#else
        cpu.bind(aes.socket);
#endif
        aes.clk(clk); aes.reset_n(rst);
        if (s == "latency_1") period = sc_time(2, SC_NS);
        if (s == "latency_2") period = sc_time(7, SC_NS);
        if (s == "latency_17") period = sc_time(37, SC_NS);
        SC_THREAD(clock_driver);
        SC_THREAD(reset_driver);
        SC_THREAD(worker0); SC_THREAD(worker1); SC_THREAD(worker2);
        SC_THREAD(run);
    }
    void clock_driver() {
        for (;;) { clk = clock_enabled; wait(period / 2); clk = false; wait(period / 2); }
    }
    void reset_driver() {
        rst = false; wait(21, SC_NS); rst = true;
        for (;;) {
            wait(reset_request);
            rst = false; wait(reset_hold); rst = true;
            reset_done.notify(SC_ZERO_TIME);
        }
    }
    void check(bool b, const char* message) {
        ++checks;
        if (!b) throw std::runtime_error(message);
    }
    void worker0() { worker(0); }
    void worker1() { worker(1); }
    void worker2() { worker(2); }
    void worker(unsigned i) {
        for (;;) {
            auto& w = workers[i]; wait(w.start);
            try { w.response = access(true, w.offset, w.value); }
            catch (const std::exception& e) { w.error = e.what(); }
            w.finish = sc_time_stamp(); w.done = true; workers_done.notify(SC_ZERO_TIME);
        }
    }
    void transfer(tlm::tlm_generic_payload& tx, sc_time& delay) { cpu->b_transport(tx, delay); }
    tlm::tlm_response_status access(bool write, unsigned off, std::uint32_t& value,
                                  unsigned char* be = nullptr, unsigned be_len = 0,
                                  sc_time delay = SC_ZERO_TIME) {
        unsigned char bytes[4];
        for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<unsigned char>(value >> (8 * i));
        tlm::tlm_generic_payload tx;
        tx.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        tx.set_address(base + off); tx.set_data_ptr(bytes);
        tx.set_data_length(4); tx.set_streaming_width(4);
        tx.set_byte_enable_ptr(be); tx.set_byte_enable_length(be_len);
#ifdef FU3_AES_BUS_TEST
        fu3_bus::ScopedAccess metadata(tx, 4, fu3_bus::Burst::Increment);
#endif
        transfer(tx, delay);
        if (delay != SC_ZERO_TIME) wait(delay);
        check(tx.get_address() == base + off && tx.get_data_length() == 4, "payload preserved");
        check(tx.get_data_ptr() == bytes && tx.get_streaming_width() == 4 &&
              tx.get_byte_enable_ptr() == be && tx.get_byte_enable_length() == be_len &&
              tx.get_command() == (write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND),
              "payload pointers, lanes, streaming and command preserved");
        check(!tx.is_dmi_allowed(), "DMI disabled for CSRs");
        value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(bytes[i]) << (8 * i);
        return tx.get_response_status();
    }
    void wr(unsigned off, std::uint32_t value) {
        check(access(true, off, value) == tlm::TLM_OK_RESPONSE, "MMIO write");
    }
    std::uint32_t rd(unsigned off) {
        std::uint32_t value = 0;
        check(access(false, off, value) == tlm::TLM_OK_RESPONSE, "MMIO read");
        return value;
    }
    std::uint32_t peek(unsigned off) {
        unsigned char bytes[4]{};
        tlm::tlm_generic_payload tx;
        tx.set_command(tlm::TLM_READ_COMMAND); tx.set_address(base + off);
        tx.set_data_ptr(bytes); tx.set_data_length(4); tx.set_streaming_width(4);
        check(cpu->transport_dbg(tx) == 4, "debug read");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(bytes[i]) << (8 * i);
        return value;
    }
    void poll(unsigned flag) {
        for (unsigned i = 0; i < 1000; ++i) if (rd(reg::STATUS) & flag) return;
        check(false, "polling timed out");
    }
    void program(bool wide) {
        wr(reg::CONFIG, reg::ENCRYPT | (wide ? reg::KEY256 : 0));
        for (unsigned i = 0; i < 8; ++i) wr(reg::key(i), key_words[i]);
    }
    void input(const Words& words) {
#ifdef FU3_AES_BUS_TEST
        // Real INCR burst: fabric must split into four local register writes.
        unsigned char bytes[16];
        for (unsigned i = 0; i < 16; ++i)
            bytes[i] = static_cast<unsigned char>(words[i / 4] >> (8 * (i % 4)));
        tlm::tlm_generic_payload tx;
        tx.set_command(tlm::TLM_WRITE_COMMAND); tx.set_address(base + reg::BLOCK0);
        tx.set_data_ptr(bytes); tx.set_data_length(16); tx.set_streaming_width(16);
        fu3_bus::ScopedAccess metadata(tx, 4, fu3_bus::Burst::Increment);
        sc_time delay = SC_ZERO_TIME;
        transfer(tx, delay); if (delay != SC_ZERO_TIME) wait(delay);
        check(tx.is_response_ok(), "FU3 INCR input burst");
        check(tx.get_address() == base + reg::BLOCK0, "burst address restored");
#else
        for (unsigned i = 0; i < 4; ++i) wr(reg::block(i), words[i]);
#endif
    }
    void output(const Words& words) {
        for (unsigned i = 0; i < 4; ++i) check(rd(reg::result(i)) == words[i], "known-answer result word");
        check(rd(reg::STATUS) == 3, "reading result does not clear VALID");
    }
    void init() { wr(reg::CTRL, reg::INIT); poll(reg::READY); }
    void next() { wr(reg::CTRL, reg::NEXT); poll(reg::VALID); }
    void reject(unsigned off, std::uint32_t value) {
        check(access(true, off, value) == tlm::TLM_GENERIC_ERROR_RESPONSE, "illegal sequence rejected");
    }
    void vectors(bool wide) {
        program(wide); input(plain); init();
        check(rd(reg::STATUS) == 1, "INIT ready without VALID");
        next(); output(wide ? cipher256 : cipher128);
        wr(reg::CONFIG, wide ? reg::KEY256 : 0); // Reuse schedule, switch direction.
        input(wide ? cipher256 : cipher128);
        next(); output(plain);
        wr(reg::CONFIG, reg::ENCRYPT | (wide ? reg::KEY256 : 0));
        input(plain); next(); output(wide ? cipher256 : cipher128);
    }
    void registers() {
        check(rd(reg::NAME0) == 0x61657320 && rd(reg::NAME1) == 0x20202020, "identity");
        check(rd(reg::VERSION) == 0x302e3630, "version 0.60");
        check(rd(reg::STATUS) == 1 && rd(reg::CTRL) == 0, "reset state");
        for (unsigned off = 0; off < reg::APERTURE; off += 4) {
            if (off == reg::CTRL || off == reg::CONFIG ||
                (off >= reg::KEY0 && off <= reg::key(7)) ||
                (off >= reg::BLOCK0 && off <= reg::block(3))) continue;
            const auto before = rd(off); wr(off, 0xffffffff);
            check(rd(off) == before, "RO/reserved writes ignored");
        }
        program(true); input(plain);
        for (unsigned i = 0; i < 8; ++i) check(rd(reg::key(i)) == 0, "key reads zero");
        for (unsigned i = 0; i < 4; ++i) check(rd(reg::block(i)) == 0, "input reads zero");
        wr(reg::CONFIG, 0xffffffff);
        check(rd(reg::CONFIG) == 0 && rd(reg::CTRL) == 12, "CONFIG masked and mirrored");
    }
    void commands() {
        reject(reg::CTRL, reg::NEXT);
        program(true); input(plain);
        wr(reg::CTRL, 3); // VP policy: INIT wins.
        check(peek(reg::CTRL) == 15, "one-cycle INIT/NEXT readback pulse");
        check(rd(reg::CTRL) == 12, "command self clears");
        reject(reg::CTRL, reg::NEXT); reject(reg::CONFIG, 0);
        reject(reg::KEY0, 0); reject(reg::BLOCK0, 0);
        poll(reg::READY); check(rd(reg::STATUS) == 1, "INIT priority");
        next(); output(cipher256);
        wr(reg::CTRL, reg::NEXT);
        check(peek(reg::STATUS) == 0, "NEXT clears READY/VALID immediately");
        reject(reg::CTRL, reg::INIT); reject(reg::KEY0, 0);
        poll(reg::VALID); output(cipher256);
        wr(reg::KEY0, key_words[0]); reject(reg::CTRL, reg::NEXT);
        init(); next(); output(cipher256);
        wr(reg::CONFIG, 1); reject(reg::CTRL, reg::NEXT);
        init(); next(); output(cipher128);
    }
    void protocol() {
        unsigned char bytes[8]{}, be[4]{0xff, 0xff, 0xff, 0xff};
        tlm::tlm_generic_payload tx;
        tx.set_command(tlm::TLM_WRITE_COMMAND); tx.set_address(reg::CONFIG);
        tx.set_data_ptr(bytes); tx.set_data_length(4); tx.set_streaming_width(4);
        auto expect = [&](tlm::tlm_response_status status) {
            sc_time delay = SC_ZERO_TIME; transfer(tx, delay);
            check(tx.get_response_status() == status, "malformed payload response");
        };
        tx.set_command(tlm::TLM_IGNORE_COMMAND); expect(tlm::TLM_COMMAND_ERROR_RESPONSE);
        tx.set_command(tlm::TLM_WRITE_COMMAND); tx.set_data_ptr(nullptr); expect(tlm::TLM_GENERIC_ERROR_RESPONSE);
        tx.set_data_ptr(bytes); tx.set_data_length(8); expect(tlm::TLM_BURST_ERROR_RESPONSE);
        tx.set_data_length(4); tx.set_streaming_width(2); expect(tlm::TLM_BURST_ERROR_RESPONSE);
        tx.set_streaming_width(4); tx.set_address(1); expect(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        tx.set_address(reg::APERTURE); expect(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        tx.set_address(0x100000000ULL); expect(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        tx.set_address(reg::CONFIG); tx.set_byte_enable_ptr(be); expect(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
        tx.set_byte_enable_length(4); be[0] = 1; expect(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
        check(cpu->transport_dbg(tx) == 0, "malformed debug rejected");
        be[0] = 0xff; check(cpu->transport_dbg(tx) == 0, "debug writes rejected");
        program(true);
        // Compose the first key through byte strobes, using its real WO storage.
        wr(reg::KEY0, 0xffffffff);
        for (unsigned lane = 0; lane < 4; ++lane) {
            for (auto& b : be) b = 0;
            be[lane] = 0xff;
            auto value = key_words[0];
            check(access(true, reg::KEY0, value, be, 4) == tlm::TLM_OK_RESPONSE, "WSTRB merge");
        }
        input(plain); init(); next(); output(cipher256);
        unsigned char disabled = 0;
        std::uint32_t value = 0;
        check(access(true, reg::KEY0, value, &disabled, 1) == tlm::TLM_OK_RESPONSE, "all lanes disabled");
        next(); output(cipher256); // Disabled write must not invalidate schedule.
        value = 0xdeadbeef;
        check(access(false, reg::NAME0, value, &disabled, 1) == tlm::TLM_OK_RESPONSE && value == 0xdeadbeef,
              "disabled read lanes preserved");
        const auto before = sc_time_stamp();
        value = 0;
        check(access(false, reg::STATUS, value, nullptr, 0, sc_time(37, SC_NS)) == tlm::TLM_OK_RESPONSE,
              "annotated request succeeds");
        check(sc_time_stamp() >= before + sc_time(37, SC_NS), "incoming delay consumed");
    }
    void reset_test() {
        program(true); init(); input(plain); wr(reg::CTRL, reg::NEXT);
        reset_request.notify(SC_ZERO_TIME); wait(reset_done);
        wait(1, SC_US);
        check(rd(reg::STATUS) == 1 && rd(reg::CTRL) == 0, "active reset aborts job and clears config");
        for (unsigned i = 0; i < 4; ++i) check(rd(reg::result(i)) == 0, "no stale result after reset");
        reject(reg::CTRL, reg::NEXT);
        program(true); wr(reg::CTRL, reg::INIT);
        reset_request.notify(SC_ZERO_TIME); wait(reset_done);
        wait(200, SC_NS); reject(reg::CTRL, reg::NEXT);
        reset_request.notify(20, SC_NS);
        std::uint32_t value = 3;
        check(access(true, reg::CONFIG, value, nullptr, 0, sc_time(60, SC_NS)) == tlm::TLM_GENERIC_ERROR_RESPONSE,
              "reset cancels delayed transaction across release");
        check(rd(reg::CTRL) == 0, "cancelled delayed write has no effect");
        vectors(true);
    }
    void timing() {
        program(true); input(plain); wr(reg::CTRL, reg::INIT);
        check(peek(reg::STATUS) == 0, "INIT busy at acceptance");
        wait(159, SC_NS); check(peek(reg::STATUS) == 0, "INIT not ready before 16 edges");
        wait(2, SC_NS); check(peek(reg::STATUS) == 1, "INIT ready after 16 edges");
        wr(reg::CTRL, reg::NEXT);
        wait(639, SC_NS); check(peek(reg::STATUS) == 0, "result not visible before 64 edges");
        wait(2, SC_NS); check(peek(reg::STATUS) == 3, "result valid after 64 edges");
        output(cipher256);
        wr(reg::CTRL, reg::NEXT);
        clock_enabled = false;
        wait(2, SC_US); check(peek(reg::STATUS) == 0, "stopped clock freezes processing");
        clock_enabled = true; poll(reg::VALID); output(cipher256);
    }
    void result_mux() {
        program(true); input(plain); init(); next();
        check(rd(reg::result(0)) == cipher256[3], "observed RESULT0 truncation");
        check(rd(reg::result(1)) == 0 && rd(reg::result(2)) == 0, "observed missing decode");
        check(rd(reg::result(3)) == cipher256[3], "observed RESULT3");
    }
#include "test_corners.inc"
    void run() {
        const bool reference = scenario == "oracle128" || scenario == "oracle256" || scenario == "sequence_oracle";
        try {
            wait(31, SC_NS);
            if (scenario == "registers") registers();
            else if (scenario == "aes128") vectors(false);
            else if (scenario == "aes256") vectors(true);
            else if (scenario == "commands") commands();
            else if (scenario == "protocol") protocol();
            else if (scenario == "reset") reset_test();
            else if (scenario == "timing") timing();
            else if (scenario == "result_mux") result_mux();
            else if (scenario == "strobes") strobes();
            else if (scenario == "status_flow") status_flow();
            else if (scenario == "access_matrix") access_matrix();
            else if (scenario == "concurrency") concurrency();
            else if (scenario == "reset_edges") reset_edges();
            else if (scenario == "reset_entry") reset_entry();
            else if (scenario == "busy_matrix") busy_matrix();
            else if (scenario == "reset_boundaries") reset_boundaries();
            else if (scenario == "debug_invariants") debug_invariants();
            else if (scenario == "hds_fields") hds_fields();
            else if (scenario == "sequence_oracle") sequence_oracle();
            else if (scenario.rfind("latency_", 0) == 0) timing_parameters();
            else if (scenario == "oracle128" || scenario == "oracle256") oracle(scenario == "oracle256");
#ifdef FU3_AES_BUS_TEST
            else if (scenario == "bus_lanes") bus_lanes();
#endif
            else throw std::runtime_error("unknown scenario");
            passed = true;
            std::cout << (reference ? "REFERENCE_PASS " : "POLICY_PASS ") << scenario
                      << " checks=" << checks << " time=" << sc_time_stamp() << '\n';
        } catch (const std::exception& e) {
            std::cerr << (reference ? "REFERENCE_FAIL " : "POLICY_FAIL ") << scenario << ": " << e.what() << '\n';
        }
        sc_stop();
    }
};
int sc_main(int argc, char** argv) {
    Bench bench{"bench", argc > 1 ? argv[1] : "aes256"};
    if (argc > 2) bench.vector_path = argv[2];
    sc_start(sc_time(20, SC_MS));
    if (!bench.passed) std::cerr << "Test failed or simulation watchdog expired\n";
    return bench.passed ? 0 : 1;
}
