#pragma once
#include <cstdint>
#include <tlm>

namespace fu3::qspi {
// One CS frame may span multiple bounded chunks. No serial pin waveform is
// implied. Targets must annotate delay and must not wait inside b_transport.
struct FlashTransaction : tlm::tlm_extension<FlashTransaction> {
    std::uint64_t frame=0, offset=0, length=0;
    std::uint32_t address=0, alternate=0;
    std::uint8_t instruction=0;
    unsigned instruction_lines=0, address_lines=0, alternate_lines=0, data_lines=0;
    unsigned address_bytes=0, alternate_bytes=0, dummy_cycles=0;
    bool begin=false, end=false, cancel=false, ddr=false, clock_mode3=false;
    tlm_extension_base* clone() const override { return new FlashTransaction(*this); }
    void copy_from(const tlm_extension_base& e) override {
        *this=static_cast<const FlashTransaction&>(e);
    }
};
}
