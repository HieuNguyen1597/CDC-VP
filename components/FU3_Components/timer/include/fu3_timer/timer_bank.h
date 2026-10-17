#pragma once
#include "fu3_timer/timer.h"
#include <array>
#include <memory>

namespace fu3_timer {
// Four independent single-instance IPs, not a packed TIMER_CNT=4 aperture.
class TimerBank : public sc_core::sc_module {
public:
    std::array<std::unique_ptr<Timer>,4> timers;
    static constexpr std::array<std::uint32_t,4> bases={FU3_TIMER0_BASE,FU3_TIMER1_BASE,FU3_TIMER2_BASE,FU3_TIMER3_BASE};
    explicit TimerBank(sc_core::sc_module_name name,
        std::array<sc_core::sc_time,4> periods={sc_core::sc_time(10,sc_core::SC_NS),sc_core::sc_time(10,sc_core::SC_NS),sc_core::sc_time(10,sc_core::SC_NS),sc_core::sc_time(10,sc_core::SC_NS)}):sc_module(name) {
        for(unsigned i=0;i<4;++i)timers[i]=std::make_unique<Timer>(("TIMER"+std::to_string(i)).c_str(),periods[i]);
    }
};
} // namespace fu3_timer
