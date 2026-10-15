// SPDX-License-Identifier: Apache-2.0
#pragma once
// Default functional PHY provider with a reproducible fault script. It models only the
// handshake semantics the controller and firmware can observe: readiness levels after
// PHY reset release, one acknowledged load at a time, and the returned observation.
#include "fx1_ddr3/phy_provider.h"

#include <deque>

namespace fx1::ddr3 {

enum class PhyLoadFault {
    None,        // acknowledged, success
    Fail,        // acknowledged, success = 0
    Lose,        // no observation is ever returned (controller timeout path)
    StaleEpoch,  // observation carries epoch - 1
    WrongRank,   // observation carries the other rank
    WrongTag,    // observation carries tag + 1
    Delay,       // success, but after FunctionalPhyConfig::delayed_latency
};

struct FunctionalPhyConfig {
    sc_core::sc_time reset_done_delay{100, sc_core::SC_NS};  // after phy reset_n release
    sc_core::sc_time ready_delay{200, sc_core::SC_NS};       // after phy reset_n release
    sc_core::sc_time load_latency{50, sc_core::SC_NS};
    sc_core::sc_time delayed_latency{10, sc_core::SC_US};
    // Opaque provider words returned unchanged (no layout is defined by the VP, SOQ-12).
    std::uint32_t cap_word = 0;
    std::uint32_t status_word = 0;
};

class FunctionalPhy : public sc_core::sc_module, public PhyProvider {
public:
    SC_HAS_PROCESS(FunctionalPhy);
    FunctionalPhy(sc_core::sc_module_name name, const FunctionalPhyConfig& cfg = {});
    // Observation response/mode values are provider-defined (BE09); this provider derives
    // them from the load payload and class so association can be checked.

    // PhyProvider
    void attach(PhyClient* client) override { client_ = client; }
    PhyLevels levels() const override { return levels_; }
    void set_phy_reset_n(bool reset_n) override;
    void set_clock_disable(bool disable) override { clock_disabled_ = disable; }
    void set_init_complete(bool complete) override { init_complete_ = complete; }
    void load(const PhyLoadRequest& req) override;
    std::uint32_t provider_cap_word() const override { return cfg_.cap_word; }
    std::uint32_t provider_status_word() const override { return status_word_; }
    std::uint16_t comp_nctl() const override { return comp_nctl_; }
    std::uint16_t comp_pctl() const override { return comp_pctl_; }
    std::uint8_t comp_rxctl() const override { return comp_rxctl_; }

    // Host-side fault and observation control (testbench / platform CLI only).
    void script_load_faults(std::deque<PhyLoadFault> faults) { load_faults_ = std::move(faults); }
    void force_failure(bool failure);          // BE07 failure level (sticky in controller)
    void force_ready(bool ready);              // readiness loss / restore
    void set_status_word(std::uint32_t word) { status_word_ = word; }
    void set_compensation(std::uint16_t n, std::uint16_t p, std::uint8_t rx) {
        comp_nctl_ = n; comp_pctl_ = p; comp_rxctl_ = rx;
    }

    // Host-side observability (benches only; not a CSR encoding).
    bool phy_reset_n() const { return reset_n_; }
    bool clock_disabled() const { return clock_disabled_; }
    bool init_complete() const { return init_complete_; }
    unsigned loads_received() const { return loads_; }
    const PhyLoadRequest& last_load() const { return last_; }
    bool load_outstanding() const { return outstanding_; }

private:
    void levels_update();
    void observation_due();
    void publish();

    FunctionalPhyConfig cfg_;
    PhyClient* client_ = nullptr;
    PhyLevels levels_{};
    bool reset_n_ = false;
    bool clock_disabled_ = false;
    bool init_complete_ = false;
    bool forced_failure_ = false;
    bool forced_not_ready_ = false;
    std::uint32_t status_word_ = 0;
    std::uint16_t comp_nctl_ = 0, comp_pctl_ = 0;
    std::uint8_t comp_rxctl_ = 0;
    std::deque<PhyLoadFault> load_faults_;
    sc_core::sc_time release_time_;
    sc_core::sc_event levels_event_;
    sc_core::sc_event obs_event_;
    PhyObservation pending_{};
    bool pending_valid_ = false;
    bool outstanding_ = false;
    PhyLoadRequest last_{};
    unsigned loads_ = 0;
};

} // namespace fx1::ddr3
