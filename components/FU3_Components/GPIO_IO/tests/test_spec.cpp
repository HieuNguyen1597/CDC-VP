#include "fu3/gpio/gpio.h"
#include "fu3/gpio/io_pad.h"
#include <array>
#include <iostream>
#include <random>
#include <string>
#include <tlm_utils/simple_initiator_socket.h>

using namespace sc_core;
using namespace fu3::gpio;
using U32 = std::uint32_t;

// Core pins are tested directly, including the 24 unbonded bits of each bank.
// IO_PAD has independent primary/alternate endpoints, without GPIO overrides.
struct SpecBench : sc_module {
    Gpio a{"a"}, b{"b"}, c{"c"};
    IoPad pad{"pad"};
    sc_clock oscillator{"oscillator", 10, SC_NS, 0.5, 5, SC_NS};
    sc_signal<bool> clock_enable{"clock_enable"}, clock{"clock"}, reset_n{"reset_n"};
    sc_vector<sc_signal<sc_dt::sc_uint<32>>> input{"input", 3}, output{"output", 3},
        direction{"direction", 3}, synced{"synced", 3};
    sc_vector<sc_signal<sc_dt::sc_uint<6>>> cfg{"cfg", 96};
    sc_vector<sc_signal<bool>> irq{"irq", 3}, wake{"wake", 3};
    sc_vector<tlm_utils::simple_initiator_socket<SpecBench>> cpu{"cpu", 3};
    sc_vector<sc_signal<sc_dt::sc_uint<2>>> select{"select", 80};
    sc_vector<sc_signal<bool>> pins{"pins", 80}, po{"po", 80}, ao{"ao", 80}, pe{"pe", 80},
        ae{"ae", 80}, data{"data", 80}, enable{"enable", 80}, pi{"pi", 80}, ai{"ai", 80};
    sc_signal<bool> test_mode{"test_mode"}, test_select{"test_select"};
    sc_event job, done;
    tlm::tlm_response_status job_status = tlm::TLM_OK_RESPONSE;
    unsigned checks = 0, errors = 0;
    std::string scenario;
    SC_HAS_PROCESS(SpecBench);
    SpecBench(sc_module_name name, const std::string& which) : sc_module(name), scenario(which) {
        Gpio* banks[] = {&a, &b, &c};
        for (unsigned bank = 0; bank < 3; ++bank) {
            auto& g = *banks[bank];
            cpu[bank].bind(g.socket);
            g.clk(clock);
            g.reset_n(reset_n);
            g.gpio_in(input[bank]);
            g.gpio_out(output[bank]);
            g.gpio_dir(direction[bank]);
            g.gpio_in_sync(synced[bank]);
            g.interrupt(irq[bank]);
            g.power_event(wake[bank]);
            for (unsigned i = 0; i < 32; ++i)
                g.gpio_padcfg[i](cfg[32 * bank + i]);
        }
        for (unsigned i = 0; i < 80; ++i) {
            pad.select[i](select[i]);
            pad.pad_in[i](pins[i]);
            pad.primary_out[i](po[i]);
            pad.alternate_out[i](ao[i]);
            pad.primary_oe[i](pe[i]);
            pad.alternate_oe[i](ae[i]);
            pad.pad_data[i](data[i]);
            pad.requested_oe[i](enable[i]);
            pad.primary_in[i](pi[i]);
            pad.alternate_in[i](ai[i]);
        }
        pad.test_mode(test_mode);
        pad.test_select(test_select);
        SC_METHOD(gate_clock);
        sensitive << oscillator << clock_enable;
        SC_THREAD(writer);
        SC_THREAD(run);
    }
    void gate_clock() {
        clock.write(clock_enable.read() && oscillator.read());
    }
    void check(bool ok, const std::string& message) {
        ++checks;
        if (!ok) {
            ++errors;
            if (errors <= 30)
                std::cerr << "FAIL " << scenario << " @ " << sc_time_stamp() << ": " << message
                          << '\n';
        }
    }
    void settle() {
        wait(1, SC_NS);
    }
    void tick(unsigned n = 1) {
        for (unsigned i = 0; i < n; ++i) {
            wait(clock.posedge_event());
            settle();
        }
    }
    void pause() {
        wait(clock.negedge_event());
        clock_enable.write(false);
        settle();
    }
    void resume() {
        clock_enable.write(true);
        settle();
    }
    void reset() {
        reset_n.write(false);
        for (unsigned bank = 0; bank < 3; ++bank)
            input[bank].write(0);
        settle();
        reset_n.write(true);
        settle();
    }
    U32 access(unsigned bank, bool write, unsigned address, U32 value = 0,
               tlm::tlm_response_status expected = tlm::TLM_OK_RESPONSE, bool debug = false) {
        unsigned char bytes[4];
        for (unsigned i = 0; i < 4; ++i)
            bytes[i] = (value >> (8 * i)) & 255;
        tlm::tlm_generic_payload tx;
        tx.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        tx.set_address(address);
        tx.set_data_ptr(bytes);
        tx.set_data_length(4);
        tx.set_streaming_width(4);
        tx.set_dmi_allowed(true);
        if (debug)
            check(cpu[bank]->transport_dbg(tx) == 4, "debug read transfers one word");
        else {
            sc_time delay = SC_ZERO_TIME;
            cpu[bank]->b_transport(tx, delay);
            check(delay == SC_ZERO_TIME, "transport delay consumed");
        }
        check(tx.get_response_status() == expected,
              "response bank " + std::to_string(bank) + " offset " + std::to_string(address));
        check(!tx.is_dmi_allowed(), "DMI disabled for MMIO");
        U32 result = 0;
        for (unsigned i = 0; i < 4; ++i)
            result |= U32(bytes[i]) << (8 * i);
        return result;
    }
    void wr(unsigned bank, unsigned address, U32 value) {
        access(bank, true, address, value);
    }
    U32 rd(unsigned bank, unsigned address) {
        return access(bank, false, address);
    }
    U32 peek(unsigned bank, unsigned address) {
        return access(bank, false, address, 0, tlm::TLM_OK_RESPONSE, true);
    }
    void writer() {
        while (true) {
            wait(job);
            access(0, true, reg::PADOUT, 0x76543210, job_status);
            done.notify(SC_ZERO_TIME);
        }
    }
    void register_outputs() {
        for (unsigned bank = 0; bank < 3; ++bank) {
            for (unsigned pin = 0; pin < 32; ++pin)
                for (unsigned invert = 0; invert < 2; ++invert) {
                    const U32 value = (U32{1} << pin) ^ (invert ? 0xffffffffu : 0);
                    wr(bank, reg::PADOUT, value);
                    wr(bank, reg::PADDIR, ~value);
                    settle();
                    check(output[bank].read() == value && direction[bank].read() == U32(~value),
                          "all 32 output/direction ports, walking ones/zeros");
                    wr(bank, reg::PADDIR, value);
                    settle();
                    check(output[bank].read() == value && direction[bank].read() == value,
                          "direction does not gate output data");
                    for (unsigned other = 0; other < 3; ++other)
                        if (other != bank)
                            check(output[other].read() == 0 && direction[other].read() == 0,
                                  "direct port bank isolation");
                }
            wr(bank, reg::PADOUT, 0);
            wr(bank, reg::PADDIR, 0);
            std::array<unsigned, 32> expected;
            expected.fill(2);
            for (unsigned pin = 0; pin < 32; ++pin) {
                const unsigned word = pin / 4, shift = 8 * (pin % 4);
                for (unsigned value = 0; value < 64; ++value) {
                    U32 packed = 0;
                    for (unsigned j = 0; j < 4; ++j)
                        packed |= U32(expected[word * 4 + j]) << (8 * j);
                    packed = (packed & ~(U32{63} << shift)) | (U32(value) << shift);
                    wr(bank, reg::PADCFG0 + 4 * word, packed | 0xc0c0c0c0);
                    expected[pin] = value;
                    settle();
                    check(peek(bank, reg::PADCFG0 + 4 * word) == packed,
                          "six-bit packing and reserved bits ignored");
                    for (unsigned j = 0; j < 32; ++j)
                        check(cfg[bank * 32 + j].read() == expected[j],
                              "all 64 pad values; adjacent pins preserved");
                    for (unsigned other = 0; other < 3; ++other)
                        if (other != bank)
                            for (unsigned j = 0; j < 32; ++j)
                                check(cfg[other * 32 + j].read() == 2,
                                      "pad configuration bank isolation");
                }
            }
            // Restore this bank so subsequent isolation tests have a known baseline.
            for (unsigned word = 0; word < 8; ++word)
                wr(bank, reg::PADCFG0 + word * 4, reg::PADCFG_RESET);
        }
    }
    void input_stream() {
        U32 q0[3] = {}, q1[3] = {}, q2[3] = {};
        std::mt19937 rng(0x6701);
        for (unsigned cycle = 0; cycle < 256; ++cycle) {
            U32 next[3];
            for (unsigned bank = 0; bank < 3; ++bank) {
                next[bank] = cycle < 64 ? (U32{1} << (cycle % 32)) ^ (cycle >= 32 ? 0xffffffffu : 0)
                                        : U32(rng());
                input[bank].write(next[bank]);
            }
            settle();
            tick();
            for (unsigned bank = 0; bank < 3; ++bank) {
                q2[bank] = q1[bank];
                q1[bank] = q0[bank];
                q0[bank] = next[bank];
                check(synced[bank].read() == q1[bank], "back-to-back samples, second stage");
                check(peek(bank, reg::PADIN) == q2[bank], "back-to-back samples, third stage");
            }
        }
        reset();
        // A pulse entirely between rising edges is not captured by the synchronizer.
        tick();
        for (unsigned bank = 0; bank < 3; ++bank)
            input[bank].write(0xffffffff);
        settle();
        for (unsigned bank = 0; bank < 3; ++bank)
            input[bank].write(0);
        settle();
        tick(4);
        for (unsigned bank = 0; bank < 3; ++bank)
            check(peek(bank, reg::PADIN) == 0, "unsampled sub-cycle pulse not captured");
        // One sampled clock of pulse is preserved: no debounce/glitch filter.
        for (unsigned bank = 0; bank < 3; ++bank)
            input[bank].write(0xffffffff);
        settle();
        tick();
        for (unsigned bank = 0; bank < 3; ++bank)
            input[bank].write(0);
        settle();
        tick();
        for (unsigned bank = 0; bank < 3; ++bank)
            check(synced[bank].read() == 0xffffffff, "sampled one-cycle pulse reaches sync output");
        tick();
        for (unsigned bank = 0; bank < 3; ++bank)
            check(peek(bank, reg::PADIN) == 0xffffffff, "one-cycle pulse reaches PADIN");
        tick();
        for (unsigned bank = 0; bank < 3; ++bank)
            check(peek(bank, reg::PADIN) == 0, "one-cycle pulse falls after pipeline delay");
    }
    void irq_boundaries() {
        for (unsigned bank = 0; bank < 3; ++bank) {
            reset();
            wr(bank, reg::INTTYPE1, 1);
            wr(bank, reg::INTEN, 1);
            input[bank].write(1);
            settle();
            tick(2);
            check(rd(bank, reg::INTSTATUS) == 0, "empty RC read returns pre-edge status");
            settle();
            check(irq[bank].read() && peek(bank, reg::INTSTATUS) == 1,
                  "first edge coincident with empty read is latched");
            wr(bank, reg::INTSTATUS, 0xffffffff);
            check(irq[bank].read() && peek(bank, reg::INTSTATUS) == 1, "status write is not W1C");
            wr(bank, reg::INTEN, 0);
            wr(bank, reg::INTTYPE0, 0xffffffff);
            check(irq[bank].read() && peek(bank, reg::INTSTATUS) == 1,
                  "disabling/reprogramming does not clear pending status");
            check(rd(bank, reg::PADOUT) == 0 && irq[bank].read(),
                  "neighboring read does not clear status");
            // Invalid reads must not clear an IRQ or consume a clock edge.
            unsigned char bytes[4] = {};
            tlm::tlm_generic_payload tx;
            tx.set_command(tlm::TLM_READ_COMMAND);
            tx.set_address(reg::INTSTATUS);
            tx.set_data_ptr(bytes);
            tx.set_data_length(2);
            tx.set_streaming_width(2);
            sc_time delay = SC_ZERO_TIME;
            cpu[bank]->b_transport(tx, delay);
            check(tx.get_response_status() == tlm::TLM_BURST_ERROR_RESPONSE && irq[bank].read(),
                  "malformed RC read has no clear side effect");
            check(rd(bank, reg::INTSTATUS) == 1, "pending status survives configuration changes");
            tick(2);
            check(!irq[bank].read(), "disabled bank remains clear");

            reset();
            input[bank].write(1);
            settle();
            tick(4);
            wr(bank, reg::INTEN, 1);
            settle();
            check(!irq[bank].read(), "enable write uses old enable at access edge");
            wr(bank, reg::INTEN, 0);
            settle();
            check(irq[bank].read(), "disable write still observes old enable at access edge");
            check(rd(bank, reg::INTSTATUS) == 1, "disable-edge status is latched");
            tick();
            check(!irq[bank].read(), "new disabled configuration applies following edge");

            reset();
            wr(bank, reg::INTTYPE1, 1);
            input[bank].write(1);
            settle();
            tick(4);
            wr(bank, reg::INTEN, 1);
            tick(3);
            check(!irq[bank].read(), "edge occurring while disabled is not replayed on enable");
            input[bank].write(0);
            settle();
            tick(3);
            check(!irq[bank].read(), "rising mode ignores falling edge");
            input[bank].write(1);
            settle();
            tick(3);
            check(irq[bank].read() && peek(bank, reg::INTSTATUS) == 1,
                  "fresh enabled rising edge is accepted");

            reset();
            wr(bank, reg::INTTYPE0, 0xaaaaaaaa);
            wr(bank, reg::INTTYPE1, 0xcccccccc);
            input[bank].write(0xaaaaaaaa);
            settle();
            tick(4);
            wr(bank, reg::INTEN, 0xffffffff);
            input[bank].write(0x55555555);
            settle();
            tick(3);
            check(peek(bank, reg::INTSTATUS) == 0xcccccccc,
                  "mixed modes: edge conditions latch before level conditions");
            check(rd(bank, reg::INTSTATUS) == 0xcccccccc, "mixed-mode RC clears old edge vector");
            tick();
            check(peek(bank, reg::INTSTATUS) == 0x33333333,
                  "persistent high/low levels latch after edge vector is cleared");

            reset();
            input[bank].write(0xffffffff);
            settle();
            tick(4);
            wr(bank, reg::INTEN, 0x5a5aa5a5);
            tick();
            check(peek(bank, reg::INTSTATUS) == 0x5a5aa5a5,
                  "INTEN masks the captured event vector per pin");
            wr(bank, reg::INTEN, 0);
            rd(bank, reg::INTSTATUS);
            tick();
            check(!irq[bank].read(), "masked high inputs cannot reassert");

            reset();
            input[bank].write(0xffffffff);
            settle();
            tick(4);
            wr(bank, reg::INTTYPE0, 0xffffffff);
            wr(bank, reg::INTTYPE1, 0xffffffff);
            wr(bank, reg::INTEN, 0xffffffff);
            input[bank].write(0);
            settle();
            tick(3);
            check(peek(bank, reg::INTSTATUS) == 0xffffffff, "simultaneous falling edge vector");
            rd(bank, reg::INTSTATUS);
            input[bank].write(0xffffffff);
            settle();
            tick(4);
            check(!irq[bank].read(), "falling mode ignores opposite rising edges");
        }
    }
    void wake_priority() {
        for (unsigned bank = 0; bank < 3; ++bank) {
            for (unsigned high = 0; high < 32; ++high) {
                const U32 bit = U32{1} << high, lower = bit - 1;
                wr(bank, reg::POWEREVENT, bit | lower);
                pause();
                input[bank].write(lower);
                settle();
                check(!wake[bank].read(),
                      "all lower selected high pins lose to selected low highest pin");
                input[bank].write(bit);
                settle();
                check(wake[bank].read(), "highest selected pin drives wake with clock stopped");
                input[bank].write(0xffffffff);
                settle();
                check(wake[bank].read(), "unselected inputs do not affect selected high pin");
                input[bank].write(~(bit | lower));
                settle();
                check(!wake[bank].read(), "unselected high inputs cannot cause wake");
                check(!irq[bank].read(), "raw power event does not generate disabled IRQ");
                for (unsigned other = 0; other < 3; ++other)
                    if (other != bank)
                        check(!wake[other].read(), "raw wake bank isolation");
                resume();
            }
            wr(bank, reg::POWEREVENT, 0);
            input[bank].write(0xffffffff);
            settle();
            check(!wake[bank].read(), "zero select mask ignores every raw input");
        }
    }
    void clock_reset() {
        tick();
        // Accepted write cannot change output before its rising access edge.
        job.notify(SC_ZERO_TIME);
        wait(2, SC_NS);
        check(output[0].read() == 0 && peek(0, reg::PADOUT) == 0,
              "pending write has no pre-edge effect");
        wait(done);
        settle();
        check(output[0].read() == 0x76543210, "write published at rising edge");
        for (unsigned bank = 0; bank < 3; ++bank) {
            wr(bank, reg::PADOUT, 0xffffffff);
            wr(bank, reg::PADDIR, 0xffffffff);
            wr(bank, reg::INTEN, 0xffffffff);
            wr(bank, reg::POWEREVENT, 0xffffffff);
            for (unsigned word = 0; word < 8; ++word)
                wr(bank, reg::PADCFG0 + 4 * word, 0x3f3f3f3f);
            input[bank].write(0xffffffff);
        }
        settle();
        tick(4);
        pause();
        for (unsigned bank = 0; bank < 3; ++bank) {
            check(irq[bank].read() && wake[bank].read(),
                  "all banks pending before asynchronous reset");
            input[bank].write(0);
        }
        settle();
        wait(27, SC_NS);
        for (unsigned bank = 0; bank < 3; ++bank) {
            check(peek(bank, reg::PADIN) == 0xffffffff && synced[bank].read() == 0xffffffff,
                  "stopped clock holds sequential input state");
            check(irq[bank].read() && !wake[bank].read(),
                  "IRQ holds while raw wake changes without clock");
        }
        // IO_PAD continues routing while GPIO clock is stopped and reset is low.
        pins[0].write(true);
        pins[56].write(true);
        po[56].write(true);
        settle();
        job_status = tlm::TLM_GENERIC_ERROR_RESPONSE;
        job.notify(SC_ZERO_TIME);
        settle();
        reset_n.write(false);
        wait(done);
        settle();
        for (unsigned bank = 0; bank < 3; ++bank) {
            check(output[bank].read() == 0 && direction[bank].read() == 0 &&
                      synced[bank].read() == 0,
                  "reset clears all core ports without clock");
            check(!irq[bank].read() && !wake[bank].read(),
                  "reset clears every IRQ and power mask without clock");
            for (unsigned word = 0; word < 16; ++word)
                check(peek(bank, word * 4) == (word >= 8 ? reg::PADCFG_RESET : 0),
                      "complete reset image without clock");
            for (unsigned pin = 0; pin < 32; ++pin)
                check(cfg[bank * 32 + pin].read() == 2, "all 96 reset pad fields");
        }
        check(pi[0].read() && pi[56].read() && data[56].read(),
              "IO_PAD has no dependency on GPIO reset or clock");
        for (unsigned bank = 0; bank < 3; ++bank)
            input[bank].write(0xffffffff);
        wait(oscillator.negedge_event());
        resume();
        tick(3);
        for (unsigned bank = 0; bank < 3; ++bank)
            check(synced[bank].read() == 0 && peek(bank, reg::PADIN) == 0 && !irq[bank].read(),
                  "asserted reset holds pipeline clear across clock edges and high inputs");
        pause();
        reset_n.write(true);
        settle();
        wait(17, SC_NS);
        for (unsigned bank = 0; bank < 3; ++bank)
            check(peek(bank, reg::PADIN) == 0, "reset release alone does not advance sampling");
        // Resume from an oscillator-low phase to give an unambiguous first edge.
        wait(oscillator.negedge_event());
        resume();
        tick();
        for (unsigned bank = 0; bank < 3; ++bank)
            check(synced[bank].read() == 0 && peek(bank, reg::PADIN) == 0,
                  "pipeline cleared stage1");
        tick();
        for (unsigned bank = 0; bank < 3; ++bank)
            check(synced[bank].read() == 0xffffffff && peek(bank, reg::PADIN) == 0,
                  "pipeline cleared stage2");
        tick();
        for (unsigned bank = 0; bank < 3; ++bank)
            check(peek(bank, reg::PADIN) == 0xffffffff && !irq[bank].read(),
                  "pipeline refills stage3; INTEN remains reset");
    }
    // Independent expected membership from IO_PAD Table 7-2, PDF page 11.
    // No expected result is calculated by calling the implementation's pin_info.
    struct ExpectedPin {
        bool digital = false, input_only = false, mux = false, reserved = false;
    };
    std::array<ExpectedPin, 80> expected_pins() {
        std::array<ExpectedPin, 80> result{};
        for (unsigned p :
             {1u,  2u,  3u,  4u,  5u,  6u,  11u, 12u, 13u, 14u, 15u, 20u, 21u, 22u, 23u,
              24u, 25u, 26u, 27u, 28u, 29u, 30u, 44u, 45u, 46u, 47u, 54u, 55u, 56u, 57u,
              58u, 59u, 60u, 61u, 62u, 67u, 68u, 69u, 70u, 71u, 72u, 73u, 74u, 75u, 76u})
            result[p - 1].digital = true;
        for (unsigned p : {1u, 5u, 11u})
            result[p - 1].input_only = true;
        for (unsigned p :
             {6u, 12u, 13u, 14u, 15u, 20u, 21u, 22u, 25u, 26u, 27u, 28u, 29u, 46u, 47u})
            result[p - 1].mux = true;
        for (unsigned p : {23u, 24u})
            result[p - 1].reserved = true;
        return result;
    }
    void verify_pad(unsigned i, const ExpectedPin& info) {
        const bool mode = pins[4].read();
        const unsigned code = info.mux ? select[i].read().to_uint() : 0;
        const bool active = info.digital && !info.reserved && (!mode || i == 4 || i == 10);
        const bool primary = active && code == 0, alternate = active && code == 1;
        const bool out = primary ? po[i].read() : alternate ? ao[i].read() : false;
        const bool req = primary ? pe[i].read() : alternate ? ae[i].read() : false;
        const std::string at = " package " + std::to_string(i + 1);
        check(data[i].read() == (out && !info.input_only), "independent mux data" + at);
        check(enable[i].read() == (req && !info.input_only),
              "requested enable separate from data" + at);
        check(pi[i].read() == (primary && pins[i].read()), "selected primary receiver" + at);
        check(ai[i].read() == (alternate && pins[i].read()), "selected alternate receiver" + at);
    }
    void pad_truth() {
        const auto expected = expected_pins();
        pause();
        for (unsigned mode = 0; mode < 2; ++mode) {
            pins[4].write(mode);
            settle();
            for (unsigned i = 0; i < 80; ++i) {
                const auto info = expected[i],
                           actual =
                               ExpectedPin{pin_info(i + 1).digital, pin_info(i + 1).input_only,
                                           pin_info(i + 1).configurable, pin_info(i + 1).reserved};
                check(info.digital == actual.digital && info.input_only == actual.input_only &&
                          info.mux == actual.mux && info.reserved == actual.reserved,
                      "independent package membership");
                for (unsigned code = 0; code < 4; ++code)
                    for (unsigned bits = 0; bits < 32; ++bits) {
                        select[i].write(code);
                        po[i].write(bits & 1);
                        ao[i].write(bits & 2);
                        pe[i].write(bits & 4);
                        ae[i].write(bits & 8);
                        if (i != 4)
                            pins[i].write(bits & 16);
                        settle();
                        verify_pad(i, info);
                        check(test_mode.read() == bool(mode) &&
                                  test_select.read() == bool(mode && pins[10].read()),
                              "test decoder remains live during routing sweep");
                    }
                if (i != 4)
                    pins[i].write(false);
            }
        }
        // Explicit return from test mode with nonzero sources in both partitions.
        for (unsigned i = 0; i < 80; ++i) {
            select[i].write(0);
            po[i].write(true);
            pe[i].write(true);
            if (i != 4)
                pins[i].write(true);
        }
        pins[4].write(true);
        settle();
        for (unsigned i = 0; i < 80; ++i)
            verify_pad(i, expected[i]);
        pins[4].write(false);
        settle();
        for (unsigned i = 0; i < 80; ++i)
            verify_pad(i, expected[i]);
        check(pi[0].read() && pi[1].read() && pi[2].read() && pi[3].read() && pi[75].read(),
              "POR, boot/mode straps and oscillator recover without clock");
    }
    void pad_random() {
        const auto expected = expected_pins();
        pause();
        for (unsigned seed : {0x12345u, 0x6701u, 20261017u}) {
            std::mt19937 rng(seed);
            for (unsigned cycle = 0; cycle < 600; ++cycle) {
                for (unsigned i = 0; i < 80; ++i) {
                    const auto value = rng();
                    select[i].write(value & 3);
                    pins[i].write(value & 4);
                    po[i].write(value & 8);
                    ao[i].write(value & 16);
                    pe[i].write(value & 32);
                    ae[i].write(value & 64);
                }
                settle();
                for (unsigned i = 0; i < 80; ++i)
                    verify_pad(i, expected[i]);
                check(test_mode.read() == pins[4].read(), "random mode decoder");
                check(test_select.read() == (pins[4].read() && pins[10].read()),
                      "random select decoder");
            }
        }
    }
    void address_decode() {
        for (unsigned bank = 0; bank < 3; ++bank) {
            for (unsigned index = 0; index < 16; ++index)
                if (index != 1 && index != 6) {
                    // Unique values keep INTEN disabled and isolate read-only side effects.
                    if (index == 3)
                        continue;
                    const U32 value =
                        index >= 8 ? (0x01020304u + index) & reg::PADCFG_MASK : 0x10203040u + index;
                    wr(bank, index * 4, value);
                    for (unsigned low = 0; low < 4; ++low)
                        for (unsigned high : {0u, 0x40u, 0xfc0u, 0xffff0000u})
                            check(rd(bank, high + index * 4 + low) == value,
                                  "all low-bit/high-bit aliases preserve word decoding");
                }
            // A normal read through an unaligned/high-address status alias must clear.
            reset();
            wr(bank, reg::INTEN, 1);
            input[bank].write(1);
            settle();
            tick(4);
            check(rd(bank, 0xffdb) == 1, "INTSTATUS alias returns pending state");
            settle();
            check(!irq[bank].read() && peek(bank, reg::INTSTATUS) == 0,
                  "INTSTATUS alias clears on accepted read");
        }
    }
    void run() {
        clock_enable.write(true);
        reset();
        if (scenario == "register_outputs")
            register_outputs();
        else if (scenario == "input_stream")
            input_stream();
        else if (scenario == "irq_boundaries")
            irq_boundaries();
        else if (scenario == "wake_priority")
            wake_priority();
        else if (scenario == "clock_reset")
            clock_reset();
        else if (scenario == "pad_truth")
            pad_truth();
        else if (scenario == "pad_random")
            pad_random();
        else if (scenario == "address_decode")
            address_decode();
        else
            check(false, "unknown scenario");
        std::cout << scenario << ": " << checks << " checks, " << errors << " failures\n";
        sc_stop();
    }
};
int sc_main(int argc, char** argv) {
    SpecBench bench("spec", argc > 1 ? argv[1] : "irq_boundaries");
    sc_start();
    return bench.errors ? 1 : 0;
}
