// SPDX-License-Identifier: Apache-2.0
// Construction, sockets, sideband controls, reset/quiesce bundle and interrupts.
#include "fx1_ddr3/ddr3_controller.h"

#include "fx1_ddr3/fx1_ddr3_regs.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace fx1::ddr3 {

using sc_core::sc_time;

Ddr3Controller::Ddr3Controller(sc_core::sc_module_name name, const BuildConfig& cfg,
                               PhyProvider* phy)
    : sc_module(name), cfg_(cfg), phy_(phy) {
    require_valid(cfg_);
    timebase_.tck_ps = tck_ps(cfg_.speed_bin);
    timebase_.ratio = cfg_.dfi_ratio();
    ext_ = ext_timing_image(cfg_);
    if (!phy_) {
        own_phy_ = std::make_unique<FunctionalPhy>("phy");
        phy_ = own_phy_.get();
    }
    phy_->attach(this);
    mem_or_lifecycle_ |= mem_event_;
    mem_or_lifecycle_ |= lifecycle_event_;

    csr_socket.register_b_transport(this, &Ddr3Controller::csr_b_transport);
    csr_socket.register_transport_dbg(this, &Ddr3Controller::csr_transport_dbg);
    for (unsigned p = 0; p < kPortBundles; ++p) {
        mem_socket[p].register_b_transport(this, &Ddr3Controller::mem_b_transport, static_cast<int>(p));
        mem_socket[p].register_transport_dbg(this, &Ddr3Controller::mem_transport_dbg, static_cast<int>(p));
    }
    apply_csr_reset();
    for (auto& lut : qos_lut_) lut.fill(kQosClassNormal);  // FE05 reset policy
    apply_core_reset();

    // A single process drives the output ports: their state changes in CSR initiator
    // threads, scheduler methods and PHY callbacks, and an sc_signal has one writer.
    SC_METHOD(drive_outputs);
    sensitive << outputs_event_;
}

Ddr3Controller::~Ddr3Controller() = default;

void Ddr3Controller::end_of_elaboration() {
    // FE01: positions above DDR_SYS_N_PORT are tied off; binding one is an integration error.
    for (unsigned p = cfg_.n_ports; p < kPortBundles; ++p) {
        if (mem_socket[p].size() > 0) {
            SC_REPORT_ERROR(name(), ("memory port " + std::to_string(p) +
                                     " is bound but the build enables only " +
                                     std::to_string(cfg_.n_ports) + " port(s)").c_str());
        }
    }
}

void Ddr3Controller::start_of_simulation() {
    init_step();  // cold reset released at time 0: init waits in HOLD
}

void Ddr3Controller::request_outputs() {
    if (sc_core::sc_is_running()) outputs_event_.notify(sc_core::SC_ZERO_TIME);
}

void Ddr3Controller::drive_outputs() {
    irq.write(irq_level_);
    if (mission_ready_out.size()) mission_ready_out->write(mission_ready_);
    if (global_idle_out.size()) global_idle_out->write(global_idle_);
    if (fe_drained_out.size()) fe_drained_out->write(fe_drained_);
    if (init_complete_out.size()) init_complete_out->write(init_complete_);
}

void Ddr3Controller::warn_once(const std::string& what) {
    // One warning per distinct message keeps logs readable.
    if (!warned_.insert(what).second) return;
    SC_REPORT_WARNING(name(), what.c_str());
}

void Ddr3Controller::check_port(unsigned port, const char* api) const {
    if (port >= kPortBundles)
        throw std::out_of_range(std::string(api) + ": port index " + std::to_string(port) + " out of range");
}

sc_time Ddr3Controller::cycles(std::uint64_t n) const {
    return sc_time(static_cast<double>(timebase_.ctrl_cycles_ps(n)), sc_core::SC_PS);
}

sc_time Ddr3Controller::tck_wait(std::uint64_t n_tck) const {
    return sc_time(static_cast<double>(timebase_.tck_wait_ps(n_tck)), sc_core::SC_PS);
}

// ---------------------------------------------------------------- sideband
void Ddr3Controller::set_bootstrap_region(unsigned port, unsigned region, RegionEntry entry) {
    check_port(port, "set_bootstrap_region");
    if (region >= kRegionsPerPort) throw std::out_of_range("set_bootstrap_region: region index out of range");
    bootstrap_[port][region] = entry;
}

RegionEntry Ddr3Controller::bootstrap_region(unsigned port, unsigned region) const {
    check_port(port, "bootstrap_region");
    if (region >= kRegionsPerPort) throw std::out_of_range("bootstrap_region: region index out of range");
    return bootstrap_[port][region];
}

void Ddr3Controller::qos_write(unsigned port, unsigned qos_index, QosClass cls) {
    check_port(port, "qos_write");
    if (qos_index >= 16) throw std::out_of_range("qos_write: QoS index must be 0..15");
    if (cls > kQosClassMax) throw std::out_of_range("qos_write: class is a 2-bit code (0..3)");
    qos_lut_[port][qos_index] = cls;
}

void Ddr3Controller::qos_set_configured(unsigned port, bool configured) {
    check_port(port, "qos_set_configured");
    qos_configured_[port] = configured;
    notify_lifecycle();  // blocked callers re-evaluate the FE05 gate
}

bool Ddr3Controller::qos_configured(unsigned port) const {
    check_port(port, "qos_configured");
    return qos_configured_[port];
}

QosClass Ddr3Controller::qos_class(unsigned port, unsigned qos_index) const {
    check_port(port, "qos_class");
    if (qos_index >= 16) throw std::out_of_range("qos_class: QoS index must be 0..15");
    return qos_lut_[port][qos_index];
}

void Ddr3Controller::set_reserved_rate(unsigned port, std::uint32_t rate, std::uint32_t depth) {
    check_port(port, "set_reserved_rate");
    if (rate >= (1u << 20) || depth >= (1u << 20))
        throw std::out_of_range("set_reserved_rate: rate and depth are 20-bit fields");
    rsv_rate_[port] = rate;
    rsv_depth_[port] = depth;
}

// ---------------------------------------------------------------- interrupts and sticky words
void Ddr3Controller::irq_set(unsigned source) {
    irq_raw_ |= 1u << source;
    irq_set_time_[source] = sc_core::sc_time_stamp();
    irq_update();
}

void Ddr3Controller::irq_clear(std::uint32_t mask) {
    // Hardware set wins a same-cycle clear (HAS §8.5): a bit set at the current simulated
    // time survives a W1C issued at that time.
    const sc_time now = sc_core::sc_time_stamp();
    for (unsigned b = 0; b < kIrqSources; ++b)
        if (((mask >> b) & 1u) && irq_set_time_[b] != now) irq_raw_ &= ~(1u << b);
    irq_update();
}

void Ddr3Controller::irq_update() {
    const bool level = (irq_raw_ & storage(FX1_DDR3_IRQ_MASK) & FX1_DDR3_IRQ_ALL) != 0;
    if (level == irq_level_) return;
    irq_level_ = level;
    request_outputs();
}

void Ddr3Controller::sticky_set(std::uint16_t offset, std::uint32_t bits) {
    storage_[offset] |= bits;
    auto& times = sticky_set_time_[offset];
    for (unsigned b = 0; b < 32; ++b)
        if ((bits >> b) & 1u) times[b] = sc_core::sc_time_stamp();
}

void Ddr3Controller::sticky_clear(std::uint16_t offset, std::uint32_t bits) {
    const sc_time now = sc_core::sc_time_stamp();
    auto& times = sticky_set_time_[offset];
    for (unsigned b = 0; b < 32; ++b)
        if (((bits >> b) & 1u) && times[b] != now) storage_[offset] &= ~(1u << b);
}

void Ddr3Controller::command_error(std::uint32_t bit) {
    // COMMAND_ERROR set paths also raise the CSR command/interlock interrupt (GAP-CSR-04).
    sticky_set(FX1_DDR3_COMMAND_ERROR, bit);
    irq_set(10);
}

// ---------------------------------------------------------------- reset / quiesce
bool Ddr3Controller::port_in_reset(unsigned port) const {
    return cold_reset_ || port_reset_[port];
}

bool Ddr3Controller::admission_stopped(unsigned port) const {
    // Frontend stop conditions: physical core reset, core reset request (all ports), port
    // quiesce from the sideband or from FE_PORT_CMD, the region-publication quiesce, the
    // soft-reset hold, the FE05 QoS gate, and the software stops (QUIESCE_CTRL request,
    // GLOBAL_CTRL admission stop). Software stops hold new transactions at the ports and
    // never strand accepted ones (MAS Table 6-73). Readiness and GLOBAL_CTRL.enable gate
    // the backend instead (backend_open()).
    return core_reset_ || core_reset_req_ || port_quiesce_req_[port] || csr_port_quiesce_[port] || pub_quiesce_ ||
           soft_reset_active_[port] || !qos_configured_[port] || global_quiesce_req_ ||
           (storage(FX1_DDR3_GLOBAL_CTRL) & FX1_DDR3_GLOBAL_CTRL_ADM_STOP) != 0;
}

void Ddr3Controller::notify_lifecycle() {
    if (sc_core::sc_is_running()) lifecycle_event_.notify(sc_core::SC_ZERO_TIME);
}

bool Ddr3Controller::port_owns_work(unsigned port) const { return !ports_[port].live.empty(); }

bool Ddr3Controller::core_owns_work() const { return core_work_ != 0; }

void Ddr3Controller::update_lifecycle() {
    bool changed = false;
    auto set = [&changed](bool& level, bool value) {
        if (level != value) {
            level = value;
            changed = true;
        }
    };
    // A core reset request stops admission on every port; the request is safe only once
    // every enabled port has drained (LC-7.1-03).
    bool all_ports_drained = true;
    bool reset_drained = true;  // fatal-contained work does not hold off reset-safe
    for (unsigned p = 0; p < kPortBundles; ++p) {
        const bool quiesce = port_quiesce_req_[p] || csr_port_quiesce_[p] || pub_quiesce_;
        // A quiesce request invalidates the port's exclusive reservations (FE-6.1.12-10).
        if (quiesce) excl_.invalidate_port(p);
        // HAS §6.1.14: ack = request and every ownership term clear (FE-6.1.14-02).
        set(port_quiesce_ack_[p], quiesce && !port_owns_work(p));
        if (soft_reset_pending_[p] && port_quiesce_ack_[p] && !soft_reset_active_[p]) {
            start_soft_reset(p);
            changed = true;
        }
        if (p < cfg_.n_ports && port_owns_work(p)) all_ports_drained = false;
        if (p < cfg_.n_ports && uncontained_contexts(p) != 0) reset_drained = false;
    }
    // reset_safe = request ∧ released ∧ local_idle ∧ endpoint_aligned ∧ online (HAS §7.1);
    // endpoint alignment and online are always true in a single-kernel VP.
    set(core_reset_safe_, core_reset_req_ && !cold_reset_ && !core_reset_ && core_work_ == core_contained_ &&
                              reset_drained);
    set(csr_reset_safe_, csr_reset_req_ && !cold_reset_ && !csr_reset_ && mutex_owner_ != MutexOwner::Csr &&
                             !snapshot_busy_);
    // HAS Table 1-3 levels: frontend drained (no accepted transaction on any port) and
    // global idle (additionally nothing in backend service).
    const bool drained = all_ports_drained;
    const bool idle = drained && !core_owns_work();
    if (drained != fe_drained_ || idle != global_idle_) {
        fe_drained_ = drained;
        global_idle_ = idle;
        request_outputs();
        changed = true;
    }
    if (changed) notify_lifecycle();
}

void Ddr3Controller::start_soft_reset(unsigned port) {
    soft_reset_pending_[port] = false;
    soft_reset_active_[port] = true;
    scheduler_.bump(kDomainPort0 + port);
    // The hold runs to completion even if the request drops early (FE-6.1.14-04).
    scheduler_.schedule(cycles(kSoftResetHoldCycles), kDomainPort0 + port, [this, port] {
        soft_reset_active_[port] = false;
        ++soft_reset_done_[port];  // completion identity for a waiting FE_PORT_CMD
        // MAS T6-64 (FE C14): a port soft reset also resets the port QoS table.
        qos_lut_[port].fill(kQosClassNormal);
        qos_configured_[port] = false;
        update_lifecycle();
        notify_lifecycle();
    });
}

void Ddr3Controller::storage_changed() {
    if (storage_listener_) storage_listener_();
}

RegionMap Ddr3Controller::decode_map(unsigned port) const {
    check_port(port, "decode_map");
    return effective_map(port);
}

void Ddr3Controller::apply_core_reset() {
    // Core domain: lifecycle state, destination images and pending completions are lost;
    // the CSR shadows survive and firmware commits again (CSR-8.1-22, LC-7.1-05).
    scheduler_.bump(kDomainCore);
    for (unsigned p = 0; p < kPortBundles; ++p) {
        scheduler_.bump(kDomainPort0 + p);
        soft_reset_active_[p] = false;
        soft_reset_pending_[p] = false;
    }
    topo_valid_ = timing_valid_ = dfi_valid_ = region_valid_ = false;
    topo_active_ = 0;
    geom_ = Geometry{};
    timing_active_.fill(0);
    dfi_active_ = DfiImage{};
    region_active_.fill(0);
    global_quiesce_req_ = false;
    csr_port_quiesce_.fill(false);
    pub_quiesce_ = false;
    // Data path: accepted transactions abort (their threads clean up their own contexts,
    // locks and slots); reservations, open rows and the ECC/service counters are core state.
    // DRAM contents, the quarantine and fatal records survive (RS10: cold reset only).
    excl_.clear();
    storage_changed();  // the map returns to the bootstrap image; initialization restarts
    bank_.reset();
    ce_count_ = ue_count_ = 0;
    ecc_last_addr_ = 0;
    ecc_last_info_ = 0;
    service_count_.fill(0);
    power_drain_pending_ = false;
    if (sc_core::sc_is_running()) mem_event_.notify(sc_core::SC_ZERO_TIME);
    phy_failure_ = false;
    recovery_required_ = false;
    if (power_transition_) {
        // Retire the power transaction: free the mutex it owns (and only that), invalidate
        // its token and answer the pending P-channel request with a denial (S2-R2).
        mutex_release(power_mutex_token_);
        power_mutex_token_ = 0;
        ++power_token_;
        power_transition_ = false;
        power_wait_phy_ = false;
        power_last_deny_ = true;
        power_last_accept_ = false;
        if (pch_.preq) pch_.pdeny = true;
        if (sc_core::sc_is_running()) pchannel_event_.notify(sc_core::SC_ZERO_TIME);
    }
    power_resident_ = FX1_DDR3_PSTATE_ACTIVE;
    scheduler_.bump(kDomainPower);
    init_restart();
    wake_commands();
}

void Ddr3Controller::apply_csr_reset() {
    // CSR domain: every CSR-side word returns to its reset value; commands in flight are
    // abandoned (epoch). The core keeps its installed images (VP-local, decision log).
    scheduler_.bump(kDomainCsr);
    storage_.clear();
    sticky_set_time_.clear();
    for (const CsrDesc& d : csr_table())
        if (d.source == CsrSource::Storage) storage_[d.offset] = d.reset;
    irq_raw_ = 0;
    irq_set_time_.fill(sc_core::SC_ZERO_TIME);
    // CSR commands are abandoned with the CSR domain; a mutex owned by a core-side power
    // transaction is not a CSR-domain owner and stays held.
    if (mutex_owner_ == MutexOwner::Csr) {
        mutex_owner_ = MutexOwner::None;
        mutex_token_ = 0;
    }
    snapshot_busy_ = false;
    snapshot_valid_ = false;
    snapshot_seq_ = 0;
    snapshot_.clear();
    topo_status_ = timing_status_ = dfi_status_ = region_status_ = 0;
    fe_port_status_ = fe_port_state_ = 0;
    irq_update();
    wake_commands();
}

void Ddr3Controller::apply_port_reset(unsigned port) {
    ++port_reset_gen_[port];
    scheduler_.bump(kDomainPort0 + port);
    soft_reset_active_[port] = false;
    soft_reset_pending_[port] = false;
    excl_.invalidate_port(port);
    if (sc_core::sc_is_running()) mem_event_.notify(sc_core::SC_ZERO_TIME);  // abort its transactions
}

void Ddr3Controller::set_cold_reset(bool asserted) {
    cold_reset_ = asserted;
    if (asserted) {
        apply_csr_reset();
        for (unsigned p = 0; p < kPortBundles; ++p) apply_port_reset(p);
        for (auto& lut : qos_lut_) lut.fill(kQosClassNormal);
        qos_configured_.fill(false);
        fatal_status_ = 0;  // RS10: quarantine and fatal records clear only on cold reset
        quarantine_active_ = false;
        quarantine_reason_ = 0;
        quarantine_lo_ = quarantine_hi_ = 0;
        fatal_ = false;
        fatal_locks_.clear();
        apply_core_reset();
    } else {
        init_step();
    }
    update_lifecycle();
    notify_lifecycle();
}

void Ddr3Controller::set_core_reset(bool asserted) {
    core_reset_ = asserted;
    if (asserted) apply_core_reset();
    else init_step();
    update_lifecycle();
    notify_lifecycle();
}

void Ddr3Controller::set_csr_reset(bool asserted) {
    csr_reset_ = asserted;
    if (asserted) apply_csr_reset();
    update_lifecycle();
    notify_lifecycle();
}

void Ddr3Controller::set_port_reset(unsigned port, bool asserted) {
    check_port(port, "set_port_reset");
    port_reset_[port] = asserted;
    if (asserted) apply_port_reset(port);
    update_lifecycle();
    notify_lifecycle();
}

void Ddr3Controller::core_reset_request(bool request) {
    core_reset_req_ = request;
    update_lifecycle();
    notify_lifecycle();
}

void Ddr3Controller::csr_reset_request(bool request) {
    csr_reset_req_ = request;
    update_lifecycle();
}

bool Ddr3Controller::core_reset_safe() const { return core_reset_safe_; }
bool Ddr3Controller::csr_reset_safe() const { return csr_reset_safe_; }

void Ddr3Controller::port_quiesce_request(unsigned port, bool request) {
    check_port(port, "port_quiesce_request");
    port_quiesce_req_[port] = request;
    update_lifecycle();
    notify_lifecycle();
}

bool Ddr3Controller::port_quiesce_ack(unsigned port) const {
    check_port(port, "port_quiesce_ack");
    return port_quiesce_ack_[port];
}

void Ddr3Controller::port_soft_reset_request(unsigned port, bool request) {
    check_port(port, "port_soft_reset_request");
    const bool rising = request && !soft_reset_req_[port];
    soft_reset_req_[port] = request;
    if (rising && !soft_reset_active_[port]) soft_reset_pending_[port] = true;
    // A request dropped before the ack never started a reset (FE-6.1.14-03).
    if (!request && !soft_reset_active_[port]) soft_reset_pending_[port] = false;
    update_lifecycle();
}

bool Ddr3Controller::port_soft_reset_pending(unsigned port) const {
    check_port(port, "port_soft_reset_pending");
    return soft_reset_pending_[port];
}

bool Ddr3Controller::port_soft_reset_active(unsigned port) const {
    check_port(port, "port_soft_reset_active");
    return soft_reset_active_[port];
}

} // namespace fx1::ddr3
