// SPDX-License-Identifier: Apache-2.0
// AXI4-Lite CSR boundary (HAS §8): decode and access policy, live policy validation,
// INIT_RW lock, command mutex, commits with acknowledged install, snapshots, interrupts.
#include "fx1_ddr3/ddr3_controller.h"

#include "fx1_ddr3/fx1_ddr3_regs.h"

#include <algorithm>
#include <cstring>

namespace fx1::ddr3 {

using sc_core::sc_time;

namespace {
void store_le32(unsigned char* p, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
std::uint32_t load_le32(const unsigned char* p) {
    return std::uint32_t{p[0]} | std::uint32_t{p[1]} << 8 | std::uint32_t{p[2]} << 16 | std::uint32_t{p[3]} << 24;
}
std::uint32_t lane_mask(std::uint8_t strobes) {
    std::uint32_t m = 0;
    for (unsigned b = 0; b < 4; ++b)
        if ((strobes >> b) & 1u) m |= 0xFFu << (8 * b);
    return m;
}
bool is_timing_active(std::uint16_t off) {
    return off >= FX1_DDR3_TIMING_ACTIVE(0) && off <= FX1_DDR3_TIMING_ACTIVE(FX1_DDR3_TIMING_WORDS - 1);
}
bool is_region_active(std::uint16_t off) {
    return off >= FX1_DDR3_FE_REGION_ACTIVE(0) && off <= FX1_DDR3_FE_REGION_ACTIVE(31);
}
} // namespace

// ---------------------------------------------------------------- transport
void Ddr3Controller::csr_b_transport(tlm::tlm_generic_payload& tx, sc_time& delay) {
    // Malformed payloads are initiator/integration errors, not AXI4-Lite responses: reject
    // them before touching any buffer.
    if (!tx.is_read() && !tx.is_write()) {
        tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        return;
    }
    unsigned char* data = tx.get_data_ptr();
    if (!data) {
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    if (tx.get_byte_enable_ptr() && tx.get_byte_enable_length() != 4) {
        tx.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
        return;
    }
    if (delay != sc_core::SC_ZERO_TIME) {  // one owner per delay
        sc_core::wait(delay);
        delay = sc_core::SC_ZERO_TIME;
    }
    if (cold_reset_ || csr_reset_) {
        // The AXI4-Lite domain is held in reset: an access is an integration error.
        SC_REPORT_WARNING(name(), "CSR access while the CSR domain is in reset");
        if (tx.is_read()) std::memset(data, 0, tx.get_data_length());
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    const std::uint64_t addr = tx.get_address();
    if (tx.get_data_length() != 4 || tx.get_streaming_width() < 4 || (addr & 3u) != 0 ||
        addr >= kCsrApertureBytes) {
        // Misaligned or non-word access to a 32-bit AXI4-Lite port (HAS §8.2: SLVERR).
        if (tx.is_read()) std::memset(data, 0, tx.get_data_length());
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    Resp r;
    if (tx.is_read()) {
        std::uint32_t value = 0;
        r = csr_read(static_cast<std::uint32_t>(addr), value, false);
        store_le32(data, r == Resp::Okay ? value : 0u);  // error reads return zero data
    } else {
        std::uint8_t strobes = 0xF;
        if (const unsigned char* be = tx.get_byte_enable_ptr()) {
            strobes = 0;
            for (unsigned b = 0; b < 4; ++b)
                if (be[b] == TLM_BYTE_ENABLED) strobes = static_cast<std::uint8_t>(strobes | (1u << b));
        }
        r = csr_write(static_cast<std::uint32_t>(addr), load_le32(data), strobes);
    }
    // The access is decided at entry; the response returns one controller cycle later, so
    // successive accesses of one master never share a cycle (HAS §8.5 set-vs-clear applies
    // to genuinely simultaneous events).
    sc_core::wait(cycles(kCsrAccessCycles));
    tx.set_response_status(r == Resp::Okay ? tlm::TLM_OK_RESPONSE
                           : r == Resp::DecErr ? tlm::TLM_ADDRESS_ERROR_RESPONSE
                                               : tlm::TLM_GENERIC_ERROR_RESPONSE);
}

unsigned Ddr3Controller::csr_transport_dbg(tlm::tlm_generic_payload& tx) {
    // Side-effect-free word reads only (debuggers, tests); writes are not supported.
    unsigned char* data = tx.get_data_ptr();
    const std::uint64_t addr = tx.get_address();
    if (!tx.is_read() || !data || tx.get_data_length() < 4 || (addr & 3u) || addr >= kCsrApertureBytes)
        return 0;
    std::uint32_t value = 0;
    if (csr_read(static_cast<std::uint32_t>(addr), value, true) != Resp::Okay) value = 0;
    store_le32(data, value);
    return 4;
}

// ---------------------------------------------------------------- read side
std::uint32_t Ddr3Controller::storage(std::uint16_t offset) const {
    const auto it = storage_.find(offset);
    return it == storage_.end() ? 0u : it->second;
}

void Ddr3Controller::set_storage(std::uint16_t offset, std::uint32_t value) { storage_[offset] = value; }

Ddr3Controller::Resp Ddr3Controller::csr_read(std::uint32_t offset, std::uint32_t& value, bool debug) {
    (void)debug;  // reads have no side effects in this map
    value = 0;
    const CsrDesc* d = csr_find(offset);
    if (!d) return Resp::DecErr;
    const auto off = d->offset;
    switch (d->source) {
    case CsrSource::Storage:
        value = storage(off);
        if (off == FX1_DDR3_CE_THRESHOLD && !cfg_.ecc_enable) value = 0;  // optional ECC word
        break;
    case CsrSource::Constant:
        if (off == FX1_DDR3_CAP_FEATURE) value = cap_feature(cfg_);
        else if (off == FX1_DDR3_CAP_WIDTH0) value = cap_width0(cfg_);
        else if (off == FX1_DDR3_PHY_CAP) value = phy_->provider_cap_word();
        else value = d->reset;
        break;
    case CsrSource::Live: value = live_value(off); break;
    case CsrSource::Snapshot: {
        const auto it = snapshot_.find(off);
        value = snapshot_valid_ && it != snapshot_.end() ? it->second : 0u;
        break;
    }
    case CsrSource::Zero: value = 0; break;
    }
    return Resp::Okay;
}

std::uint32_t Ddr3Controller::live_value(std::uint16_t off) const {
    if (is_region_active(off)) return region_valid_ ? region_active_[(off - FX1_DDR3_FE_REGION_ACTIVE(0)) / 4] : 0u;
    if (is_timing_active(off)) return timing_valid_ ? timing_active_[(off - FX1_DDR3_TIMING_ACTIVE(0)) / 4] : 0u;
    switch (off) {
    case FX1_DDR3_QUIESCE_STATUS: {
        // A software stop holds new transactions at the ports; it is acknowledged once the
        // frontend has drained (MAS Table 6-73). Complete = stopped and nothing owned.
        const bool backend_idle = !core_owns_work();
        const bool stopped = global_quiesce_req_ || (storage(FX1_DDR3_GLOBAL_CTRL) & FX1_DDR3_GLOBAL_CTRL_ADM_STOP);
        std::uint32_t v = 0;
        if (global_quiesce_req_) v |= FX1_DDR3_QUIESCE_ST_REQUEST;
        if (stopped && fe_drained_) v |= FX1_DDR3_QUIESCE_ST_ADM_STOPPED;
        if (global_idle_) v |= FX1_DDR3_QUIESCE_ST_GLOBAL_IDLE;
        if (backend_idle) v |= FX1_DDR3_QUIESCE_ST_MISSION_IDLE | FX1_DDR3_QUIESCE_ST_DFI_EMPTY;
        if (global_quiesce_req_ && global_idle_) v |= FX1_DDR3_QUIESCE_ST_COMPLETE;
        return v;  // [6] timeout reads 0: no finite timeout is exposed (RD-CSR-02)
    }
    case FX1_DDR3_SNAPSHOT_STATUS:
        return (snapshot_busy_ ? FX1_DDR3_SNAPSHOT_ST_BUSY : 0u) | (snapshot_valid_ ? FX1_DDR3_SNAPSHOT_ST_VALID : 0u) |
               (std::uint32_t{snapshot_seq_} << 16);
    case FX1_DDR3_FE_PORT_CMD_STATUS: return fe_port_status_;
    case FX1_DDR3_FE_PORT_STATE: return fe_port_state_;
    case FX1_DDR3_FE_REGION_STATUS: return region_status_ | (region_valid_ ? FX1_DDR3_FE_REGION_ST_VALID : 0u);
    case FX1_DDR3_TOPO_ACTIVE: return topo_valid_ ? topo_active_ : 0u;
    case FX1_DDR3_TOPO_COMMIT_STATUS: return topo_status_;
    case FX1_DDR3_TIMING_COMMIT_STATUS: return timing_status_;
    case FX1_DDR3_DFI_TIMING_STATUS: return dfi_status_;
    case FX1_DDR3_ECC_CAP_STATUS: {
        // [0], [12:9], [20:13] are capability constants; [1] RMW busy and [8:2] lock
        // occupancy are snapshot fields (HAS Table 8-16 "snapshot", "coherent"): they read 0
        // before the first capture. ECC absent: the word reads 0.
        if (!cfg_.ecc_enable) return 0;
        const auto it = snapshot_.find(FX1_DDR3_ECC_CAP_STATUS);
        const std::uint32_t dyn = snapshot_valid_ && it != snapshot_.end() ? it->second : 0u;
        return FX1_DDR3_ECC_CAP_PRESENT | dyn | (4u << 9) | (8u << 13);
    }
    case FX1_DDR3_QUARANTINE_STATUS:
        // MAS o_quarantine_status: [1:0] = 11 while active, [4:2] reason, [15:8] read
        // contexts still open (GAP-BE-15).
        return quarantine_active_ ? (FX1_DDR3_QUAR_ST_ACTIVE | (quarantine_reason_ << 2) |
                                     (std::min(open_reads_, 255u) << 8))
                                  : 0u;
    case FX1_DDR3_QUARANTINE_FIRST_REQ_LO: return quarantine_lo_;
    case FX1_DDR3_QUARANTINE_FIRST_REQ_HI: return quarantine_hi_;
    case FX1_DDR3_FATAL_STATUS: return fatal_status_;
    case FX1_DDR3_IRQ_RAW: return irq_raw_;
    case FX1_DDR3_IRQ_STATUS: return irq_raw_ & storage(FX1_DDR3_IRQ_MASK) & FX1_DDR3_IRQ_ALL;
    default: return 0;
    }
}

std::uint32_t Ddr3Controller::snapshot_source(std::uint16_t off) const {
    const bool geo = topo_valid_;
    switch (off) {
    case FX1_DDR3_USABLE_GRANULES_LO: return geo ? static_cast<std::uint32_t>(geom_.usable_granules) : 0u;
    case FX1_DDR3_USABLE_GRANULES_HI: return geo ? static_cast<std::uint32_t>(geom_.usable_granules >> 32) : 0u;
    case FX1_DDR3_PHYSICAL_GRANULES_LO: return geo ? static_cast<std::uint32_t>(geom_.physical_granules) : 0u;
    case FX1_DDR3_PHYSICAL_GRANULES_HI: return geo ? static_cast<std::uint32_t>(geom_.physical_granules >> 32) : 0u;
    case FX1_DDR3_ECC_BASE_LO: return geo ? static_cast<std::uint32_t>(geom_.ecc_base) : 0u;
    case FX1_DDR3_ECC_BASE_HI: return geo ? static_cast<std::uint32_t>(geom_.ecc_base >> 32) : 0u;
    case FX1_DDR3_REFRESH_RANK(0): return refresh_rank_word(0);
    case FX1_DDR3_REFRESH_RANK(1): return refresh_rank_word(1);
    case FX1_DDR3_REFRESH_STATUS: return refresh_status_word();
    case FX1_DDR3_INIT_STATUS: {
        const bool stop = storage(FX1_DDR3_GLOBAL_CTRL) & FX1_DDR3_GLOBAL_CTRL_ADM_STOP;
        const bool block = !mission_ready_ || stop || fatal_;
        return (init_done_ ? FX1_DDR3_INIT_ST_DRAM_INIT : 0u) | (init_failed_ ? FX1_DDR3_INIT_ST_FAILED : 0u) |
               ((init_stage_ & 0xFu) << 7) | (block ? FX1_DDR3_INIT_ST_ADM_BLOCK : 0u);
    }
    case FX1_DDR3_POWER_STATUS:
        return (power_resident_ & 3u) | (power_transition_ ? FX1_DDR3_POWER_ST_ACTIVE : 0u) |
               (power_last_accept_ ? FX1_DDR3_POWER_ST_ACCEPT : 0u) | (power_last_deny_ ? FX1_DDR3_POWER_ST_DENY : 0u) |
               (recovery_required_ ? FX1_DDR3_POWER_ST_FATAL_RECOVERY : 0u);
    case FX1_DDR3_PHY_STATUS:
        return (phy_levels_.reset_done ? FX1_DDR3_PHY_STATUS_RESET_DONE : 0u) |
               (phy_levels_.ready ? FX1_DDR3_PHY_STATUS_READY : 0u) |
               (phy_levels_.failure ? FX1_DDR3_PHY_STATUS_FAILURE : 0u) |
               (phy_levels_.timing_wait_ok ? FX1_DDR3_PHY_STATUS_TIMING_WAIT_OK : 0u);
    case FX1_DDR3_TRAIN_STATUS: return train_status_;
    case FX1_DDR3_TRAIN_OBSERVATION0: return train_obs_;
    case FX1_DDR3_PHY_COMP_NCTL: return phy_->comp_nctl();
    case FX1_DDR3_PHY_COMP_PCTL: return phy_->comp_pctl();
    case FX1_DDR3_PHY_COMP_RXCTL: return phy_->comp_rxctl() & 0x7Fu;
    // ECC snapshot words read 0 without ECC (HAS Table 8-16): the counters never move then.
    case FX1_DDR3_CE_COUNT: return ce_count_;
    case FX1_DDR3_UE_COUNT: return ue_count_;
    case FX1_DDR3_ECC_LAST_ADDR_LO: return static_cast<std::uint32_t>(ecc_last_addr_);
    case FX1_DDR3_ECC_LAST_ADDR_HI: return static_cast<std::uint32_t>(ecc_last_addr_ >> 32);
    case FX1_DDR3_ECC_LAST_INFO: return ecc_last_info_;
    case FX1_DDR3_SERVICE_COUNT_BE: return service_count_[0];
    case FX1_DDR3_SERVICE_COUNT_SRT: return service_count_[1];
    case FX1_DDR3_SERVICE_COUNT_HRT: return service_count_[2];
    default:
        // Scheduler, dispatch and backend occupancy words (DEC-03: unavailable, read 0).
        return 0;
    }
}

// ---------------------------------------------------------------- write side
Ddr3Controller::Resp Ddr3Controller::csr_write(std::uint32_t offset, std::uint32_t data, std::uint8_t strobes) {
    const CsrDesc* d = csr_find(offset);
    if (!d) return Resp::DecErr;
    const auto off = d->offset;
    switch (d->access) {
    case CsrAccess::RO: return Resp::SlvErr;
    case CsrAccess::W1C:
    case CsrAccess::WO_CMD:
        // Full-word strobes required; partial writes have no side effect (HAS §8.2).
        if (strobes != 0xF) return Resp::SlvErr;
        if (data & d->rsvd_zero) return Resp::SlvErr;
        if (d->access == CsrAccess::WO_CMD) return command(off, data);
        if (off == FX1_DDR3_IRQ_CLEAR) {
            irq_clear(data & FX1_DDR3_IRQ_ALL);
        } else if (off == FX1_DDR3_COMMAND_ERROR) {
            sticky_clear(off, data & 0x7u);
        } else if (off == FX1_DDR3_INIT_ERROR) {
            sticky_clear(off, data & 0xFFFFu);
        } else if (off == FX1_DDR3_POWER_ERROR) {
            sticky_clear(off, data & 0xFFFFFFu);
        } else if (off == FX1_DDR3_PHY_BRIDGE_ERROR) {
            sticky_clear(off, data & 0xFFu);
        }
        // RESET_CAUSE reads zero in this profile: nothing to clear.
        return Resp::Okay;
    case CsrAccess::RW:
    case CsrAccess::SHADOW:
    case CsrAccess::INIT_RW: {
        if (strobes == 0) return Resp::Okay;  // legal no-op for byte-writeable classes
        const std::uint32_t m = lane_mask(strobes);
        const std::uint32_t merged = (storage(off) & ~m) | (data & m);
        if (merged & d->rsvd_zero) return Resp::SlvErr;
        if (d->access == CsrAccess::INIT_RW) return write_init_rw(off, merged);
        if (d->access == CsrAccess::RW) return write_policy(off, merged);
        set_storage(off, merged);  // SHADOW: takes effect only through its commit
        return Resp::Okay;
    }
    }
    return Resp::SlvErr;
}

Ddr3Controller::Resp Ddr3Controller::write_policy(std::uint16_t off, std::uint32_t merged) {
    switch (off) {
    case FX1_DDR3_WRITE_DRAIN_CFG:
    case FX1_DDR3_STARVE_LIMIT:
    case FX1_DDR3_READ_TIMEOUT: {
        // One validated coherent policy image; an invalid value keeps the old image.
        PolicyImage p{storage(FX1_DDR3_WRITE_DRAIN_CFG), storage(FX1_DDR3_STARVE_LIMIT), storage(FX1_DDR3_READ_TIMEOUT)};
        if (off == FX1_DDR3_WRITE_DRAIN_CFG) p.write_drain_cfg = merged;
        if (off == FX1_DDR3_STARVE_LIMIT) p.starve_limit = merged;
        if (off == FX1_DDR3_READ_TIMEOUT) p.read_timeout = merged;
        if (!check_policy(p, dfi_valid_ ? &dfi_active_ : nullptr, cfg_.dfi_ratio()).ok) return Resp::SlvErr;
        break;
    }
    case FX1_DDR3_REFRESH_CFG: {
        const unsigned limit = merged & 0xFu;
        if (limit < 1 || limit > 8) return Resp::SlvErr;
        break;
    }
    case FX1_DDR3_POWER_TIMEOUT:
    case FX1_DDR3_TRAIN_TIMEOUT:
        if (merged == 0) return Resp::SlvErr;
        break;
    case FX1_DDR3_CE_THRESHOLD:
        if (!cfg_.ecc_enable || merged == 0) return Resp::SlvErr;  // optional ECC word; nonzero (C-16)
        break;
    case FX1_DDR3_IRQ_MASK:
        merged &= FX1_DDR3_IRQ_ALL;  // [31:12] read zero; writes ignored
        break;
    default: break;  // GLOBAL_CTRL, SCRATCH
    }
    set_storage(off, merged);
    if (off == FX1_DDR3_IRQ_MASK) irq_update();
    if (off == FX1_DDR3_GLOBAL_CTRL) {
        update_readiness();
        update_lifecycle();
        notify_lifecycle();  // admission stop / enable change the port and backend gates
    }
    if (off == FX1_DDR3_REFRESH_CFG) {
        refresh_reschedule(0);
        refresh_reschedule(1);
    }
    return Resp::Okay;
}

Ddr3Controller::Resp Ddr3Controller::write_init_rw(std::uint16_t off, std::uint32_t merged) {
    if (init_rw_locked()) {
        command_error(FX1_DDR3_CMD_ERR_COMMIT);  // interlock rejection (GAP-LC-06)
        return Resp::SlvErr;
    }
    if (off >= FX1_DDR3_MR(0) && off <= FX1_DDR3_MR(3)) {
        if (!check_mode_register((off - FX1_DDR3_MR(0)) / 4, merged).ok) return Resp::SlvErr;
    }
    if (off == FX1_DDR3_TRAIN_TRAS_WAIT && merged == 0) return Resp::SlvErr;
    set_storage(off, merged);
    return Resp::Okay;
}

// ---------------------------------------------------------------- command ownership
std::uint64_t Ddr3Controller::mutex_acquire(MutexOwner owner) {
    if (mutex_busy()) {
        if (owner == MutexOwner::Csr) command_error(FX1_DDR3_CMD_ERR_BUSY);  // SLVERR, not queued
        return 0;
    }
    mutex_owner_ = owner;
    mutex_token_ = ++mutex_next_token_;
    update_lifecycle();
    return mutex_token_;
}

void Ddr3Controller::mutex_release(std::uint64_t token) {
    // Only the current owner frees the mutex; a token of a retired command is ignored.
    if (token == 0 || token != mutex_token_ || !mutex_busy()) return;
    mutex_owner_ = MutexOwner::None;
    mutex_token_ = 0;
    update_lifecycle();
}

Ddr3Controller::CmdGuard Ddr3Controller::guard() const {
    return {scheduler_.epoch(kDomainCsr), scheduler_.epoch(kDomainCore)};
}

bool Ddr3Controller::csr_alive(const CmdGuard& g) const { return scheduler_.epoch(kDomainCsr) == g.csr; }
bool Ddr3Controller::core_alive(const CmdGuard& g) const { return scheduler_.epoch(kDomainCore) == g.core; }

bool Ddr3Controller::endpoint_wait(const CmdGuard& g) {
    sc_core::wait(cycles(kEndpointCycles));
    return csr_alive(g) && core_alive(g);
}

void Ddr3Controller::abandon_command(const CmdGuard& g, std::uint64_t token, std::uint32_t* status) {
    if (!csr_alive(g)) return;  // the CSR reset already cleared the CSR-side state and mutex
    if (status) commit_reject(*status, FX1_DDR3_BLK_CANCELLED, true);
    mutex_release(token);
}

void Ddr3Controller::wake_commands() {
    if (!sc_core::sc_is_running()) return;
    train_event_.notify(sc_core::SC_ZERO_TIME);
    lifecycle_event_.notify(sc_core::SC_ZERO_TIME);
}

void Ddr3Controller::commit_reject(std::uint32_t& status, std::uint8_t blocker, bool acknowledged) {
    status = FX1_DDR3_COMMIT_ST_ERROR | (std::uint32_t{blocker} << (acknowledged ? 8 : 16));
    command_error(acknowledged ? FX1_DDR3_CMD_ERR_BLOCKED : FX1_DDR3_CMD_ERR_COMMIT);
    irq_set(4);  // mapping/configuration (GAP-CSR-10)
}

bool Ddr3Controller::quiesce_ok() const {
    // A completed global quiesce, or a controller whose mission admission has not opened
    // since the last reset/reinitialization and is idle (GAP-LC-07).
    const bool idle = !core_owns_work();
    return (global_quiesce_req_ && idle) || (!admission_opened_ && idle);
}

TimingImage Ddr3Controller::timing_shadow() const {
    TimingImage t{};
    for (unsigned i = 0; i < kTimingWords; ++i) t[i] = storage(static_cast<std::uint16_t>(FX1_DDR3_TIMING_SHADOW(i)));
    return t;
}

DfiImage Ddr3Controller::dfi_shadow() const {
    return {storage(FX1_DDR3_DFI_WR_ENABLE_DELAY), storage(FX1_DDR3_DFI_RD_ENABLE_DELAY),
            storage(FX1_DDR3_PHY_MAX_READ_RETURN), storage(FX1_DDR3_DFI_ASSEMBLY_MARGIN)};
}

void Ddr3Controller::recheck_dfi_against_timing() {
    // The DFI image is legal only against the active timing (MAS §6.2.12 formulas). A new
    // timing image that no longer matches requires a new DFI_TIMING_COMMIT (VP-local).
    if (dfi_valid_ && !check_dfi_image(dfi_active_, dfi_inputs(cfg_, timing_active_), storage(FX1_DDR3_READ_TIMEOUT)).ok)
        dfi_valid_ = false;
}

// ---------------------------------------------------------------- commands
Ddr3Controller::Resp Ddr3Controller::command(std::uint16_t off, std::uint32_t data) {
    switch (off) {
    case FX1_DDR3_QUIESCE_CTRL: return cmd_quiesce(data);
    case FX1_DDR3_SNAPSHOT_CMD: return cmd_snapshot();
    case FX1_DDR3_FE_PORT_CMD: return cmd_fe_port(data);
    case FX1_DDR3_FE_REGION_COMMIT: return (data & 1u) ? cmd_region_commit() : Resp::Okay;
    case FX1_DDR3_TOPO_COMMIT: return (data & 1u) ? cmd_topo_commit() : Resp::Okay;
    case FX1_DDR3_TIMING_COMMIT: return (data & 1u) ? cmd_timing_commit() : Resp::Okay;
    case FX1_DDR3_DFI_TIMING_COMMIT: return (data & 1u) ? cmd_dfi_commit() : Resp::Okay;
    case FX1_DDR3_INIT_CMD:   // start/retry: SLVERR, no launch (DEC-05, HAS §9.1.1)
    case FX1_DDR3_POWER_CMD:  // power is requested through the P-channel only
        return Resp::SlvErr;
    case FX1_DDR3_TRAIN_CMD: return cmd_train(data);
    case FX1_DDR3_IRQ_TEST_SET: return cmd_irq_test_set(data);
    default: return Resp::SlvErr;
    }
}

Ddr3Controller::Resp Ddr3Controller::cmd_quiesce(std::uint32_t data) {
    const unsigned op = data & 3u;
    if (op != 1u && op != 2u) return Resp::SlvErr;  // exactly one of request/release (GAP-LC-08)
    const CmdGuard g = guard();
    const std::uint64_t tok = mutex_acquire(MutexOwner::Csr);
    if (!tok) return Resp::SlvErr;
    global_quiesce_req_ = (op == 1u);
    update_readiness();
    notify_lifecycle();
    if (!endpoint_wait(g)) {  // B returns once applied; completion is polled
        abandon_command(g, tok, nullptr);
        return Resp::SlvErr;
    }
    mutex_release(tok);
    return Resp::Okay;
}

Ddr3Controller::Resp Ddr3Controller::cmd_snapshot() {
    if (snapshot_busy_) {
        command_error(FX1_DDR3_CMD_ERR_BUSY);
        return Resp::SlvErr;
    }
    const CmdGuard g = guard();
    snapshot_busy_ = true;
    update_lifecycle();
    if (!endpoint_wait(g)) {
        // A reset invalidates the pending capture; a partial capture is never valid. After
        // a CSR reset the snapshot state already belongs to the reset (or a newer capture).
        if (csr_alive(g)) {
            snapshot_busy_ = false;
            update_lifecycle();
        }
        return Resp::SlvErr;
    }
    // One coherent capture point for every snapshot-backed word (GAP-CSR-20).
    for (const CsrDesc& d : csr_table())
        if (d.source == CsrSource::Snapshot) snapshot_[d.offset] = snapshot_source(d.offset);
    // ECC_CAP_STATUS dynamic fields: [1] an ECC write parent in service, [8:2] granule locks
    // held (queued owners plus fatal-held locks, APPROX occupancy).
    const std::size_t locks = std::min<std::size_t>(0x7F, lock_queue_.size() + fatal_locks_.size());
    snapshot_[FX1_DDR3_ECC_CAP_STATUS] =
        (ecc_writes_ ? FX1_DDR3_ECC_CAP_RMW_BUSY : 0u) | (static_cast<std::uint32_t>(locks) << 2);
    ++snapshot_seq_;
    snapshot_valid_ = true;
    snapshot_busy_ = false;
    update_lifecycle();
    return Resp::Okay;
}

Ddr3Controller::Resp Ddr3Controller::cmd_fe_port(std::uint32_t data) {
    const unsigned op = data & 7u;
    const unsigned port = (data >> 8) & 3u;
    const CmdGuard g = guard();
    const std::uint64_t tok = mutex_acquire(MutexOwner::Csr);
    if (!tok) return Resp::SlvErr;
    const std::uint32_t cmd_bits = (std::uint32_t{op} << 3) | (std::uint32_t{port} << 8);
    auto reject = [&](std::uint8_t blocker) {
        fe_port_status_ = FX1_DDR3_FE_PORT_ST_ERROR | cmd_bits | (std::uint32_t{blocker} << 16);
        command_error(FX1_DDR3_CMD_ERR_BLOCKED);
        mutex_release(tok);
        return Resp::SlvErr;
    };
    const bool valid_op = op == FX1_DDR3_FE_OP_QUIESCE || op == FX1_DDR3_FE_OP_RELEASE || op == FX1_DDR3_FE_OP_SOFT_RESET;
    if (!valid_op || port >= cfg_.n_ports) return reject(FX1_DDR3_BLK_BAD_PORT_OR_OP);
    if (op == FX1_DDR3_FE_OP_SOFT_RESET && !port_quiesce_ack_[port])
        return reject(FX1_DDR3_BLK_NO_QUIESCE_ACK);  // SOQ-10: rejected instead of stalling
    fe_port_status_ = FX1_DDR3_FE_PORT_ST_BUSY | cmd_bits;  // a new legal command clears done/error
    const std::uint64_t holds_before = soft_reset_done_[port];
    const std::uint64_t port_gen = port_reset_gen_[port];
    if (op == FX1_DDR3_FE_OP_QUIESCE) csr_port_quiesce_[port] = true;
    if (op == FX1_DDR3_FE_OP_RELEASE) csr_port_quiesce_[port] = false;
    if (op == FX1_DDR3_FE_OP_SOFT_RESET) {
        soft_reset_pending_[port] = true;
        soft_reset_req_[port] = false;
    }
    update_lifecycle();
    notify_lifecycle();
    // The write response waits for the endpoint result: the ack for a quiesce, a completed
    // hold for a soft reset. Any reset of the CSR, core or port domain cancels it.
    auto finished = [&] {
        if (op == FX1_DDR3_FE_OP_QUIESCE) return port_quiesce_ack_[port];
        if (op == FX1_DDR3_FE_OP_SOFT_RESET) return soft_reset_done_[port] != holds_before;
        return true;
    };
    auto cancelled = [&] {
        // Any reset of the command's CSR, core or port domain since acceptance cancels it,
        // including a port reset pulse that has already been released (S2-R4a).
        if (!csr_alive(g) || !core_alive(g) || port_in_reset(port) || port_reset_gen_[port] != port_gen) return true;
        // A soft reset that is neither pending, holding nor completed was retired.
        return op == FX1_DDR3_FE_OP_SOFT_RESET && !soft_reset_pending_[port] && !soft_reset_active_[port] &&
               soft_reset_done_[port] == holds_before;
    };
    bool ok = endpoint_wait(g) && !cancelled();
    while (ok && !finished()) {
        sc_core::wait(lifecycle_event_);
        ok = !cancelled();
    }
    if (!ok) {
        if (csr_alive(g)) {
            fe_port_status_ = FX1_DDR3_FE_PORT_ST_ERROR | cmd_bits | (std::uint32_t{FX1_DDR3_BLK_CANCELLED} << 16);
            command_error(FX1_DDR3_CMD_ERR_BLOCKED);
            mutex_release(tok);
        }
        return Resp::SlvErr;
    }
    fe_port_status_ = FX1_DDR3_FE_PORT_ST_DONE | cmd_bits;
    std::uint32_t state = FX1_DDR3_FE_STATE_VALID;
    for (unsigned p = 0; p < kPortBundles; ++p) {
        state |= (csr_port_quiesce_[p] ? 1u : 0u) << p;
        state |= ((csr_port_quiesce_[p] || port_quiesce_req_[p]) ? 1u : 0u) << (4 + p);
        state |= (port_quiesce_ack_[p] ? 1u : 0u) << (8 + p);
        state |= (soft_reset_active_[p] ? 1u : 0u) << (12 + p);
        state |= (soft_reset_pending_[p] ? 1u : 0u) << (20 + p);
        state |= (!port_owns_work(p) ? 1u : 0u) << (24 + p);
    }
    fe_port_state_ = state;  // published on a successful endpoint result
    mutex_release(tok);
    return Resp::Okay;
}

Ddr3Controller::Resp Ddr3Controller::cmd_region_commit() {
    const CmdGuard g = guard();
    const std::uint64_t tok = mutex_acquire(MutexOwner::Csr);
    if (!tok) return Resp::SlvErr;
    RegionImage img{};
    for (unsigned i = 0; i < 32; ++i) img[i] = storage(static_cast<std::uint16_t>(FX1_DDR3_FE_REGION_SHADOW(i)));
    const CheckResult c = check_region_image(img, topo_valid_ ? geom_.usable_granules : 0);
    if (!c.ok) {
        region_status_ = FX1_DDR3_FE_REGION_ST_ERROR | (std::uint32_t{c.blocker} << 8);
        command_error(FX1_DDR3_CMD_ERR_COMMIT);
        irq_set(4);
        mutex_release(tok);
        return Resp::SlvErr;
    }
    region_status_ = FX1_DDR3_FE_REGION_ST_BUSY;
    // Publication quiesce on every port: no new AW/AR, accepted transactions drain (waits
    // indefinitely, GAP-FE-12); the endpoint then installs the frozen image atomically and
    // releases the quiesce (HAS §6.1.15, MAS T6-68). The quiesce also invalidates every
    // exclusive reservation (FE-6.1.12-10).
    const std::uint64_t pub = ++pub_quiesce_token_;
    pub_quiesce_ = true;
    update_lifecycle();
    notify_lifecycle();
    auto release_quiesce = [&] {
        if (pub_quiesce_token_ != pub) return;  // a newer publication owns the quiesce
        pub_quiesce_ = false;
        update_lifecycle();
        notify_lifecycle();
    };
    auto drained = [&] {
        for (unsigned p = 0; p < cfg_.n_ports; ++p)
            if (port_owns_work(p)) return false;
        return true;
    };
    bool ok = endpoint_wait(g);
    while (ok && !drained()) {
        sc_core::wait(lifecycle_event_);
        ok = csr_alive(g) && core_alive(g);
    }
    if (!ok) {
        release_quiesce();
        if (csr_alive(g)) {
            region_status_ = FX1_DDR3_FE_REGION_ST_ERROR | (std::uint32_t{FX1_DDR3_BLK_CANCELLED} << 8);
            command_error(FX1_DDR3_CMD_ERR_BLOCKED);
        }
        abandon_command(g, tok, nullptr);
        return Resp::SlvErr;
    }
    region_active_ = img;
    region_valid_ = true;
    storage_changed();  // addresses may now designate other storage
    region_status_ = FX1_DDR3_FE_REGION_ST_DONE;
    release_quiesce();
    mutex_release(tok);
    return Resp::Okay;
}

Ddr3Controller::Resp Ddr3Controller::cmd_topo_commit() {
    const CmdGuard g = guard();
    const std::uint64_t tok = mutex_acquire(MutexOwner::Csr);
    if (!tok) return Resp::SlvErr;
    auto reject = [&](std::uint8_t blocker) {
        commit_reject(topo_status_, blocker, false);
        mutex_release(tok);
        return Resp::SlvErr;
    };
    if (recovery_required_) return reject(FX1_DDR3_BLK_RECOVERY_REQUIRED);
    if (!quiesce_ok()) return reject(FX1_DDR3_BLK_NOT_QUIESCED);
    Geometry geo{};
    const std::uint32_t shadow = storage(FX1_DDR3_TOPO_SHADOW);
    if (!check_topology(shadow, cfg_, geo).ok) return reject(kBlockerTopology);
    topo_status_ = 0;
    if (!endpoint_wait(g)) {
        abandon_command(g, tok, &topo_status_);
        return Resp::SlvErr;
    }
    topo_active_ = shadow;
    geom_ = geo;
    topo_valid_ = true;
    topo_status_ = FX1_DDR3_COMMIT_ST_DONE | FX1_DDR3_COMMIT_ST_REINIT;
    init_restart();  // BE05: a topology publication restarts full initialization
    init_step();
    mutex_release(tok);
    return Resp::Okay;
}

Ddr3Controller::Resp Ddr3Controller::cmd_timing_commit() {
    const CmdGuard g = guard();
    const std::uint64_t tok = mutex_acquire(MutexOwner::Csr);
    if (!tok) return Resp::SlvErr;
    auto reject = [&](std::uint8_t blocker) {
        commit_reject(timing_status_, blocker, false);
        mutex_release(tok);
        return Resp::SlvErr;
    };
    if (recovery_required_) return reject(FX1_DDR3_BLK_RECOVERY_REQUIRED);
    if (!quiesce_ok()) return reject(FX1_DDR3_BLK_NOT_QUIESCED);
    const TimingImage t = timing_shadow();
    if (!check_timing(t).ok) return reject(kBlockerTiming);
    timing_status_ = 0;
    if (!endpoint_wait(g)) {
        abandon_command(g, tok, &timing_status_);
        return Resp::SlvErr;
    }
    timing_active_ = t;
    timing_valid_ = true;
    recheck_dfi_against_timing();
    timing_status_ = FX1_DDR3_COMMIT_ST_DONE | FX1_DDR3_COMMIT_ST_REINIT;
    init_restart();
    init_step();
    mutex_release(tok);
    return Resp::Okay;
}

Ddr3Controller::Resp Ddr3Controller::cmd_dfi_commit() {
    const CmdGuard g = guard();
    const std::uint64_t tok = mutex_acquire(MutexOwner::Csr);
    if (!tok) return Resp::SlvErr;
    auto reject = [&](std::uint8_t blocker) {
        commit_reject(dfi_status_, blocker, false);
        mutex_release(tok);
        return Resp::SlvErr;
    };
    if (recovery_required_) return reject(FX1_DDR3_BLK_RECOVERY_REQUIRED);
    if (!quiesce_ok()) return reject(FX1_DDR3_BLK_NOT_QUIESCED);
    if (!timing_valid_) return reject(FX1_DDR3_BLK_DEPENDENCY);
    const DfiImage d = dfi_shadow();
    // Idle interlock: no read token pending and the adapter idle (no owned work).
    if (core_owns_work()) return reject(FX1_DDR3_BLK_NOT_QUIESCED);
    if (!check_dfi_image(d, dfi_inputs(cfg_, timing_active_), storage(FX1_DDR3_READ_TIMEOUT)).ok)
        return reject(FX1_DDR3_BLK_FIELD_ILLEGAL);
    dfi_status_ = 0;
    if (!endpoint_wait(g)) {
        abandon_command(g, tok, &dfi_status_);
        return Resp::SlvErr;
    }
    dfi_active_ = d;
    dfi_valid_ = true;
    dfi_status_ = FX1_DDR3_COMMIT_ST_DONE;
    init_restart();
    init_step();
    mutex_release(tok);
    return Resp::Okay;
}

Ddr3Controller::Resp Ddr3Controller::cmd_train(std::uint32_t data) {
    const unsigned prim = (data >> 1) & 7u;
    const unsigned rank = (data >> FX1_DDR3_TRAIN_CMD_RANK_SHIFT) & 1u;
    const unsigned lanes = (data & FX1_DDR3_TRAIN_CMD_LANES_MASK) >> FX1_DDR3_TRAIN_CMD_LANES_SHIFT;
    const auto options = static_cast<std::uint16_t>(data >> 12);
    if (!(data & 1u)) return Resp::SlvErr;  // TRAIN_CMD[0] start must be one
    if (prim > FX1_DDR3_TRAIN_PRIM_LOAD_ALL) return Resp::SlvErr;
    const CmdGuard g = guard();
    const std::uint64_t tok = mutex_acquire(MutexOwner::Csr);
    if (!tok) return Resp::SlvErr;
    auto set_status = [&](bool ok, std::uint8_t code) {
        train_status_ = (ok ? FX1_DDR3_TRAIN_STATUS_DONE : FX1_DDR3_TRAIN_STATUS_ERROR) | (std::uint32_t{code} << 8);
    };
    // Preconditions (GAP-LC-12): an initialized controller in the active power state, a
    // populated rank, at least one selected byte lane, no sticky training/PHY failure and
    // no recovery requirement. An unsupported request is rejected, never acknowledged.
    if (!init_done_ || init_failed_ || power_resident_ != FX1_DDR3_PSTATE_ACTIVE || power_transition_ ||
        training_error_ || phy_failure_ || recovery_required_ || rank >= geom_.ranks || lanes == 0) {
        set_status(false, FX1_DDR3_TRAIN_CODE_REJECTED);
        command_error(FX1_DDR3_CMD_ERR_BLOCKED);
        mutex_release(tok);
        return Resp::SlvErr;
    }
    // Runtime training: stop admission, drain, train one rank, reopen (LC-9.1.2-08).
    train_status_ = FX1_DDR3_TRAIN_STATUS_BUSY;
    training_busy_ = true;
    runtime_training_ = true;
    update_readiness();
    // Readiness is closed: accepted transactions wait at the backend gate. Drain what is in
    // service before the PHY delays change.
    while (core_owns_work()) {
        sc_core::wait(lifecycle_event_);
        if (!core_alive(g)) {
            if (csr_alive(g)) mutex_release(tok);
            return Resp::SlvErr;  // reinitialization already cleared the training flags
        }
        if (!csr_alive(g)) {
            // No PHY load was issued: the rank keeps its delays and stays usable.
            training_busy_ = false;
            runtime_training_ = false;
            set_status(false, FX1_DDR3_TRAIN_CODE_CANCELLED);
            update_readiness();
            return Resp::SlvErr;
        }
    }
    train_tag_ = static_cast<std::uint8_t>(train_tag_ % 15u + 1u);  // tag 0 is the boot convention
    std::vector<PhyLoadRequest> loads;
    const auto make = [&](PhyLoadClass cls, std::uint32_t payload) {
        PhyLoadRequest r;
        r.rank = rank;
        r.cls = cls;
        r.payload = payload;
        r.skew0 = storage(FX1_DDR3_PHY_SKEW_CTRL0);
        r.skew1 = storage(FX1_DDR3_PHY_SKEW_CTRL1);
        r.epoch = train_epoch_;
        r.tag = train_tag_;
        r.lane_mask = static_cast<std::uint8_t>(lanes);  // carried to the provider (S2-R5)
        r.options = options;
        loads.push_back(r);
    };
    const std::uint32_t wrlvl = (1u << (storage(FX1_DDR3_PHY_WRLVL_DELAY) & 7u)) - 1u;  // BE18 thermometer
    if (prim == FX1_DDR3_TRAIN_PRIM_LOAD_RDLVL || prim == FX1_DDR3_TRAIN_PRIM_LOAD_ALL)
        make(PhyLoadClass::ReadLevel, storage(FX1_DDR3_PHY_RDLVL_DELAY) & 0xFFFFFFu);
    if (prim == FX1_DDR3_TRAIN_PRIM_LOAD_GATE || prim == FX1_DDR3_TRAIN_PRIM_LOAD_ALL)
        make(PhyLoadClass::GateLevel, storage(FX1_DDR3_PHY_GATE_DELAY) & 0xFFFFFFu);
    if (prim == FX1_DDR3_TRAIN_PRIM_LOAD_WRLVL || prim == FX1_DDR3_TRAIN_PRIM_LOAD_ALL)
        make(PhyLoadClass::WriteLevel, wrlvl);
    train_finished_ = false;
    training_start(std::move(loads), [this](bool ok, std::uint8_t code) {
        train_finished_ = true;
        train_code_ = ok ? FX1_DDR3_TRAIN_CODE_OK : code;
        train_event_.notify(sc_core::SC_ZERO_TIME);
    });
    const std::uint64_t episode = train_episode_;
    while (!train_finished_) {
        sc_core::wait(train_event_);
        if (!core_alive(g)) {
            // Core reset: reinitialization already retired the episode and its flags.
            if (csr_alive(g)) mutex_release(tok);
            return Resp::SlvErr;
        }
        if (!csr_alive(g)) {
            // CSR reset: the command is gone, so retire its core-side episode here. The PHY
            // delay state of the rank is unknown: it stays unavailable until recovery.
            if (train_episode_ == episode) training_abort();
            training_busy_ = false;
            runtime_training_ = false;
            training_error_ = true;
            set_status(false, FX1_DDR3_TRAIN_CODE_CANCELLED);
            update_readiness();
            return Resp::SlvErr;
        }
    }
    const bool ok = train_code_ == FX1_DDR3_TRAIN_CODE_OK;
    set_status(ok, train_code_);
    training_busy_ = false;
    runtime_training_ = false;
    if (!ok) {
        training_error_ = true;  // the rank stays unavailable until recovery (BE06)
        irq_set(7);
    }
    update_readiness();
    mutex_release(tok);
    return ok ? Resp::Okay : Resp::SlvErr;
}

Ddr3Controller::Resp Ddr3Controller::cmd_irq_test_set(std::uint32_t data) {
    if (!cfg_.irq_test_enable) return Resp::SlvErr;  // feature absent (CAP_FEATURE[11] = 0)
    for (unsigned b = 0; b < kIrqSources; ++b)
        if ((data >> b) & 1u) irq_set(b);  // same raw path; no ECC/PHY metadata is created
    return Resp::Okay;
}

} // namespace fx1::ddr3
