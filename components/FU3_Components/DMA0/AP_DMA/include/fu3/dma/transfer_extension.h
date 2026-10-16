#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <systemc>
#include <tlm>

namespace fu3::dma {
// Optional bus-adapter contract. Ordinary RAM targets can ignore this extension.
// Targets which observe AXI phases call enter_phase before waiting in that phase.
enum class BusPhase : unsigned { ReadAddress, ReadData, WriteAddress, WriteData, WriteResponse };
enum BusFault : std::uint32_t {
    NoFault = 0, FifoOverflow = 1u << 0, FifoUnderflow = 1u << 1,
    ReadLastError = 1u << 2, WriteCreditError = 1u << 3,
    WriteStrobeError = 1u << 4, MultipleFifoReturns = 1u << 5
};
struct TransferExtension : tlm::tlm_extension<TransferExtension> {
    unsigned channel = 0, unit_bytes = 1, beats = 1, axi_id = 4;
    unsigned cache = 0, prot = 0, beat_index = 0, logical_beats = 1;
    bool increment = true, command_fetch = false;
    std::vector<unsigned char> write_strobes;
    // Optional observations supplied by an AXI adapter, never fabricated by RAM.
    bool response_id_valid = false;
    unsigned response_id = 4;
    std::uint32_t faults = NoFault;
    BusPhase phase = BusPhase::ReadData;
    sc_core::sc_time phase_started = sc_core::SC_ZERO_TIME;
    sc_core::sc_event* phase_changed = nullptr;
    void enter_phase(BusPhase next) {
        phase = next;
        phase_started = sc_core::sc_time_stamp();
        if (phase_changed) phase_changed->notify(sc_core::SC_ZERO_TIME);
    }
    tlm::tlm_extension_base* clone() const override {
        auto* result = new TransferExtension(*this);
        result->phase_changed = nullptr; // Clones do not retain a live engine callback.
        return result;
    }
    void copy_from(const tlm::tlm_extension_base& other) override {
        auto* callback = phase_changed;
        *this = static_cast<const TransferExtension&>(other);
        phase_changed = callback;
    }
};
}
