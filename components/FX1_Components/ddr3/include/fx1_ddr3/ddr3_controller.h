// SPDX-License-Identifier: Apache-2.0
#pragma once
// FTEL DDR3 controller VP model: SystemC/TLM wrapper (see docs/architecture.md).
//
// Functional blocks: the CSR map with commits, mutex, snapshots and interrupts; the
// lifecycle (initialization, training, startup scrub, readiness, refresh debt, P-channel
// power); and the memory data path (frontend decode/ordering/exclusive monitor, backend
// mapping, approximate bank timing, SECDED companion storage, RMW, read quarantine, fatal
// containment) with host debug and fault-injection APIs.
#include "fx1_ddr3/address_map.h"
#include "fx1_ddr3/axi_extension.h"
#include "fx1_ddr3/bank_timer.h"
#include "fx1_ddr3/config_checks.h"
#include "fx1_ddr3/csr_map.h"
#include "fx1_ddr3/ecc_codec.h"
#include "fx1_ddr3/epoch_scheduler.h"
#include "fx1_ddr3/exclusive_monitor.h"
#include "fx1_ddr3/frontend.h"
#include "fx1_ddr3/functional_phy.h"
#include "fx1_ddr3/granule_store.h"
#include "fx1_ddr3/profile.h"
#include "fx1_ddr3/speed_bin.h"
#include "fx1_ddr3/timebase.h"

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fx1::ddr3 {

constexpr unsigned kPortBundles = 4;   // FE01: four physical port positions
constexpr unsigned kRegionsPerPort = 4;
constexpr std::uint32_t kCsrApertureBytes = 0x10000;  // 16-bit AXI4-Lite address
constexpr unsigned kIrqSources = 12;

// HAS Table 1-4 QoS class: a 2-bit code per QoS index. NORMAL = 1 is derivable (FE05,
// MAS QOS_LUT reset 0x55555555); the other code names are PROVISIONAL (SOQ-13,
// provisional_encodings.h). The model stores the code and does not prioritise by it.
using QosClass = std::uint8_t;
constexpr QosClass kQosClassNormal = 1;
constexpr QosClass kQosClassMax = 3;

// P-channel request/response (RD-PWR-01). pstate encoding: provisional_encodings.h.
struct PChannelState {
    bool preq = false;
    std::uint8_t pstate = 0;
    bool paccept = false;
    bool pdeny = false;
    bool pactive = true;
};

class Ddr3Controller : public sc_core::sc_module, private PhyClient {
public:
    using MemSocket = tlm_utils::simple_target_socket_tagged_optional<Ddr3Controller>;
    template <typename T>
    using OptionalOut = sc_core::sc_port<sc_core::sc_signal_inout_if<T>, 1, sc_core::SC_ZERO_OR_MORE_BOUND>;

    // AXI4-Lite CSR target: 16-bit offsets (the bus removes the aperture base).
    tlm_utils::simple_target_socket<Ddr3Controller> csr_socket{"csr_socket"};
    // AXI4 memory ports 0..3; only the first BuildConfig::n_ports may be bound.
    sc_core::sc_vector<MemSocket> mem_socket{"mem_socket", kPortBundles};
    // Active-high level interrupt (HAS §8.5).
    sc_core::sc_out<bool> irq{"irq"};
    // Lifecycle levels (HAS Table 1-3, BE08); optional, also readable by getters.
    OptionalOut<bool> mission_ready_out{"mission_ready_out"};
    OptionalOut<bool> global_idle_out{"global_idle_out"};
    OptionalOut<bool> fe_drained_out{"fe_drained_out"};
    OptionalOut<bool> init_complete_out{"init_complete_out"};

    SC_HAS_PROCESS(Ddr3Controller);
    // `phy` may be null: the controller then owns a FunctionalPhy with default config.
    Ddr3Controller(sc_core::sc_module_name name, const BuildConfig& cfg,
                   PhyProvider* phy = nullptr);
    ~Ddr3Controller() override;

    const BuildConfig& config() const noexcept { return cfg_; }
    const TimeBase& timebase() const noexcept { return timebase_; }
    PhyProvider& phy() noexcept { return *phy_; }
    // The owned default PHY, or null when an external provider was supplied.
    FunctionalPhy* functional_phy() noexcept { return own_phy_.get(); }
    EpochScheduler& scheduler() noexcept { return scheduler_; }

    // ---- Sideband controls (HAS Table 1-3 / 1-4); driven by DDR_SYSCTRL or a bench ----
    // Bootstrap region map (DEC-07). It is the effective decode map from cold reset until
    // the first successful FE_REGION_COMMIT, and again after core offline. It is never
    // copied into the CSR image: FE_REGION_ACTIVE and FE_REGION_STATUS[16] read 0 until
    // a commit installs one.
    void set_bootstrap_region(unsigned port, unsigned region, RegionEntry entry);
    RegionEntry bootstrap_region(unsigned port, unsigned region) const;
    // QoS lookup programming: one entry per call, then mark the table configured. A port
    // admits no request until its table is configured (FE05).
    void qos_write(unsigned port, unsigned qos_index, QosClass cls);
    void qos_set_configured(unsigned port, bool configured);
    bool qos_configured(unsigned port) const;
    QosClass qos_class(unsigned port, unsigned qos_index) const;
    // Reserved-rate token bucket of one port (20-bit rate and depth).
    void set_reserved_rate(unsigned port, std::uint32_t rate, std::uint32_t depth);
    // P-channel (RD-PWR-01) as seen by the system power controller: a 4-phase handshake.
    // Raise preq with pstate; PACCEPT or PDENY rises and is held until preq falls; the
    // next request may start once both responses are low.
    void pchannel_request(bool preq, std::uint8_t pstate);
    PChannelState pchannel() const { return pch_; }
    // Notified (delta) whenever PACCEPT, PDENY or PACTIVE changes.
    const sc_core::sc_event& pchannel_event() const noexcept { return pchannel_event_; }

    // ---- Reset and quiesce bundle (HAS Table 1-3, §7.1 Table 7-4, §6.1.14; IF-1.5-04) ----
    // See docs/interfaces.md §5. Reset inputs are levels with `true` = asserted (the
    // active-low pins inverted). The model powers up as if every reset had been pulsed
    // before time 0. DRAM contents are never erased by a reset.
    void set_cold_reset(bool asserted);                   // cold mailbox reset: whole IP
    void set_core_reset(bool asserted);                   // mission/core rst_n
    void set_csr_reset(bool asserted);                    // csr_aresetn
    void set_port_reset(unsigned port, bool asserted);    // aresetn of one AXI port domain
    // Coordinated reset request -> reset-safe acknowledgement (LC-7.1-02/03).
    void core_reset_request(bool request);
    void csr_reset_request(bool request);
    bool core_reset_safe() const;
    bool csr_reset_safe() const;
    // External per-port quiesce (level, released by dropping it) and warm/soft reset.
    // A soft reset starts only while the port's quiesce is acknowledged and then holds for
    // 4 x SYNC_STAGES = 8 controller cycles even if the request drops (FE-6.1.14-03/04).
    void port_quiesce_request(unsigned port, bool request);
    bool port_quiesce_ack(unsigned port) const;
    void port_soft_reset_request(unsigned port, bool request);
    bool port_soft_reset_pending(unsigned port) const;   // requested, waiting for the ack
    bool port_soft_reset_active(unsigned port) const;    // hold in progress
    // Notified (delta) whenever a reset input, a reset/quiesce request, reset-safe,
    // quiesce-ack, soft-reset or readiness level changes.
    const sc_core::sc_event& lifecycle_event() const noexcept { return lifecycle_event_; }

    // ---- Lifecycle observations (also driven on the optional output ports) ----
    bool mission_ready() const noexcept { return mission_ready_; }
    bool global_idle() const noexcept { return global_idle_; }
    bool fe_drained() const noexcept { return fe_drained_; }
    bool init_complete() const noexcept { return init_complete_; }
    bool irq_level() const noexcept { return irq_level_; }
    // Host-side diagnostics (benches, platform logs); not a CSR encoding.
    unsigned init_stage() const noexcept { return init_stage_; }
    bool init_failed() const noexcept { return init_failed_; }
    bool training_complete() const noexcept { return training_complete_; }
    bool scrub_complete() const noexcept { return scrub_complete_; }
    bool scrub_running() const noexcept { return scrub_running_; }
    // Startup scrub in progress: granules (data + companion) written so far.
    std::uint64_t scrub_progress() const;
    std::uint64_t scrub_total() const noexcept { return scrub_total_; }
    unsigned power_resident() const noexcept { return power_resident_; }
    bool recovery_required() const noexcept { return recovery_required_; }
    std::uint32_t irq_raw() const noexcept { return irq_raw_; }
    int refresh_debt(unsigned rank) const;
    const Geometry& geometry() const noexcept { return geom_; }
    bool images_valid() const noexcept { return topo_valid_ && timing_valid_ && dfi_valid_; }
    // Effective decode map of a port: the committed region image, or the bootstrap map
    // before the first commit and after a core reset (DEC-07). A global address in entry r
    // has user offset `addr - base` (regions alias, L-13).
    RegionMap decode_map(unsigned port) const;

    // ---- Platform coherence hook (host side; not reachable from firmware) ----
    // Called synchronously whenever the controller changes what a DDR address designates or
    // holds: a core or cold reset, an initialization (re)start (followed, with ECC, by the
    // zeroing startup scrub), the start of the startup scrub, and the installation of a
    // region image. A platform-level exclusive monitor drops its DDR reservations here, so
    // that CPU atomics and the controller's AXI exclusives see the same storage changes.
    void set_storage_listener(std::function<void()> listener) { storage_listener_ = std::move(listener); }

    // ---- Host debug access: preload / dump (plan §4.4; not reachable from firmware) ----
    // Offsets are user (region-normalized) byte offsets in [0, USABLE_GRANULES x 32) of
    // the committed geometry; false (nothing transferred) without a committed topology or
    // outside that range. Writes regenerate the companion ECC record of every touched
    // granule (consistent ECC); reads return the stored bytes without correction. No
    // interrupt, admission, ordering or exclusive state is involved. A preload issued
    // before the startup scrub has passed the data is erased by it, as in hardware.
    bool debug_write(std::uint64_t user_offset, const std::uint8_t* data, std::size_t length);
    bool debug_read(std::uint64_t user_offset, std::uint8_t* data, std::size_t length);
    std::size_t resident_pages() const noexcept { return store_.resident_pages(); }

    // ---- Host fault injection (testbench / platform CLI; not reachable from firmware) ----
    // While blocked, refresh deadlines of `rank` are missed and its debt grows (BE04).
    void fault_block_refresh(unsigned rank, bool blocked);
    // Corrupt stored bits without updating the ECC record: data bit 0..255 of a user
    // granule, or check bit 0..7 of one lane in the granule's companion record (ECC only).
    // Returns false when the granule is not addressable.
    bool fault_flip_data_bit(std::uint64_t user_granule, unsigned bit);
    bool fault_flip_check_bit(std::uint64_t user_granule, unsigned lane, unsigned bit);
    // Read-return faults (HAS Table 6-40, MAS Table 6-135), applied to the first granule
    // read of each of the next `count` AXI read transactions entering the backend (the
    // internal reads of ECC writes are not targeted). Timeout: the return never arrives and the head
    // deadline (issue + READ_TIMEOUT tCK) quarantines; Malformed / ContextMismatch /
    // GenerationMismatch: the return is rejected when it arrives. Unexpected is a
    // tokenless return injected at once (count ignored).
    enum class ReturnFault { Timeout, Malformed, Unexpected, ContextMismatch, GenerationMismatch };
    void fault_read_return(ReturnFault fault, unsigned count = 1);
    // The next `count` granule writes report a DFI write-result error: SLVERR, or the ECC
    // fatal hold for an ECC write (the write is never rolled back, HAS BE02).
    void fault_write_result(unsigned count = 1);
    // Fatal sources (FATAL_STATUS): raised at once, except ScrubMapping, which fails the
    // running startup scrub (or the next one to start).
    enum class FatalFault { DfiProtocol, WriteDataProtocol, WriteResultUnmatched, ScrubMapping, StaleLockRelease };
    void fault_fatal(FatalFault fault);
    void fault_clear();  // disarm pending read/write/scrub faults

    // ---- Data-path observations (benches, platform logs) ----
    std::uint32_t ce_count() const noexcept { return ce_count_; }
    std::uint32_t ue_count() const noexcept { return ue_count_; }
    bool quarantined() const noexcept { return quarantine_active_; }
    bool fatal() const noexcept { return fatal_; }
    std::uint32_t fatal_status() const noexcept { return fatal_status_; }
    const BankTimer& bank_timer() const noexcept { return bank_; }

protected:
    // Owned memory work: accepted transactions of a port (frontend contexts) and requests
    // in backend service (HAS §6.1.14 ownership terms, §7.1 local idle).
    virtual bool port_owns_work(unsigned port) const;
    virtual bool core_owns_work() const;

private:
    // Socket callbacks.
    void csr_b_transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay);
    unsigned csr_transport_dbg(tlm::tlm_generic_payload& tx);
    void mem_b_transport(int port, tlm::tlm_generic_payload& tx, sc_core::sc_time& delay);
    unsigned mem_transport_dbg(int port, tlm::tlm_generic_payload& tx);

    // PhyClient
    void phy_observation(const PhyObservation& obs) override;
    void phy_levels_changed(const PhyLevels& levels) override;

    void end_of_elaboration() override;
    void start_of_simulation() override;
    void drive_outputs();    // SC_METHOD: the only writer of the output ports
    void request_outputs();  // schedule drive_outputs (state may change in any process)
    void check_port(unsigned port, const char* api) const;
    void notify_lifecycle();
    sc_core::sc_time cycles(std::uint64_t controller_cycles) const;
    sc_core::sc_time tck_wait(std::uint64_t n_tck) const;

    // ---- reset / quiesce (ddr3_controller.cpp)
    bool port_in_reset(unsigned port) const;
    bool admission_stopped(unsigned port) const;
    void update_lifecycle();
    void start_soft_reset(unsigned port);
    void apply_core_reset();
    void apply_csr_reset();
    void apply_port_reset(unsigned port);

    // ---- interrupts and sticky words (ddr3_controller.cpp)
    void irq_set(unsigned source);
    void irq_clear(std::uint32_t mask);
    void irq_update();
    void sticky_set(std::uint16_t offset, std::uint32_t bits);
    void sticky_clear(std::uint16_t offset, std::uint32_t bits);
    void command_error(std::uint32_t bit);

    // ---- CSR (ddr3_csr.cpp)
    enum class Resp { Okay, SlvErr, DecErr };
    Resp csr_read(std::uint32_t offset, std::uint32_t& value, bool debug);
    Resp csr_write(std::uint32_t offset, std::uint32_t data, std::uint8_t strobes);
    std::uint32_t storage(std::uint16_t offset) const;
    void set_storage(std::uint16_t offset, std::uint32_t value);
    std::uint32_t live_value(std::uint16_t offset) const;
    std::uint32_t snapshot_source(std::uint16_t offset) const;
    Resp write_policy(std::uint16_t offset, std::uint32_t merged);
    Resp write_init_rw(std::uint16_t offset, std::uint32_t merged);
    Resp command(std::uint16_t offset, std::uint32_t data);
    // Command mutex with an owner (HAS §8.4): CSR commands and P-channel power transitions.
    // A token identifies one ownership; only its owner (or the reset of its domain) frees it.
    enum class MutexOwner : std::uint8_t { None, Csr, Power };
    std::uint64_t mutex_acquire(MutexOwner owner);  // 0 when busy (CSR: COMMAND_ERROR[0])
    void mutex_release(std::uint64_t token);
    bool mutex_busy() const noexcept { return mutex_owner_ != MutexOwner::None; }
    // Epochs captured when a command is accepted; every wait re-checks them so a reset
    // retires the command with exactly one terminal outcome.
    struct CmdGuard {
        std::uint32_t csr;
        std::uint32_t core;
    };
    CmdGuard guard() const;
    bool csr_alive(const CmdGuard& g) const;
    bool core_alive(const CmdGuard& g) const;
    Resp cmd_quiesce(std::uint32_t data);
    Resp cmd_snapshot();
    Resp cmd_fe_port(std::uint32_t data);
    Resp cmd_region_commit();
    Resp cmd_topo_commit();
    Resp cmd_timing_commit();
    Resp cmd_dfi_commit();
    Resp cmd_train(std::uint32_t data);
    Resp cmd_irq_test_set(std::uint32_t data);
    bool quiesce_ok() const;
    bool endpoint_wait(const CmdGuard& g);  // false when a CSR or core reset cancelled it
    void commit_reject(std::uint32_t& status, std::uint8_t blocker, bool acknowledged);
    // Terminal outcome of a command cancelled by a reset: a CSR reset already cleared the
    // CSR-side state; after a core reset the command reports the cancellation and frees
    // its own mutex token.
    void abandon_command(const CmdGuard& g, std::uint64_t token, std::uint32_t* status);
    void wake_commands();  // reset: blocked command threads re-check their guards
    void storage_changed();  // calls the platform listener (set_storage_listener)
    std::function<void()> storage_listener_;
    TimingImage timing_shadow() const;
    DfiImage dfi_shadow() const;
    void recheck_dfi_against_timing();

    // ---- memory data path (ddr3_memory.cpp)
    // Identity of an accepted transaction: a core reset or a reset of its port domain
    // aborts it (GAP-FE-21).
    struct MemGuard {
        std::uint32_t core;
        std::uint64_t port_gen;
        unsigned port;
    };
    struct PortContext {
        std::uint64_t seq;  // acceptance order within the port
        unsigned id;
        bool write;
        bool contained = false;  // held by fatal containment until a reset aborts it
    };
    struct PortState {
        std::vector<PortContext> live;
        std::uint64_t next_seq = 0;
    };
    // Per-atom backend work of one transaction.
    struct AtomWork {
        MapResult map{};
        AxiResp status = AxiResp::Okay;
        std::uint32_t mask = 0;            // write: strobed bytes
        std::array<std::uint8_t, kGranuleBytes> buf{};
        std::uint64_t rd_end = 0;          // tCK: internal reads done (RMW)
        std::uint64_t end = 0;             // tCK: data returned / written
        bool killed = false;               // read return lost to the quarantine
        bool written = false;
    };
    bool mem_alive(const MemGuard& g) const;
    bool backend_open() const;
    RegionMap effective_map(unsigned port) const;
    std::uint64_t now_tck() const;
    std::uint64_t sc_tck(const sc_core::sc_time& t) const;
    sc_core::sc_time tck_time(std::uint64_t tck) const;
    // Waits; false when the transaction was aborted by a reset.
    bool wait_same_id(unsigned port, std::uint64_t seq, unsigned id, const MemGuard& g);
    bool wait_read_order(unsigned port, std::uint64_t seq, const MemGuard& g);
    bool wait_backend(unsigned port, std::uint64_t seq, const MemGuard& g);
    // Fatal containment of work that has not launched: held until a reset aborts it.
    bool contain(unsigned port, std::uint64_t seq, bool in_core, const MemGuard& g);
    unsigned uncontained_contexts(unsigned port) const;
    bool wait_until_tck(std::uint64_t tck, bool stop_on_quarantine, const MemGuard& g);
    bool mem_transaction(unsigned port, std::uint64_t seq, tlm::tlm_generic_payload& tx, AxiExtension& ext,
                         const MemGuard& g, AxiResp& resp);
    bool backend_service(unsigned port, std::uint64_t seq, const AxiRequest& req, QosClass cls,
                         std::vector<Atom>& atoms, std::vector<AtomWork>& work, bool exclusive, bool& excl_ok,
                         const MemGuard& g);
    void retire_context(unsigned port, std::uint64_t seq);
    void core_work_done();
    std::uint32_t read_record(const MapResult& m);
    void write_record(const MapResult& m, std::uint32_t record);
    void ecc_event(bool ue, unsigned ce_codewords, std::uint64_t granule, const GranuleDecode& dec);
    void quarantine_start(unsigned reason, unsigned slot, unsigned gen, unsigned kind, std::uint64_t deadline);
    void fatal_event(std::uint32_t bits);
    void scrub_materialize(std::uint64_t writes);  // apply the first `writes` scrub writes
    void scrub_sync();                             // materialize the scrub up to now
    bool user_range(std::uint64_t offset, std::uint64_t length) const;
    void lock_release(std::uint64_t ticket, const std::vector<std::uint64_t>& granules);

    // ---- lifecycle (ddr3_lifecycle.cpp)
    void init_restart();
    void init_step();
    void init_enter(unsigned stage);
    void init_wait(unsigned stage, sc_core::sc_time duration, unsigned next);
    void init_fail(unsigned stage, bool training);
    void init_done();
    bool init_rw_locked() const;
    void training_start(std::vector<PhyLoadRequest> loads, std::function<void(bool, std::uint8_t)> done);
    void training_issue_next();
    void training_finish(bool ok, std::uint8_t code);
    void training_abort();  // retire the active episode without a callback
    std::vector<PhyLoadRequest> boot_training_loads() const;
    void scrub_start();
    void scrub_abort();
    void update_readiness();
    // Refresh (lazy, per rank).
    void refresh_restart();
    void refresh_reschedule(unsigned rank);
    int refresh_debt_at(unsigned rank, const sc_core::sc_time& t) const;
    unsigned refresh_limit() const;
    std::uint32_t refresh_rank_word(unsigned rank) const;
    std::uint32_t refresh_status_word() const;
    // Power (P-channel).
    void power_evaluate_request();
    void power_run_entry(std::uint8_t target);
    void power_run_exit();
    void power_complete(bool accept);
    void power_timeout_check(std::uint64_t token);
    void pchannel_update_pactive();

    // EpochScheduler domains.
    static constexpr unsigned kDomainCore = 0;
    static constexpr unsigned kDomainCsr = 1;
    static constexpr unsigned kDomainPort0 = 2;   // + port index (2..5)
    static constexpr unsigned kDomainInit = 6;
    static constexpr unsigned kDomainTrain = 7;
    static constexpr unsigned kDomainScrub = 8;
    static constexpr unsigned kDomainRefresh0 = 9;  // + rank (9..10)
    static constexpr unsigned kDomainPower = 11;
    static constexpr unsigned kSoftResetHoldCycles = 8;  // 4 x SYNC_STAGES (MAS Table 5-6)
    static constexpr unsigned kPhyResetCycles = 4;       // fixed PHY-reset wait (HAS §8.1)
    static constexpr unsigned kEndpointCycles = 4;       // VP-local endpoint/CDC latency
    static constexpr unsigned kCsrAccessCycles = 1;      // VP-local AXI4-Lite response latency
    static constexpr unsigned kFrontendCycles = 4;       // VP-local port + CDC latency, each way
    static constexpr unsigned kRequestSlots = 16;        // backend Request table (HAS §6.2.3)

    BuildConfig cfg_;
    TimeBase timebase_;
    ExtTimingImage ext_;
    std::unique_ptr<FunctionalPhy> own_phy_;
    PhyProvider* phy_;
    EpochScheduler scheduler_{"scheduler"};

    std::array<std::array<RegionEntry, kRegionsPerPort>, kPortBundles> bootstrap_{};
    std::array<std::array<QosClass, 16>, kPortBundles> qos_lut_{};
    std::array<bool, kPortBundles> qos_configured_{};
    std::array<std::uint32_t, kPortBundles> rsv_rate_{};
    std::array<std::uint32_t, kPortBundles> rsv_depth_{};
    PChannelState pch_{};
    sc_core::sc_event pchannel_event_;

    bool mission_ready_ = false;
    bool global_idle_ = true;
    bool fe_drained_ = true;
    bool init_complete_ = false;
    bool irq_level_ = false;

    // Reset/quiesce bundle state.
    bool cold_reset_ = false, core_reset_ = false, csr_reset_ = false;
    std::array<bool, kPortBundles> port_reset_{};
    bool core_reset_req_ = false, csr_reset_req_ = false;
    bool core_reset_safe_ = false, csr_reset_safe_ = false;
    std::array<bool, kPortBundles> port_quiesce_req_{};
    std::array<bool, kPortBundles> csr_port_quiesce_{};
    std::array<bool, kPortBundles> port_quiesce_ack_{};
    std::array<bool, kPortBundles> soft_reset_req_{};
    std::array<bool, kPortBundles> soft_reset_pending_{};
    std::array<bool, kPortBundles> soft_reset_active_{};
    std::array<std::uint64_t, kPortBundles> soft_reset_done_{};  // completed holds per port
    // Port-domain reset generation: counts every port reset assertion (including cold reset)
    // so a command can detect a reset pulse that began and ended while it waited. Separate
    // from the scheduler epoch, which a soft-reset hold also advances.
    std::array<std::uint64_t, kPortBundles> port_reset_gen_{};
    sc_core::sc_event lifecycle_event_;
    sc_core::sc_event outputs_event_;

    // CSR state.
    std::map<std::uint16_t, std::uint32_t> storage_;                 // Storage-source words
    std::map<std::uint16_t, std::array<sc_core::sc_time, 32>> sticky_set_time_;
    std::uint32_t irq_raw_ = 0;
    std::array<sc_core::sc_time, kIrqSources> irq_set_time_{};
    MutexOwner mutex_owner_ = MutexOwner::None;
    std::uint64_t mutex_token_ = 0;
    std::uint64_t mutex_next_token_ = 0;
    bool snapshot_busy_ = false;
    bool snapshot_valid_ = false;
    std::uint16_t snapshot_seq_ = 0;
    std::map<std::uint16_t, std::uint32_t> snapshot_;
    bool global_quiesce_req_ = false;
    bool admission_opened_ = false;  // mission admission opened since the last (re)init
    // Committed images.
    bool topo_valid_ = false, timing_valid_ = false, dfi_valid_ = false, region_valid_ = false;
    std::uint32_t topo_active_ = 0;
    Geometry geom_{};
    TimingImage timing_active_{};
    DfiImage dfi_active_{};
    RegionImage region_active_{};
    std::uint32_t topo_status_ = 0, timing_status_ = 0, dfi_status_ = 0, region_status_ = 0;
    std::uint32_t fe_port_status_ = 0, fe_port_state_ = 0;
    std::uint32_t fatal_status_ = 0;
    bool quarantine_active_ = false;
    unsigned quarantine_reason_ = 0;
    std::uint64_t quarantine_tck_ = 0;
    std::uint32_t quarantine_lo_ = 0, quarantine_hi_ = 0;
    bool pub_quiesce_ = false;  // FE_REGION_COMMIT publication quiesce on every port
    std::uint64_t pub_quiesce_token_ = 0;

    // Memory data path state.
    GranuleStore store_;
    BankTimer bank_;
    ExclusiveMonitor excl_;
    std::array<PortState, kPortBundles> ports_{};
    sc_core::sc_event mem_event_;   // context retired, lock released, slot freed, quarantine, reset
    // mem_event_ | lifecycle_event_, built once: the data-path waits on it without allocating.
    sc_core::sc_event_or_list mem_or_lifecycle_;
    unsigned core_work_ = 0;        // transactions in backend service
    unsigned core_contained_ = 0;   // ... of which held by fatal containment
    unsigned open_reads_ = 0;       // read transactions in backend service
    unsigned ecc_writes_ = 0;       // ECC write parents in service (ECC_CAP_STATUS[1])
    std::array<bool, kRequestSlots> slot_busy_{};
    std::array<std::uint8_t, kRequestSlots> slot_gen_{};
    std::uint64_t lock_ticket_ = 0;
    std::map<std::uint64_t, std::deque<std::uint64_t>> lock_queue_;  // per data granule, FIFO
    std::set<std::uint64_t> fatal_locks_;  // granules held by a post-launch ECC fatal (BE02)
    std::uint32_t ce_count_ = 0, ue_count_ = 0;
    std::uint64_t ecc_last_addr_ = 0;
    std::uint32_t ecc_last_info_ = 0;
    std::array<std::uint32_t, 3> service_count_{};  // BE, SRT, HRT (provisional class mapping)
    std::uint64_t scrub_applied_ = 0;  // scrub writes already applied to the store
    std::uint64_t scrub_ecc_base_ = 0; // companion base of the geometry being scrubbed
    std::deque<ReturnFault> return_faults_;
    unsigned write_faults_ = 0;
    bool scrub_fault_armed_ = false;
    bool power_drain_pending_ = false;  // power entry waits for the backend to drain

    // Lifecycle state.
    unsigned init_stage_ = 0;
    bool init_done_ = false, init_failed_ = false;
    bool init_waiting_level_ = false;  // a timed stage expired and waits for timing_wait_ok
    unsigned init_wait_next_ = 0;
    bool training_complete_ = false, training_busy_ = false, training_error_ = false;
    bool runtime_training_ = false;
    std::uint16_t train_epoch_ = 0;
    std::uint8_t train_tag_ = 0;
    std::vector<PhyLoadRequest> train_loads_;
    std::size_t train_next_ = 0;
    bool train_load_pending_ = false;
    PhyLoadRequest train_pending_{};
    std::function<void(bool, std::uint8_t)> train_done_;
    std::uint64_t train_episode_ = 0;      // active training episode (0 = none)
    std::uint64_t train_episode_seq_ = 0;
    sc_core::sc_event train_event_;      // runtime TRAIN_CMD completion
    bool train_finished_ = false;
    std::uint8_t train_code_ = 0;
    std::uint32_t train_status_ = 0, train_obs_ = 0;
    std::array<PhyObservation, 2> rank_training_context_{};
    bool scrub_complete_ = false, scrub_running_ = false;
    sc_core::sc_time scrub_start_;
    std::uint64_t scrub_total_ = 0;
    sc_core::sc_time scrub_write_time_;
    bool phy_failure_ = false;  // sticky PHY failure until reinit or reset (BE07)
    PhyLevels phy_levels_{};
    bool recovery_required_ = false;
    bool fatal_ = false;
    // Refresh (per rank).
    struct RefreshRank {
        sc_core::sc_time grid_origin;   // deadlines at grid_origin + k * tREFI
        bool held = false;              // self-refresh holds ordinary debt
        int held_debt = 0;
        bool fault_blocked = false;     // host fault injection
        bool pd_blocked = false;        // resident in power-down: refresh needs a wake
        bool blocked = false;           // fault_blocked || pd_blocked
        sc_core::sc_time blocked_since;
        int debt_at_block = 0;
        bool repaying = false;
        sc_core::sc_time repay_since;
        int debt_at_repay = 0;
        bool saturated = false;
    };
    void refresh_set_blocked(unsigned rank);
    void refresh_hold(bool hold);
    std::array<RefreshRank, 2> refresh_{};
    bool refresh_running_ = false;
    // Power.
    unsigned power_resident_ = 0;  // FX1_DDR3_PSTATE_*
    bool power_transition_ = false;
    bool power_last_accept_ = false, power_last_deny_ = false;
    std::uint8_t power_target_ = 0;
    std::uint64_t power_token_ = 0;
    bool power_cutoff_passed_ = false;  // irreversible CKE/entry step launched
    std::uint64_t power_mutex_token_ = 0;
    bool power_wait_phy_ = false;       // wake complete, waiting for PHY ready
    std::uint8_t power_from_ = 0;
    unsigned waiting_callers_ = 0;

    std::set<std::string> warned_;
    void warn_once(const std::string& what);
};

} // namespace fx1::ddr3
