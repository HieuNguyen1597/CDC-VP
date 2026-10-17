#pragma once
#include <array>
#include <cstdint>
#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

namespace fu3::gpio {
namespace reg {
constexpr unsigned PADDIR = 0x00, PADIN = 0x04, PADOUT = 0x08, INTEN = 0x0c, INTTYPE0 = 0x10,
                   INTTYPE1 = 0x14, INTSTATUS = 0x18, POWEREVENT = 0x1c, PADCFG0 = 0x20;
constexpr std::uint32_t PADCFG_MASK = 0x3f3f3f3f, PADCFG_RESET = 0x02020202;
} // namespace reg
// Clocked APB functional target. Addresses are local; decode is PADDR[5:2].
class Gpio : public sc_core::sc_module {
  public:
    tlm_utils::simple_target_socket<Gpio> socket{"socket"};
    sc_core::sc_in<bool> clk{"clk"}, reset_n{"reset_n"};
    sc_core::sc_in<sc_dt::sc_uint<32>> gpio_in{"gpio_in"};
    sc_core::sc_out<sc_dt::sc_uint<32>> gpio_out{"gpio_out"}, gpio_dir{"gpio_dir"},
        gpio_in_sync{"gpio_in_sync"};
    sc_core::sc_vector<sc_core::sc_out<sc_dt::sc_uint<6>>> gpio_padcfg{"gpio_padcfg", 32};
    sc_core::sc_out<bool> interrupt{"interrupt"}, power_event{"power_event"};
    SC_HAS_PROCESS(Gpio);
    explicit Gpio(sc_core::sc_module_name name);

  private:
    std::array<std::uint32_t, 16> words_{};
    std::uint32_t sync0_ = 0, sync1_ = 0, sample_ = 0;
    bool irq_ = false;
    std::uint64_t reset_generation_ = 0;
    tlm::tlm_generic_payload* pending_ = nullptr;
    sc_core::sc_mutex bus_lock_;
    sc_core::sc_event completed_, changed_;
    void tick();
    void outputs();
    void b_transport(tlm::tlm_generic_payload&, sc_core::sc_time&);
    unsigned transport_dbg(tlm::tlm_generic_payload&);
    bool validate(tlm::tlm_generic_payload&);
    void transfer(tlm::tlm_generic_payload&, bool side_effect);
};
} // namespace fu3::gpio
