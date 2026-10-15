// SPDX-License-Identifier: Apache-2.0
#pragma once
// Exclusive-access monitor (HAS §6.1.12, T6-22; allocation policy and clearing rules
// MAS §6.1.12 / T6-54..T6-56, adopted where HAS is silent). Pure C++17.
//
// Reservation = {port, AXI ID, 32-byte normalized granule, byte coverage}; one entry per
// owner. Allocation: the owner's own entry, else the lowest invalid entry, else the
// round-robin victim. Coverage is derived from address, SIZE and LEN (C13, GAP-FE-24).
#include <cstdint>
#include <vector>

namespace fx1::ddr3 {

class ExclusiveMonitor {
public:
    struct Entry {
        bool valid = false;
        unsigned port = 0;
        unsigned id = 0;
        std::uint64_t granule = 0;
        std::uint32_t coverage = 0;
    };

    explicit ExclusiveMonitor(unsigned entries = 4) : entries_(entries) {}

    // Exclusive read. A single-granule access (`shape_ok`) creates or refreshes the owner's
    // reservation and returns true (EXOKAY). Otherwise the owner's entry is cleared and the
    // read returns false (OKAY, C12 / GAP-FE-03).
    bool exclusive_read(unsigned port, unsigned id, std::uint64_t granule, std::uint32_t coverage, bool shape_ok);
    // Exclusive write verdict: true when the owner's entry matches granule and coverage.
    // A pass is a taken write: it clears every reservation of the granule (MAS T6-55).
    bool exclusive_write(unsigned port, unsigned id, std::uint64_t granule, std::uint32_t coverage, bool shape_ok);
    // The same match without taking it: an exclusive write that emits no write atom (every
    // strobe zero) changes no reservation (MAS §6.1.12: only taken write atoms clear).
    bool exclusive_matches(unsigned port, unsigned id, std::uint64_t granule, std::uint32_t coverage,
                           bool shape_ok) const;
    // A taken ordinary write with at least one enabled byte in `granule` (FE-6.1.12-05).
    void write_taken(std::uint64_t granule);
    void invalidate_port(unsigned port);  // quiesce / reset of one port (FE-6.1.12-10)
    void clear();

    bool port_owns(unsigned port) const;
    unsigned victim() const noexcept { return victim_; }
    const std::vector<Entry>& entries() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
    unsigned victim_ = 0;
};

} // namespace fx1::ddr3
