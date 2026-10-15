// SPDX-License-Identifier: Apache-2.0
#pragma once
// Timed completions with generation safety (plan §4.3; HAS §8.3 "epoch-safe result").
// Every scheduled action belongs to a domain (e.g. init, training, power, a memory
// port). bump(domain) starts a new epoch: actions scheduled under the old epoch never
// run, so a reset or reinitialisation cannot be completed by a stale event. Each action
// runs at most once. Actions execute in SC_METHOD context and must not call wait().
#include <systemc>

#include <cstdint>
#include <functional>
#include <map>
#include <queue>
#include <vector>

namespace fx1::ddr3 {

class EpochScheduler : public sc_core::sc_module {
public:
    using Action = std::function<void()>;
    using Domain = unsigned;

    SC_HAS_PROCESS(EpochScheduler);
    explicit EpochScheduler(sc_core::sc_module_name name);

    // Runs `action` after `delay` unless the domain epoch changes first. Returns a
    // handle that can cancel this action alone.
    std::uint64_t schedule(const sc_core::sc_time& delay, Domain domain, Action action);
    void cancel(std::uint64_t handle);
    // Invalidates every pending action of the domain; returns the new epoch.
    std::uint32_t bump(Domain domain);
    std::uint32_t epoch(Domain domain) const;

    std::size_t pending() const { return live_.size(); }
    std::uint64_t executed() const { return executed_; }
    std::uint64_t discarded() const { return discarded_; }

private:
    struct Item {
        sc_core::sc_time when;
        std::uint64_t seq;
        Domain domain;
        std::uint32_t epoch;
    };
    struct Later {
        bool operator()(const Item& a, const Item& b) const {
            return a.when != b.when ? a.when > b.when : a.seq > b.seq;
        }
    };
    void run_due();
    void arm();

    std::priority_queue<Item, std::vector<Item>, Later> queue_;
    std::map<std::uint64_t, Action> live_;  // seq -> action; erased on run/cancel
    std::map<Domain, std::uint32_t> epochs_;
    sc_core::sc_event wake_;
    std::uint64_t next_seq_ = 1;
    std::uint64_t executed_ = 0;
    std::uint64_t discarded_ = 0;
};

} // namespace fx1::ddr3
