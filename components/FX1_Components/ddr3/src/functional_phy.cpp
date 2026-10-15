// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/functional_phy.h"

#include <algorithm>
#include <initializer_list>

namespace fx1::ddr3 {

using sc_core::sc_time;
using sc_core::sc_time_stamp;

FunctionalPhy::FunctionalPhy(sc_core::sc_module_name name, const FunctionalPhyConfig& cfg)
    : sc_module(name), cfg_(cfg), status_word_(cfg.status_word) {
    SC_METHOD(levels_update);
    sensitive << levels_event_;
    dont_initialize();
    SC_METHOD(observation_due);
    sensitive << obs_event_;
    dont_initialize();
}

void FunctionalPhy::set_phy_reset_n(bool reset_n) {
    if (reset_n == reset_n_) return;
    reset_n_ = reset_n;
    if (!reset_n) {
        // Reset discards any outstanding load; its observation is never returned.
        pending_valid_ = false;
        outstanding_ = false;
        obs_event_.cancel();
        levels_event_.cancel();
        publish();
        return;
    }
    release_time_ = sc_time_stamp();
    levels_event_.notify(std::min(cfg_.reset_done_delay, cfg_.ready_delay));
}

void FunctionalPhy::levels_update() {
    publish();
    // Schedule the later of the two level edges if it is still ahead.
    const sc_time now = sc_time_stamp();
    for (const sc_time& d : {cfg_.reset_done_delay, cfg_.ready_delay}) {
        if (reset_n_ && release_time_ + d > now) {
            levels_event_.notify(release_time_ + d - now);
            break;
        }
    }
}

void FunctionalPhy::publish() {
    const sc_time now = sc_time_stamp();
    PhyLevels next;
    next.reset_done = reset_n_ && now >= release_time_ + cfg_.reset_done_delay;
    next.ready = next.reset_done && !forced_not_ready_ && now >= release_time_ + cfg_.ready_delay;
    next.timing_wait_ok = next.ready;
    next.failure = forced_failure_;
    const bool changed = next.reset_done != levels_.reset_done || next.ready != levels_.ready ||
                         next.failure != levels_.failure ||
                         next.timing_wait_ok != levels_.timing_wait_ok;
    levels_ = next;
    if (changed && client_) client_->phy_levels_changed(levels_);
}

void FunctionalPhy::force_failure(bool failure) {
    forced_failure_ = failure;
    publish();
}

void FunctionalPhy::force_ready(bool ready) {
    forced_not_ready_ = !ready;
    publish();
}

void FunctionalPhy::load(const PhyLoadRequest& req) {
    if (outstanding_) {
        SC_REPORT_ERROR(name(), "PHY load issued while another load is outstanding (BE09)");
        return;
    }
    ++loads_;
    last_ = req;
    const PhyLoadFault fault = load_faults_.empty() ? PhyLoadFault::None : load_faults_.front();
    if (!load_faults_.empty()) load_faults_.pop_front();
    if (!reset_n_) return;  // a PHY held in reset never answers
    outstanding_ = true;
    if (fault == PhyLoadFault::Lose) return;

    PhyObservation obs;
    obs.rank = req.rank;
    obs.epoch = req.epoch;
    obs.tag = req.tag;
    obs.success = fault != PhyLoadFault::Fail;
    // Deterministic, payload-derived observation so tests can check association.
    obs.rdlvl_resp = static_cast<std::uint8_t>(req.payload);
    obs.wrlvl_resp = static_cast<std::uint8_t>(req.payload >> 8);
    obs.rdlvl_mode = static_cast<std::uint8_t>(req.cls == PhyLoadClass::ReadLevel ? 1 : 0);
    obs.gate_mode = static_cast<std::uint8_t>(req.cls == PhyLoadClass::GateLevel ? 1 : 0);
    obs.wrlvl_mode = static_cast<std::uint8_t>(req.cls == PhyLoadClass::WriteLevel ? 1 : 0);
    if (fault == PhyLoadFault::StaleEpoch) obs.epoch = static_cast<std::uint16_t>(req.epoch - 1u);
    if (fault == PhyLoadFault::WrongRank) obs.rank = req.rank ^ 1u;
    if (fault == PhyLoadFault::WrongTag) obs.tag = static_cast<std::uint8_t>((req.tag + 1u) & 0xFu);
    pending_ = obs;
    pending_valid_ = true;
    obs_event_.notify(fault == PhyLoadFault::Delay ? cfg_.delayed_latency : cfg_.load_latency);
}

void FunctionalPhy::observation_due() {
    if (!pending_valid_) return;
    pending_valid_ = false;
    outstanding_ = false;
    if (client_) client_->phy_observation(pending_);
}

} // namespace fx1::ddr3
