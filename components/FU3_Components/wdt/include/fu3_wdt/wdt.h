#pragma once
#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>
#include <cstdint>
#include "fu3_wdt/registers.h"

namespace fu3_wdt {
class Watchdog : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<Watchdog> socket{"socket"};
    sc_core::sc_in<bool> preset_n{"preset_n"}, wdogres_n{"wdogres_n"};
    sc_core::sc_in<bool> clock_enabled{"clock_enabled"}, debug_halt{"debug_halt"};
    sc_core::sc_out<bool> irq{"irq"}, reset_request{"reset_request"}, paused{"paused"};
    SC_HAS_PROCESS(Watchdog);
    explicit Watchdog(sc_core::sc_module_name,
        sc_core::sc_time tick=sc_core::sc_time(10,sc_core::SC_NS),
        sc_core::sc_time access_latency=sc_core::sc_time(1,sc_core::SC_NS));
private:
    std::uint32_t load_=UINT32_MAX, count_=UINT32_MAX, control_=FU3_WDT_RESEN;
    bool locked_=false, raw_=false, seen_=false, running_=false, pause_=false;
    sc_core::sc_time tick_, latency_, last_tick_, pulse_until_;
    sc_core::sc_event changed_, outputs_;
    void synchronize();
    void timeout();
    void run();
    void publish();
    bool validate(tlm::tlm_generic_payload&);
    std::uint32_t read(unsigned) const;
    void write(unsigned,std::uint32_t,std::uint32_t);
    void transport(tlm::tlm_generic_payload&,sc_core::sc_time&);
    unsigned debug(tlm::tlm_generic_payload&);
    void read_bytes(tlm::tlm_generic_payload&);
};
} // namespace fu3_wdt
