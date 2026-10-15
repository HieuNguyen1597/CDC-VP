// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/epoch_scheduler.h"

namespace fx1::ddr3 {

using sc_core::sc_time;
using sc_core::sc_time_stamp;

EpochScheduler::EpochScheduler(sc_core::sc_module_name name) : sc_module(name) {
    SC_METHOD(run_due);
    sensitive << wake_;
    dont_initialize();
}

std::uint64_t EpochScheduler::schedule(const sc_time& delay, Domain domain, Action action) {
    const std::uint64_t seq = next_seq_++;
    queue_.push({sc_time_stamp() + delay, seq, domain, epoch(domain)});
    live_.emplace(seq, std::move(action));
    arm();
    return seq;
}

void EpochScheduler::cancel(std::uint64_t handle) {
    if (live_.erase(handle)) ++discarded_;
}

std::uint32_t EpochScheduler::bump(Domain domain) {
    return ++epochs_[domain];
}

std::uint32_t EpochScheduler::epoch(Domain domain) const {
    const auto it = epochs_.find(domain);
    return it == epochs_.end() ? 0u : it->second;
}

void EpochScheduler::arm() {
    // Drop cancelled heads so the wake-up time is that of a live action.
    while (!queue_.empty() && !live_.count(queue_.top().seq)) queue_.pop();
    if (queue_.empty()) return;
    const sc_time now = sc_time_stamp();
    const sc_time when = queue_.top().when;
    // notify() keeps the earlier of two pending notifications.
    wake_.notify(when > now ? when - now : sc_core::SC_ZERO_TIME);
}

void EpochScheduler::run_due() {
    const sc_time now = sc_time_stamp();
    while (!queue_.empty() && queue_.top().when <= now) {
        const Item item = queue_.top();
        queue_.pop();
        auto it = live_.find(item.seq);
        if (it == live_.end()) continue;  // cancelled
        Action action = std::move(it->second);
        live_.erase(it);
        if (item.epoch != epoch(item.domain)) {
            ++discarded_;
            continue;
        }
        ++executed_;
        action();  // may schedule more work
    }
    arm();
}

} // namespace fx1::ddr3
