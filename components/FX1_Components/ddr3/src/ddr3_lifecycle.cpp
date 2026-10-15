// SPDX-License-Identifier: Apache-2.0
// Lifecycle (HAS §9): initialization, boot/runtime training, startup-scrub timing,
// readiness composition, refresh debt and the P-channel power lifecycle.
#include "fx1_ddr3/ddr3_controller.h"

#include "fx1_ddr3/fx1_ddr3_regs.h"

#include <algorithm>
#include <stdexcept>

namespace fx1::ddr3 {

using sc_core::sc_time;

namespace {
bool in_progress(unsigned stage) {
    return stage != FX1_DDR3_INIT_STAGE_HOLD && stage != FX1_DDR3_INIT_STAGE_DONE &&
           stage != FX1_DDR3_INIT_STAGE_FAILED;
}
} // namespace

// ---------------------------------------------------------------- initialization (HAS §9.1.1)
void Ddr3Controller::init_restart() {
    // BE05/LC-9.1.1-14: a restart reruns the complete sequence; nothing partial survives.
    // Scrub writes accepted before the restart stay in DRAM (no rollback).
    scrub_sync();
    storage_changed();
    scheduler_.bump(kDomainInit);
    scheduler_.bump(kDomainTrain);
    scheduler_.bump(kDomainScrub);
    scheduler_.bump(kDomainRefresh0);
    scheduler_.bump(kDomainRefresh0 + 1);
    ++train_epoch_;
    // Any training episode (boot or runtime) is retired without a callback; a runtime
    // TRAIN_CMD waiting for it observes the core-epoch change and ends with SLVERR.
    training_abort();
    training_busy_ = false;
    runtime_training_ = false;
    init_stage_ = FX1_DDR3_INIT_STAGE_HOLD;
    init_done_ = init_failed_ = false;
    init_waiting_level_ = false;
    training_complete_ = training_error_ = false;
    scrub_complete_ = scrub_running_ = false;
    scrub_total_ = 0;
    admission_opened_ = false;
    phy_failure_ = false;
    refresh_running_ = false;
    for (auto& r : refresh_) {
        const bool fault = r.fault_blocked;  // host fault injection survives reinitialization
        r = RefreshRank{};
        r.fault_blocked = fault;
    }
    if (init_complete_) {
        init_complete_ = false;
        request_outputs();
    }
    phy_->set_init_complete(false);
    phy_->set_clock_disable(false);  // init, training and traffic run with the clock enabled
    update_readiness();
}

bool Ddr3Controller::init_rw_locked() const {
    // Initialization lock (GAP-LC-06): from leaving HOLD until DONE/FAILED, and while a
    // runtime training command uses the PHY delay words.
    return in_progress(init_stage_) || runtime_training_;
}

void Ddr3Controller::init_step() {
    if (cold_reset_ || core_reset_) return;
    if (init_stage_ == FX1_DDR3_INIT_STAGE_HOLD) {
        // LC-9.1.1-02: leave hold only with valid topology, timing and DFI images.
        if (images_valid()) init_enter(FX1_DDR3_INIT_STAGE_PHY_RESET);
        return;
    }
    if (init_stage_ == FX1_DDR3_INIT_STAGE_PHY_READY_WAIT) {
        if (phy_levels_.failure) {
            init_fail(init_stage_, false);
        } else if (phy_levels_.reset_done && phy_levels_.ready) {
            init_wait(FX1_DDR3_INIT_STAGE_RESET_RELEASE_WAIT, cycles(storage(FX1_DDR3_INIT_RESET_RELEASE_WAIT)),
                      FX1_DDR3_INIT_STAGE_CKE_LOW_WAIT);
        }
        return;
    }
    if (init_waiting_level_ && phy_levels_.timing_wait_ok) {
        init_waiting_level_ = false;
        init_enter(init_wait_next_);
    }
}

void Ddr3Controller::init_enter(unsigned stage) {
    init_stage_ = stage;
    const unsigned ranks = std::max(1u, geom_.ranks);
    switch (stage) {
    case FX1_DDR3_INIT_STAGE_PHY_RESET:
        // Fixed PHY-reset wait of four core cycles (HAS §8.1); INIT_RESET_ASSERT_WAIT is
        // readback only.
        phy_->set_phy_reset_n(false);
        scheduler_.schedule(cycles(kPhyResetCycles), kDomainInit, [this] {
            phy_->set_phy_reset_n(true);
            init_enter(FX1_DDR3_INIT_STAGE_PHY_READY_WAIT);
        });
        break;
    case FX1_DDR3_INIT_STAGE_PHY_READY_WAIT: init_step(); break;
    case FX1_DDR3_INIT_STAGE_CKE_LOW_WAIT:
        init_wait(stage, cycles(storage(FX1_DDR3_INIT_CKE_LOW_WAIT)), FX1_DDR3_INIT_STAGE_CKE_HIGH_WAIT);
        break;
    case FX1_DDR3_INIT_STAGE_CKE_HIGH_WAIT:
        init_wait(stage, cycles(storage(FX1_DDR3_INIT_CKE_HIGH_WAIT)), FX1_DDR3_INIT_STAGE_MR2);
        break;
    // MRS per active rank: tMRD between mode-register sets, tMOD after the last one.
    case FX1_DDR3_INIT_STAGE_MR2: init_wait(stage, tck_wait(ranks * ext_.t_mrd), FX1_DDR3_INIT_STAGE_MR3); break;
    case FX1_DDR3_INIT_STAGE_MR3: init_wait(stage, tck_wait(ranks * ext_.t_mrd), FX1_DDR3_INIT_STAGE_MR1); break;
    case FX1_DDR3_INIT_STAGE_MR1: init_wait(stage, tck_wait(ranks * ext_.t_mrd), FX1_DDR3_INIT_STAGE_MR0); break;
    case FX1_DDR3_INIT_STAGE_MR0: init_wait(stage, tck_wait(ranks * ext_.t_mod), FX1_DDR3_INIT_STAGE_DLL_WAIT); break;
    case FX1_DDR3_INIT_STAGE_DLL_WAIT:
        init_wait(stage, cycles(storage(FX1_DDR3_INIT_DLL_WAIT)), FX1_DDR3_INIT_STAGE_ZQCL);
        break;
    case FX1_DDR3_INIT_STAGE_ZQCL: init_wait(stage, tck_wait(ranks), FX1_DDR3_INIT_STAGE_ZQ_WAIT); break;
    case FX1_DDR3_INIT_STAGE_ZQ_WAIT:  // ranks are calibrated one at a time (MAS §5.6)
        init_wait(stage, cycles(std::uint64_t{ranks} * storage(FX1_DDR3_INIT_ZQ_WAIT)), FX1_DDR3_INIT_STAGE_TRAINING);
        break;
    case FX1_DDR3_INIT_STAGE_TRAINING: {
        // DEC-06: boot training loads the INIT_RW delay words per active rank with tag 0.
        // The result applies only to the initialization attempt that started it.
        const std::uint32_t attempt = scheduler_.epoch(kDomainInit);
        training_start(boot_training_loads(), [this, attempt](bool ok, std::uint8_t) {
            if (attempt != scheduler_.epoch(kDomainInit) || init_stage_ != FX1_DDR3_INIT_STAGE_TRAINING) return;
            if (ok) init_done();
            else init_fail(FX1_DDR3_INIT_STAGE_TRAINING, true);
        });
        break;
    }
    default: break;
    }
}

void Ddr3Controller::init_wait(unsigned stage, sc_time duration, unsigned next) {
    // stage_done = (time >= stage_start + configured_wait) ∧ required_handshake; the PHY
    // timing_wait_ok level gates each timed transition (LC-9.1.1-04, GAP-LC-05).
    init_stage_ = stage;
    scheduler_.schedule(duration, kDomainInit, [this, next] {
        if (phy_levels_.failure) {
            init_fail(init_stage_, false);
        } else if (!phy_levels_.timing_wait_ok) {
            init_waiting_level_ = true;
            init_wait_next_ = next;
        } else {
            init_enter(next);
        }
    });
}

void Ddr3Controller::init_fail(unsigned stage, bool training) {
    scheduler_.bump(kDomainInit);
    training_abort();  // a failed attempt keeps no live training work (S2-R3)
    init_failed_ = true;
    init_done_ = false;
    init_stage_ = FX1_DDR3_INIT_STAGE_FAILED;
    training_error_ = training;
    sticky_set(FX1_DDR3_INIT_ERROR, FX1_DDR3_INIT_ERROR_STAGE(stage));  // one-hot failing stage
    irq_set(6);
    if (training) irq_set(7);
    update_readiness();
}

void Ddr3Controller::init_done() {
    init_done_ = true;
    init_stage_ = FX1_DDR3_INIT_STAGE_DONE;
    training_complete_ = true;
    init_complete_ = true;  // BE08: after successful DRAM initialization and training
    request_outputs();
    phy_->set_init_complete(true);
    topo_status_ &= ~FX1_DDR3_COMMIT_ST_REINIT;  // GAP-CSR-07
    timing_status_ &= ~FX1_DDR3_COMMIT_ST_REINIT;
    refresh_restart();
    if (!cfg_.ecc_enable) scrub_complete_ = true;  // LC-9.1-04
    update_readiness();
}

// ---------------------------------------------------------------- training (HAS §9.1.2)
std::vector<PhyLoadRequest> Ddr3Controller::boot_training_loads() const {
    std::vector<PhyLoadRequest> loads;
    const std::uint32_t wrlvl = (1u << (storage(FX1_DDR3_PHY_WRLVL_DELAY) & 7u)) - 1u;  // BE18 thermometer
    for (unsigned rank = 0; rank < std::max(1u, geom_.ranks); ++rank) {
        const std::pair<PhyLoadClass, std::uint32_t> items[] = {
            {PhyLoadClass::ReadLevel, storage(FX1_DDR3_PHY_RDLVL_DELAY) & 0xFFFFFFu},
            {PhyLoadClass::GateLevel, storage(FX1_DDR3_PHY_GATE_DELAY) & 0xFFFFFFu},
            {PhyLoadClass::WriteLevel, wrlvl}};
        for (const auto& it : items) {
            PhyLoadRequest r;
            r.rank = rank;
            r.cls = it.first;
            r.payload = it.second;
            r.skew0 = storage(FX1_DDR3_PHY_SKEW_CTRL0);
            r.skew1 = storage(FX1_DDR3_PHY_SKEW_CTRL1);
            r.epoch = train_epoch_;
            r.tag = 0;  // boot convention (LC-9.1.2-09)
            loads.push_back(r);
        }
    }
    return loads;
}

void Ddr3Controller::training_start(std::vector<PhyLoadRequest> loads, std::function<void(bool, std::uint8_t)> done) {
    train_episode_ = ++train_episode_seq_;
    train_loads_ = std::move(loads);
    train_next_ = 0;
    train_done_ = std::move(done);
    training_issue_next();
}

void Ddr3Controller::training_issue_next() {
    if (train_next_ >= train_loads_.size()) {
        training_finish(true, FX1_DDR3_TRAIN_CODE_OK);
        return;
    }
    if (!phy_levels_.ready) {
        sticky_set(FX1_DDR3_PHY_BRIDGE_ERROR, FX1_DDR3_PHY_BRIDGE_ERR_NOT_READY);
        training_finish(false, FX1_DDR3_TRAIN_CODE_PHY_FAIL);
        return;
    }
    // One acknowledged PHY operation at a time; the payload stays stable until the
    // observation (BE09). The timeout counts controller cycles (TRAIN_TIMEOUT).
    scheduler_.bump(kDomainTrain);  // retire the timeout of the previous load
    train_pending_ = train_loads_[train_next_];
    train_load_pending_ = true;
    phy_->load(train_pending_);
    scheduler_.schedule(cycles(storage(FX1_DDR3_TRAIN_TIMEOUT)), kDomainTrain, [this] {
        if (!train_load_pending_) return;
        sticky_set(FX1_DDR3_PHY_BRIDGE_ERROR, FX1_DDR3_PHY_BRIDGE_ERR_TIMEOUT);
        training_finish(false, FX1_DDR3_TRAIN_CODE_TIMEOUT);
    });
}

void Ddr3Controller::training_finish(bool ok, std::uint8_t code) {
    if (!train_episode_) return;  // one terminal result per episode
    auto cb = std::move(train_done_);
    training_abort();
    if (cb) cb(ok, code);
}

void Ddr3Controller::training_abort() {
    // Retire the active episode: pending timeout, outstanding load identity and callback.
    // A late observation then finds no pending load and is discarded as stale.
    scheduler_.bump(kDomainTrain);
    train_load_pending_ = false;
    train_done_ = nullptr;
    train_episode_ = 0;
}

void Ddr3Controller::phy_observation(const PhyObservation& obs) {
    if (!train_load_pending_ || obs.epoch != train_pending_.epoch) {
        // No outstanding load, or a result of an older episode: discarded as stale; it
        // cannot complete a newer episode (HAS §8.3 epoch-safe result).
        sticky_set(FX1_DDR3_PHY_BRIDGE_ERROR, FX1_DDR3_PHY_BRIDGE_ERR_STALE);
        return;
    }
    train_obs_ = (std::uint32_t{obs.rdlvl_resp} << FX1_DDR3_TRAIN_OBS_RDLVL_SHIFT) |
                 (std::uint32_t{obs.wrlvl_resp} << FX1_DDR3_TRAIN_OBS_WRLVL_SHIFT) |
                 (std::uint32_t{obs.rdlvl_mode & 3u} << FX1_DDR3_TRAIN_OBS_RDMODE_SHIFT) |
                 (std::uint32_t{obs.gate_mode & 3u} << FX1_DDR3_TRAIN_OBS_GATEMODE_SHIFT) |
                 (std::uint32_t{obs.wrlvl_mode & 3u} << FX1_DDR3_TRAIN_OBS_WRMODE_SHIFT) |
                 (std::uint32_t{obs.rank & 1u} << FX1_DDR3_TRAIN_OBS_RANK_SHIFT);
    if (obs.tag != train_pending_.tag || obs.rank != train_pending_.rank) {
        // Identity mismatch on the current transaction fails the episode (BE09).
        sticky_set(FX1_DDR3_PHY_BRIDGE_ERROR, FX1_DDR3_PHY_BRIDGE_ERR_STALE);
        training_finish(false, FX1_DDR3_TRAIN_CODE_STALE);
        return;
    }
    train_load_pending_ = false;
    if (!obs.success) {
        sticky_set(FX1_DDR3_PHY_BRIDGE_ERROR, FX1_DDR3_PHY_BRIDGE_ERR_PROVIDER_FAIL);
        training_finish(false, FX1_DDR3_TRAIN_CODE_PHY_FAIL);
        return;
    }
    rank_training_context_[obs.rank & 1u] = obs;  // only the selected rank's context
    ++train_next_;
    training_issue_next();
}

void Ddr3Controller::phy_levels_changed(const PhyLevels& levels) {
    const bool failure_rise = levels.failure && !phy_levels_.failure;
    const bool ready_lost = phy_levels_.ready && !levels.ready;
    phy_levels_ = levels;
    if (failure_rise) {
        irq_set(9);           // PHY alert/failure (GAP-CSR-10: rising edge)
        phy_failure_ = true;  // explicit failure is sticky until reinit/reset (BE07)
        // Failure and ready are independent inputs: failure alone terminates the active
        // training episode (boot or runtime) with a provider failure (S2-R3).
        if (train_episode_) {
            sticky_set(FX1_DDR3_PHY_BRIDGE_ERROR, FX1_DDR3_PHY_BRIDGE_ERR_PROVIDER_FAIL);
            training_finish(false, FX1_DDR3_TRAIN_CODE_PHY_FAIL);
        }
        if (in_progress(init_stage_)) init_fail(init_stage_, false);
    }
    if (ready_lost && train_load_pending_) {
        sticky_set(FX1_DDR3_PHY_BRIDGE_ERROR, FX1_DDR3_PHY_BRIDGE_ERR_NOT_READY);
        training_finish(false, FX1_DDR3_TRAIN_CODE_PHY_FAIL);
    }
    if (power_wait_phy_ && levels.ready) {
        power_wait_phy_ = false;
        power_complete(true);
    }
    init_step();
    update_readiness();
}

// ---------------------------------------------------------------- startup scrub (BE03)
void Ddr3Controller::scrub_start() {
    // N_total = U + U/8 full-mask zero writes; only accepted writes advance. One BL8 write
    // occupies 4 tCK; the duration is evaluated analytically (DEC-10). The data effect is a
    // frontier: the writes passed so far are applied to the store whenever it is accessed
    // (scrub_sync), by dropping resident pages, never by allocating zero pages.
    scrub_total_ = geom_.usable_granules + geom_.usable_granules / 8;
    scrub_write_time_ = tck_wait(4);
    scrub_start_ = sc_core::sc_time_stamp();
    scrub_running_ = true;
    storage_changed();
    scrub_applied_ = 0;  // a restart begins again at granule 0 (RS05)
    scrub_ecc_base_ = geom_.ecc_base;
    scheduler_.bump(kDomainScrub);
    const sc_time total = scrub_write_time_ * static_cast<double>(scrub_total_);
    scheduler_.schedule(total, kDomainScrub, [this] {
        scrub_materialize(scrub_total_);  // last group accepted, then DRAIN and complete
        scrub_running_ = false;
        scrub_complete_ = true;
        update_readiness();
    });
    if (scrub_fault_armed_) {
        // Injected mapping fault: the sweep fails after its first write (BE03 scrub fatal).
        scrub_fault_armed_ = false;
        scheduler_.schedule(scrub_write_time_, kDomainScrub, [this] { fault_fatal(FatalFault::ScrubMapping); });
    }
}

void Ddr3Controller::scrub_abort() {
    // Loss of an ordinary prerequisite re-arms the sweep; it restarts from granule 0. The
    // writes already accepted are not rolled back.
    scrub_sync();
    scheduler_.bump(kDomainScrub);
    scrub_running_ = false;
}

std::uint64_t Ddr3Controller::scrub_progress() const {
    if (scrub_complete_) return scrub_total_;
    if (!scrub_running_ || scrub_write_time_ == sc_core::SC_ZERO_TIME) return 0;
    const double done = (sc_core::sc_time_stamp() - scrub_start_) / scrub_write_time_;
    return std::min<std::uint64_t>(scrub_total_, static_cast<std::uint64_t>(done));
}

// ---------------------------------------------------------------- readiness (HAS §9.1, §9.4)
void Ddr3Controller::update_readiness() {
    const bool power_allows = power_resident_ == FX1_DDR3_PSTATE_ACTIVE && !power_transition_;
    const bool phy_ok = phy_levels_.ready && !phy_failure_;
    // Scrub runs only with every prerequisite present; losing one re-arms it (BE03).
    const bool scrub_prereq = cfg_.ecc_enable && init_done_ && !init_failed_ && phy_ok && power_allows &&
                              !training_busy_ && !fatal_ && topo_valid_;
    if (scrub_running_ && !scrub_prereq) scrub_abort();
    if (!scrub_running_ && !scrub_complete_ && scrub_prereq) scrub_start();
    const bool ready = phy_ok && init_done_ && !init_failed_ && training_complete_ && !training_busy_ &&
                       !training_error_ && scrub_complete_ && power_allows && !fatal_ && !recovery_required_;
    if (ready != mission_ready_) {
        mission_ready_ = ready;
        if (ready) admission_opened_ = true;
        request_outputs();
        notify_lifecycle();
    }
}

// ---------------------------------------------------------------- refresh (HAS §9.2, BE04)
unsigned Ddr3Controller::refresh_limit() const {
    return std::max(1u, std::min(8u, storage(FX1_DDR3_REFRESH_CFG) & 0xFu));
}

void Ddr3Controller::refresh_restart() {
    refresh_running_ = true;
    for (unsigned r = 0; r < 2; ++r) {
        const bool fault = refresh_[r].fault_blocked;
        refresh_[r] = RefreshRank{};
        refresh_[r].grid_origin = sc_core::sc_time_stamp();
        refresh_[r].fault_blocked = fault;
        refresh_set_blocked(r);
    }
}

int Ddr3Controller::refresh_debt_at(unsigned rank, const sc_time& t) const {
    if (!refresh_running_ || rank >= geom_.ranks || !timing_valid_) return 0;
    const RefreshRank& r = refresh_[rank];
    const int limit = static_cast<int>(refresh_limit());
    if (r.held) return r.held_debt;
    if (r.blocked) {
        const sc_time interval = tck_wait(timing_active_[kTRefi]);
        const auto k_now = static_cast<long long>((t - r.grid_origin) / interval);
        const auto k_blk = static_cast<long long>((r.blocked_since - r.grid_origin) / interval);
        return static_cast<int>(std::min<long long>(limit, r.debt_at_block + (k_now - k_blk)));
    }
    if (r.repaying) {
        // Each accepted REF retires one interval of debt after tRFC.
        const sc_time trfc = tck_wait(timing_active_[kTRfc]);
        const auto paid = static_cast<long long>((t - r.repay_since) / trfc);
        return static_cast<int>(std::max<long long>(0, r.debt_at_repay - paid));
    }
    return 0;
}

int Ddr3Controller::refresh_debt(unsigned rank) const { return refresh_debt_at(rank, sc_core::sc_time_stamp()); }

void Ddr3Controller::refresh_set_blocked(unsigned rank) {
    RefreshRank& r = refresh_[rank];
    const bool want = r.fault_blocked || r.pd_blocked;
    if (want == r.blocked) return;
    const sc_time now = sc_core::sc_time_stamp();
    const int debt = refresh_debt_at(rank, now);
    if (want) {
        r.blocked = true;
        r.repaying = false;
        r.blocked_since = now;
        r.debt_at_block = debt;
    } else {
        r.blocked = false;
        r.repaying = debt > 0;
        r.repay_since = now;
        r.debt_at_repay = debt;
    }
    refresh_reschedule(rank);
}

void Ddr3Controller::refresh_hold(bool hold) {
    // Self-refresh holds ordinary debt; a full interval restarts after a legal exit.
    const sc_time now = sc_core::sc_time_stamp();
    for (unsigned rank = 0; rank < 2; ++rank) {
        RefreshRank& r = refresh_[rank];
        if (hold) {
            r.held_debt = refresh_debt_at(rank, now);
            r.held = true;
        } else if (r.held) {
            r.held = false;
            r.grid_origin = now;
            r.blocked_since = now;
            r.debt_at_block = r.held_debt;
            r.repaying = !r.blocked && r.held_debt > 0;
            r.repay_since = now;
            r.debt_at_repay = r.held_debt;
        }
        refresh_reschedule(rank);
    }
}

void Ddr3Controller::refresh_reschedule(unsigned rank) {
    // Debt is evaluated lazily. Two points need an event while a rank is blocked: reaching
    // high urgency (PACTIVE may rise, GAP-LC-22) and the first miss beyond the limit, which
    // is a refresh emergency (IRQ_RAW[5], GAP-LC-18).
    scheduler_.bump(kDomainRefresh0 + rank);
    RefreshRank& r = refresh_[rank];
    if (!refresh_running_ || rank >= geom_.ranks || !timing_valid_ || !r.blocked || r.held || r.saturated) return;
    const int limit = static_cast<int>(refresh_limit());
    const int high = (limit + 1) >> 1;  // BE04
    const sc_time interval = tck_wait(timing_active_[kTRefi]);
    const auto k_blk = static_cast<long long>((r.blocked_since - r.grid_origin) / interval);
    const sc_time now = sc_core::sc_time_stamp();
    auto at_miss = [&](long long misses) {
        const sc_time when = r.grid_origin + interval * static_cast<double>(k_blk + misses);
        return when > now ? when - now : sc_core::SC_ZERO_TIME;
    };
    if (r.debt_at_block < high)
        scheduler_.schedule(at_miss(high - r.debt_at_block), kDomainRefresh0 + rank, [this] { pchannel_update_pactive(); });
    scheduler_.schedule(at_miss(limit - r.debt_at_block + 1), kDomainRefresh0 + rank, [this, rank] {
        refresh_[rank].saturated = true;
        irq_set(5);
        pchannel_update_pactive();
    });
}

std::uint32_t Ddr3Controller::refresh_rank_word(unsigned rank) const {
    const int limit = static_cast<int>(refresh_limit());
    const int d = refresh_debt(rank);
    const int high = (limit + 1) >> 1;  // BE04
    const unsigned urgency = d >= limit  ? FX1_DDR3_REFRESH_URG_MANDATORY
                             : d >= high ? FX1_DDR3_REFRESH_URG_HIGH
                             : d >= 1    ? FX1_DDR3_REFRESH_URG_NORMAL
                                         : FX1_DDR3_REFRESH_URG_BACKGROUND;
    return (static_cast<std::uint32_t>(d) & 0x1Fu) | (urgency << 5);
}

std::uint32_t Ddr3Controller::refresh_status_word() const {
    std::uint32_t v = 0;
    for (unsigned rank = 0; rank < 2; ++rank) {
        const unsigned urgency = (refresh_rank_word(rank) >> 5) & 3u;
        if (urgency == FX1_DDR3_REFRESH_URG_MANDATORY) {
            v |= rank == 0 ? FX1_DDR3_REFRESH_STATUS_R0_MANDATORY : FX1_DDR3_REFRESH_STATUS_R1_MANDATORY;
            v |= FX1_DDR3_REFRESH_STATUS_ANY_MANDATORY;
        }
        if (urgency >= FX1_DDR3_REFRESH_URG_HIGH) v |= FX1_DDR3_REFRESH_STATUS_ANY_HIGH;
        if (refresh_[rank].saturated) v |= FX1_DDR3_REFRESH_STATUS_SATURATION;
    }
    return v;
}

void Ddr3Controller::fault_block_refresh(unsigned rank, bool blocked) {
    if (rank >= 2) throw std::out_of_range("fault_block_refresh: rank must be 0 or 1");
    refresh_[rank].fault_blocked = blocked;
    refresh_set_blocked(rank);
}

// ---------------------------------------------------------------- power (HAS §9.3, RD-PWR-01)
void Ddr3Controller::pchannel_request(bool preq, std::uint8_t pstate) {
    const bool rising = preq && !pch_.preq;
    const bool falling = !preq && pch_.preq;
    pch_.preq = preq;
    pch_.pstate = static_cast<std::uint8_t>(pstate & 3u);
    if (falling) {
        // 4-phase: the held response drops after PREQ is low.
        if (pch_.paccept || pch_.pdeny) {
            pch_.paccept = pch_.pdeny = false;
            if (sc_core::sc_is_running()) pchannel_event_.notify(sc_core::SC_ZERO_TIME);
        }
        return;
    }
    if (!rising) return;
    if (pch_.paccept || pch_.pdeny || power_transition_) {
        SC_REPORT_WARNING(name(), "P-channel request before both responses are low (RD-PWR-01); ignored");
        return;
    }
    power_evaluate_request();
}

void Ddr3Controller::power_evaluate_request() {
    const std::uint8_t target = pch_.pstate;
    auto deny = [&](std::uint32_t err) {
        sticky_set(FX1_DDR3_POWER_ERROR, err);
        power_last_deny_ = true;
        power_last_accept_ = false;
        pch_.pdeny = true;
        if (sc_core::sc_is_running()) pchannel_event_.notify(sc_core::SC_ZERO_TIME);
    };
    if (target == power_resident_) {  // same state: accepted as a no-op (GAP-LC-23)
        power_last_accept_ = true;
        power_last_deny_ = false;
        pch_.paccept = true;
        if (sc_core::sc_is_running()) pchannel_event_.notify(sc_core::SC_ZERO_TIME);
        return;
    }
    // LC-9.3-01/07: checked against ownership, maintenance and the mutex before any step.
    if (!init_done_ || init_failed_ || recovery_required_ || training_busy_ || scrub_running_ || mutex_busy())
        return deny(FX1_DDR3_POWER_ERR_DENIED_BUSY);
    if (power_resident_ != FX1_DDR3_PSTATE_ACTIVE && target != FX1_DDR3_PSTATE_ACTIVE)
        return deny(FX1_DDR3_POWER_ERR_DENIED_ILLEGAL);  // power-down <-> self-refresh via active only
    power_mutex_token_ = mutex_acquire(MutexOwner::Power);  // serialized by the mutex (LC-9.3-10)
    power_transition_ = true;
    power_cutoff_passed_ = false;
    power_wait_phy_ = false;
    power_from_ = static_cast<std::uint8_t>(power_resident_);
    power_target_ = target;
    update_readiness();  // admission closes for the transition
    update_lifecycle();
    const std::uint64_t token = ++power_token_;
    scheduler_.schedule(cycles(storage(FX1_DDR3_POWER_TIMEOUT)), kDomainPower,
                        [this, token] { power_timeout_check(token); });
    if (target != FX1_DDR3_PSTATE_ACTIVE) power_run_entry(target);
    else power_run_exit();
}

void Ddr3Controller::power_run_entry(std::uint8_t target) {
    // Drain the backend (readiness is already closed, so accepted transactions wait at the
    // backend gate), prepare (precharge, and for self-refresh one refresh), then launch the
    // irreversible CKE/entry step.
    if (core_owns_work()) {
        power_drain_pending_ = true;  // resumed by core_work_done()
        return;
    }
    // Power stop acknowledged (admission stopped, backend drained): the exclusive
    // reservations of every port are invalidated (MAS §6.1.12 i_port_invalidate, S3-R4).
    for (unsigned p = 0; p < kPortBundles; ++p) excl_.invalidate_port(p);
    sc_time prepare = tck_wait(timing_active_[kTRp]);
    if (target == FX1_DDR3_PSTATE_SELF_REFRESH) prepare += tck_wait(timing_active_[kTRfc]);
    const std::uint64_t token = power_token_;
    scheduler_.schedule(prepare, kDomainPower, [this, target, token] {
        if (token != power_token_ || !power_transition_) return;
        power_cutoff_passed_ = true;
        sc_time enter = tck_wait(ext_.t_cke);
        if (target == FX1_DDR3_PSTATE_SELF_REFRESH)
            enter = tck_wait(std::max<std::uint64_t>(storage(FX1_DDR3_T_CKSRE), 1) + ext_.t_ckesr);
        scheduler_.schedule(enter, kDomainPower, [this, target, token] {
            if (token != power_token_ || !power_transition_) return;
            power_resident_ = target;
            // Precharge power-down and self-refresh close every row (BE-6.2.6-03).
            if (target != FX1_DDR3_PSTATE_ACTIVE_POWER_DOWN) bank_.close_all();
            if (target == FX1_DDR3_PSTATE_SELF_REFRESH) {
                // The clock may stop only after a completed self-refresh entry (HAS Table 1-4
                // clock_disable, MAS: the DRAM clock stops only in self-refresh).
                phy_->set_clock_disable(true);
                refresh_hold(true);
            } else {
                for (unsigned r = 0; r < 2; ++r) {
                    refresh_[r].pd_blocked = true;  // power-down: refresh needs a wake
                    refresh_set_blocked(r);
                }
            }
            power_complete(true);
        });
    });
}

void Ddr3Controller::power_run_exit() {
    // Exit is irreversible from its first step. Wake timing: tXP (APD), tXPDLL (PPD),
    // tCKSRX + tXSDLL + ZQCL (SR); then wait for PHY ready before reopening admission.
    power_cutoff_passed_ = true;
    phy_->set_clock_disable(false);  // the clock runs again before the exit timing starts
    sc_time wake = tck_wait(ext_.t_xp);
    if (power_from_ == FX1_DDR3_PSTATE_PRECHARGE_POWER_DOWN) wake = tck_wait(ext_.t_xpdll);
    if (power_from_ == FX1_DDR3_PSTATE_SELF_REFRESH)
        wake = tck_wait(std::max<std::uint64_t>(storage(FX1_DDR3_T_CKSRX), 1) + ext_.t_xsdll + ext_.t_zqoper);
    const std::uint64_t token = power_token_;
    scheduler_.schedule(wake, kDomainPower, [this, token] {
        if (token != power_token_ || !power_transition_) return;
        if (phy_levels_.ready) power_complete(true);
        else power_wait_phy_ = true;  // completes on the PHY level change, or times out
    });
}

void Ddr3Controller::power_complete(bool accept) {
    if (accept && power_target_ == FX1_DDR3_PSTATE_ACTIVE) {
        if (power_from_ == FX1_DDR3_PSTATE_SELF_REFRESH) refresh_hold(false);
        for (unsigned r = 0; r < 2; ++r) {
            refresh_[r].pd_blocked = false;
            refresh_set_blocked(r);
        }
        power_resident_ = FX1_DDR3_PSTATE_ACTIVE;
    }
    ++power_token_;  // retire the timeout and any pending step
    power_transition_ = false;
    power_drain_pending_ = false;
    power_wait_phy_ = false;
    mutex_release(power_mutex_token_);
    power_mutex_token_ = 0;
    power_last_accept_ = accept;
    power_last_deny_ = !accept;
    pch_.paccept = accept;
    pch_.pdeny = !accept;
    update_readiness();
    update_lifecycle();
    pchannel_update_pactive();
    if (sc_core::sc_is_running()) pchannel_event_.notify(sc_core::SC_ZERO_TIME);
}

void Ddr3Controller::power_timeout_check(std::uint64_t token) {
    if (token != power_token_ || !power_transition_) return;
    irq_set(8);  // power transition failure
    if (!power_cutoff_passed_) {
        // Before the irreversible step: deny and return to the active path.
        sticky_set(FX1_DDR3_POWER_ERROR, FX1_DDR3_POWER_ERR_TIMEOUT_PRE_CUTOFF);
    } else {
        // After it: recovery required, admission stays closed (LC-9.3-03).
        std::uint32_t err = FX1_DDR3_POWER_ERR_TIMEOUT_POST_CUTOFF;
        if (power_wait_phy_) err |= FX1_DDR3_POWER_ERR_WAKE_PHY_NOT_READY;
        sticky_set(FX1_DDR3_POWER_ERROR, err);
        recovery_required_ = true;
    }
    power_complete(false);
}

void Ddr3Controller::pchannel_update_pactive() {
    // PACTIVE: the controller needs the active state: pending memory requests or refresh
    // urgency high while resident in a low-power state (GAP-LC-22). Reset value 1.
    bool active = true;
    if (power_resident_ != FX1_DDR3_PSTATE_ACTIVE) {
        const bool refresh_high = (refresh_status_word() & FX1_DDR3_REFRESH_STATUS_ANY_HIGH) != 0;
        active = waiting_callers_ > 0 || refresh_high;
    }
    if (active != pch_.pactive) {
        pch_.pactive = active;
        if (sc_core::sc_is_running()) pchannel_event_.notify(sc_core::SC_ZERO_TIME);
    }
}

} // namespace fx1::ddr3
