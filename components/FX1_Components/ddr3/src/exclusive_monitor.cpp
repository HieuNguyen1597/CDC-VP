// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/exclusive_monitor.h"

namespace fx1::ddr3 {

bool ExclusiveMonitor::exclusive_read(unsigned port, unsigned id, std::uint64_t granule, std::uint32_t coverage,
                                      bool shape_ok) {
    if (entries_.empty()) return false;
    Entry* own = nullptr;
    for (Entry& e : entries_)
        if (e.valid && e.port == port && e.id == id) own = &e;
    if (!shape_ok) {
        if (own) own->valid = false;  // MAS shape-fail rule
        return false;
    }
    Entry* slot = own;
    if (!slot) {
        for (Entry& e : entries_) {
            if (!e.valid) {
                slot = &e;
                break;
            }
        }
    }
    if (!slot) {
        slot = &entries_[victim_];
        victim_ = (victim_ + 1u) % static_cast<unsigned>(entries_.size());
    }
    *slot = Entry{true, port, id, granule, coverage};
    return true;
}

bool ExclusiveMonitor::exclusive_write(unsigned port, unsigned id, std::uint64_t granule, std::uint32_t coverage,
                                       bool shape_ok) {
    if (!shape_ok) return false;
    for (const Entry& e : entries_) {
        if (e.valid && e.port == port && e.id == id && e.granule == granule && e.coverage == coverage) {
            write_taken(granule);
            return true;
        }
    }
    return false;
}

bool ExclusiveMonitor::exclusive_matches(unsigned port, unsigned id, std::uint64_t granule, std::uint32_t coverage,
                                         bool shape_ok) const {
    if (!shape_ok) return false;
    for (const Entry& e : entries_)
        if (e.valid && e.port == port && e.id == id && e.granule == granule && e.coverage == coverage) return true;
    return false;
}

void ExclusiveMonitor::write_taken(std::uint64_t granule) {
    for (Entry& e : entries_)
        if (e.valid && e.granule == granule) e.valid = false;
}

void ExclusiveMonitor::invalidate_port(unsigned port) {
    for (Entry& e : entries_)
        if (e.port == port) e.valid = false;
}

void ExclusiveMonitor::clear() {
    for (Entry& e : entries_) e.valid = false;
    victim_ = 0;
}

bool ExclusiveMonitor::port_owns(unsigned port) const {
    for (const Entry& e : entries_)
        if (e.valid && e.port == port) return true;
    return false;
}

} // namespace fx1::ddr3
