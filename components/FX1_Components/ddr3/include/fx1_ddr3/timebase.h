// SPDX-License-Identifier: Apache-2.0
#pragma once
// Clock arithmetic of the model. DRAM timing registers count tCK whatever the DFI
// ratio; a wait of n tCK lasts ceil(n / R) controller cycles (MAS §5.5). Watchdogs and
// policy counters (READ_TIMEOUT, POWER_TIMEOUT, TRAIN_TIMEOUT, INIT_CMD_TIMEOUT) count
// controller (core) cycles. All values are integer picoseconds: no floating point and
// no dependency on SystemC, so the conversion rules are unit-testable.
#include <cstdint>

namespace fx1::ddr3 {

struct TimeBase {
    std::uint32_t tck_ps = 1250;  // DRAM clock period (DDR3-1600: 800 MHz)
    unsigned ratio = 2;           // controller clock = DRAM clock / ratio

    std::uint64_t ctrl_period_ps() const noexcept { return std::uint64_t{tck_ps} * ratio; }
    // Controller cycles needed to cover n tCK.
    std::uint64_t ctrl_cycles_for_tck(std::uint64_t n_tck) const noexcept {
        return (n_tck + ratio - 1u) / ratio;
    }
    // Duration of a tCK-counted wait, rounded up to whole controller cycles.
    std::uint64_t tck_wait_ps(std::uint64_t n_tck) const noexcept {
        return ctrl_cycles_for_tck(n_tck) * ctrl_period_ps();
    }
    std::uint64_t ctrl_cycles_ps(std::uint64_t cycles) const noexcept {
        return cycles * ctrl_period_ps();
    }
};

} // namespace fx1::ddr3
