#include "regression_bench.h"

struct TimingRegression : Regression {
    struct Service {
        unsigned channel;
        sc_core::sc_time accepted, completed;
    };
    std::vector<Service> services;
    bool read = true, annotated = false, transition = false;
    unsigned duration_ns = 30;
    fu3::dma::BusPhase phase = fu3::dma::BusPhase::ReadData;

    TimingRegression(sc_core::sc_module_name name, const std::string& mode,
                     const fu3::dma::EngineConfig& config)
        : Regression(name, mode, 61443, config) {}

    void transport(tlm::tlm_generic_payload& t, sc_core::sc_time& delay) override {
        auto* meta = t.get_extension<fu3::dma::TransferExtension>();
        const auto accepted = sc_core::sc_time_stamp();
        Regression::transport(t, delay);
        if (scenario == "peripheral_recovery") {
            if ((t.is_read() && t.get_address() == 0x1e000) ||
                (t.is_write() && t.get_address() == 0x1f000)) {
                delay += sc_core::sc_time(3, sc_core::SC_NS);
                services.push_back({meta->channel, accepted, sc_core::sc_time_stamp() + delay});
            }
        } else if (t.is_read() == read) {
            if (scenario == "phase_boundary") meta->enter_phase(phase);
            const auto duration = sc_core::sc_time(duration_ns, sc_core::SC_NS);
            if (annotated) delay += duration;
            else wait(duration);
            if (transition) {
                // Preserve elapsed time even if the target leaves/re-enters a
                // phase before the scheduler runs at the same timestamp.
                auto next = phase;
                if (phase == fu3::dma::BusPhase::ReadAddress) next = fu3::dma::BusPhase::ReadData;
                if (phase == fu3::dma::BusPhase::WriteAddress) next = fu3::dma::BusPhase::WriteData;
                if (phase == fu3::dma::BusPhase::WriteData) next = fu3::dma::BusPhase::WriteResponse;
                meta->enter_phase(next);
            }
        }
    }

    void timer_case(unsigned wake_ns, unsigned timeout_bit) {
        reset(); configure(0, 0x1000, 0x8000, 4, 0, 4, 4);
        reg(true, 0x44, 1);
        if (wake_ns) {
            wait(wake_ns, sc_core::SC_NS);
            reg(true, 0xa8, 0x1fff); // Unrelated MMIO perturbs scheduler event order.
        }
        done(0);
        const bool expired = duration_ns > 30;
        check(reg(false, 0xa0) == (expired ? 1u << timeout_bit : 1u),
              "deadline result independent of MMIO wake order and target timing style");
        check(reg(false, 0x50) == (expired ? 0u : 0x10001u),
              "late response cannot generate successful command completion");
        check(reg(false, 0x3c) == 0 && irq.read(), "timed context drains and IRQ is asserted");
    }

    void timer_boundaries() {
        if (scenario == "transaction_boundary") {
            for (bool direction : {true, false}) for (bool annotation : {false, true})
                for (unsigned ns : {29u, 30u, 31u, 45u}) for (unsigned wake = 0; wake < 6; ++wake) {
                    read = direction; annotated = annotation; duration_ns = ns;
                    timer_case(wake, read ? 7 : 9);
                }
        } else {
            const unsigned bits[] = {8, 7, 11, 10, 9};
            for (unsigned index = 0; index < 5; ++index) for (unsigned style = 0; style < 3; ++style)
                for (unsigned ns : {29u, 30u, 31u, 45u}) for (unsigned wake = 0; wake < 6; ++wake) {
                    phase = static_cast<fu3::dma::BusPhase>(index); read = index < 2;
                    annotated = style == 1; transition = style == 2; duration_ns = ns;
                    timer_case(wake, bits[index]);
                }
        }
    }

    void peripheral_channel(unsigned channel, unsigned id, unsigned recovery) {
        configure(channel, read ? 0x1e000 : 0x1000 + channel * 256,
                  read ? 0x8000 + channel * 256 : 0x1f000, 1, 0, 1, 1);
        reg(true, channel * 256 + (read ? 0x10 : 0x14), 0x01010001);
        reg(true, channel * 256 + 0x20, (id | (recovery << 8)) << (read ? 0 : 16));
    }

    void request(unsigned id, bool external, bool asserted = true) {
        const unsigned bits = asserted ? 1u << id : 0;
        if (external) (read ? rx : tx).write(bits);
        else reg(true, read ? 0x1050 : 0x1054, bits);
    }

    sc_core::sc_time wait_clear(unsigned id) {
        unsigned limit = 0;
        while (!((read ? rc : tc).read().to_uint() & (1u << id)) && limit++ < 200)
            wait(1, sc_core::SC_NS);
        check(limit < 200, "peripheral clear arrives");
        return sc_core::sc_time_stamp();
    }

    void recovery_cases() {
        for (bool direction : {true, false}) for (unsigned recovery : {0u, 1u, 3u, 7u})
            for (bool external : {false, true}) for (bool restart : {false, true}) {
                read = direction; reset(); services.clear();
                peripheral_channel(0, 2, recovery);
                if (!restart) peripheral_channel(1, 2, 0);
                reg(true, 0x1048, restart ? 1 : 3); request(2, external);
                wait_clear(2); done(0);
                if (external) request(2, true, false);
                wait(1, sc_core::SC_NS);
                if (restart) { peripheral_channel(0, 2, 0); reg(true, 0x44, 1); }
                request(2, external);
                done(restart ? 0 : 1);
                check(services.size() == 2, "both peripheral credits are serviced");
                if (services.size() == 2) {
                    check(services[0].channel == 0 && services[1].channel == (restart ? 0u : 1u),
                          "shared endpoint services the intended channels");
                    const auto earliest = services[0].completed +
                        sc_core::sc_time(10 * std::max(1u, recovery), sc_core::SC_NS);
                    check(services[1].accepted >= earliest,
                          "endpoint recovery survives channel changes, starts and config writes");
                    check(services[1].accepted <= earliest + sc_core::sc_time(2, sc_core::SC_NS),
                          "scheduler wakes at recovery expiry without another request edge");
                }
            }

        // A different endpoint is usable while ID 2 recovers; reset clears the
        // old recovery deadline so ID 2 can be reused immediately afterwards.
        for (bool direction : {true, false}) {
            read = direction; reset(); services.clear();
            peripheral_channel(0, 2, 7); reg(true, 0x44, 1); request(2, false);
            const auto cleared = wait_clear(2); done(0);
            peripheral_channel(1, 3, 0); reg(true, 0x144, 1); request(3, false); done(1);
            check(services.size() == 2 && services[1].accepted < cleared + sc_core::sc_time(70, sc_core::SC_NS),
                  "recovery does not stall a different peripheral ID");
            reset(); peripheral_channel(0, 2, 0); reg(true, 0x44, 1); request(2, false); done(0);
            check(services.size() == 3 && services[2].accepted < cleared + sc_core::sc_time(70, sc_core::SC_NS),
                  "reset discards the old endpoint recovery deadline");
        }
    }

    void run() override {
        latency = 0;
        if (scenario == "peripheral_recovery") recovery_cases();
        else timer_boundaries();
        std::cout << scenario << ": checks=" << checks << " errors=" << errors << '\n';
        sc_core::sc_stop();
    }
};

int sc_main(int argc, char** argv) {
    if (argc != 2) return 2;
    sc_core::sc_set_time_resolution(1, sc_core::SC_NS);
    const std::string mode = argv[1];
    fu3::dma::EngineConfig config;
    if (mode == "transaction_boundary") config.transaction_timeout = sc_core::sc_time(30, sc_core::SC_NS);
    else if (mode == "phase_boundary")
        for (auto& timeout : config.phase_timeouts) timeout = sc_core::sc_time(30, sc_core::SC_NS);
    else if (mode != "peripheral_recovery") return 2;
    TimingRegression bench{"bench", mode, config}; sc_core::sc_start(); return bench.errors ? 1 : 0;
}
