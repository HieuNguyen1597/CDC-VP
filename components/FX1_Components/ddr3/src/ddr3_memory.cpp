// SPDX-License-Identifier: Apache-2.0
// Memory data path (HAS §6.1-§6.2): frontend acceptance, decode, same-ID and read-return
// order, exclusive monitor, backend mission gate, mapping, approximate bank timing, SECDED
// companion storage with read-modify-write, startup-scrub data effects, read quarantine,
// fatal containment, host debug access and fault injection.
//
// One b_transport call is one AXI transaction (docs/interfaces.md §2). Its life:
//   accepted (frontend context) -> same-ID turn -> [decode error: local completion]
//   -> backend gate (GLOBAL_CTRL.enable, mission readiness) -> request slot -> granule
//   locks in install order -> bank timing -> data effects at their modelled instants ->
//   locks released -> frontend latency -> read-return order -> response.
#include "fx1_ddr3/ddr3_controller.h"

#include "fx1_ddr3/fx1_ddr3_regs.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace fx1::ddr3 {

using sc_core::sc_time;

namespace {
constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

AxiResp worst(AxiResp a, AxiResp b) noexcept {
    // DECERR > SLVERR > EXOKAY > OKAY (HAS §6.1.13): the enum values are in that order.
    return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
}

unsigned popcount8(unsigned v) noexcept {
    unsigned n = 0;
    for (; v; v &= v - 1u) ++n;
    return n;
}

// SOQ-36 (provisional): frontend class code -> backend service class BE / SRT / HRT.
unsigned service_bucket(QosClass cls) noexcept {
    if (cls == FX1_DDR3_QOS_CLASS_HRT) return 2;
    if (cls == FX1_DDR3_QOS_CLASS_LATENCY) return 1;
    return 0;
}

std::uint32_t le32(const std::uint8_t* p) noexcept {
    return std::uint32_t{p[0]} | std::uint32_t{p[1]} << 8 | std::uint32_t{p[2]} << 16 | std::uint32_t{p[3]} << 24;
}
} // namespace

// ---------------------------------------------------------------- helpers
std::uint64_t Ddr3Controller::now_tck() const {
    const double ps = sc_core::sc_time_stamp() / sc_time(1, sc_core::SC_PS);
    return static_cast<std::uint64_t>(ps) / timebase_.tck_ps;
}

std::uint64_t Ddr3Controller::sc_tck(const sc_time& t) const {
    return static_cast<std::uint64_t>(t / sc_time(1, sc_core::SC_PS)) / timebase_.tck_ps;
}

sc_time Ddr3Controller::tck_time(std::uint64_t tck) const {
    return sc_time(static_cast<double>(tck * timebase_.tck_ps), sc_core::SC_PS);
}

bool Ddr3Controller::mem_alive(const MemGuard& g) const {
    return scheduler_.epoch(kDomainCore) == g.core && port_reset_gen_[g.port] == g.port_gen &&
           !port_in_reset(g.port);
}

bool Ddr3Controller::backend_open() const {
    // MAS Table 6-73 admission terms that close the backend at once: GLOBAL_CTRL.enable and
    // mission readiness (PHY, init, training, scrub, power, fatal, recovery). Accepted
    // transactions wait here; they are never failed by a stop.
    return (storage(FX1_DDR3_GLOBAL_CTRL) & FX1_DDR3_GLOBAL_CTRL_ENABLE) != 0 && mission_ready_;
}

RegionMap Ddr3Controller::effective_map(unsigned port) const {
    // effective_map = installed ? active : bootstrap (HAS §6.1.15 T6-29, DEC-07).
    if (!region_valid_) return bootstrap_[port];
    RegionMap m{};
    for (unsigned r = 0; r < kRegionsPerPort; ++r)
        m[r] = {region_active_[2 * (4 * port + r)], region_active_[2 * (4 * port + r) + 1]};
    return m;
}

bool Ddr3Controller::user_range(std::uint64_t offset, std::uint64_t length) const {
    if (!topo_valid_) return false;
    const std::uint64_t limit = geom_.usable_granules * kGranuleBytes;
    return offset <= limit && length <= limit - offset;
}

// ---------------------------------------------------------------- waits
bool Ddr3Controller::wait_same_id(unsigned port, std::uint64_t seq, unsigned id, const MemGuard& g) {
    // FE-6.1.5-01/02: no memory access until every older context with the same ID (either
    // direction) has issued its terminal response.
    for (;;) {
        if (!mem_alive(g)) return false;
        bool older = false, older_contained = false;
        for (const PortContext& c : ports_[port].live) {
            if (c.seq >= seq || c.id != id) continue;
            older = true;
            older_contained |= c.contained;
        }
        if (!older) return true;
        // Behind a fatal-contained predecessor this transaction cannot proceed before a reset
        // either: it is contained too, so it does not hold off reset-safe (S3-R1a).
        if (fatal_ && older_contained) return contain(port, seq, false, g);
        sc_core::wait(mem_or_lifecycle_);
    }
}

bool Ddr3Controller::wait_read_order(unsigned port, std::uint64_t seq, const MemGuard& g) {
    // FE-6.1.8-01: R data of a port leaves in AR acceptance order across all IDs.
    for (;;) {
        if (!mem_alive(g)) return false;
        bool older = false, older_contained = false;
        for (const PortContext& c : ports_[port].live) {
            if (c.seq >= seq || c.write) continue;
            older = true;
            older_contained |= c.contained;
        }
        if (!older) return true;
        if (fatal_ && older_contained) return contain(port, seq, false, g);  // S3-R1a
        sc_core::wait(mem_or_lifecycle_);
    }
}

bool Ddr3Controller::wait_backend(unsigned port, std::uint64_t seq, const MemGuard& g) {
    if (fatal_) return contain(port, seq, false, g);
    if (backend_open()) return mem_alive(g);
    ++waiting_callers_;  // PACTIVE: pending work while resident in a low-power state
    pchannel_update_pactive();
    bool alive = true;
    while (alive && !backend_open() && !fatal_) {
        sc_core::wait(mem_or_lifecycle_);
        alive = mem_alive(g);
    }
    --waiting_callers_;
    pchannel_update_pactive();
    if (alive && fatal_) return contain(port, seq, false, g);
    return alive;
}

bool Ddr3Controller::contain(unsigned port, std::uint64_t seq, bool in_core, const MemGuard& g) {
    // Fatal containment (HAS §6.2.10, MAS Table 6-73: the work behind the fatal terms is
    // contained, not completed). Work that has not launched is held; only a reset of its
    // core or port domain (cold reset included) releases it, as an abort. Contained work
    // does not hold off reset-safe: the coordinated reset is the recovery path.
    auto mark = [&](bool v) {
        for (PortContext& c : ports_[port].live)
            if (c.seq == seq) c.contained = v;
    };
    mark(true);
    if (in_core) ++core_contained_;
    mem_event_.notify(sc_core::SC_ZERO_TIME);  // followers re-evaluate their dependencies
    update_lifecycle();
    while (mem_alive(g)) sc_core::wait(mem_or_lifecycle_);
    mark(false);
    if (in_core) --core_contained_;
    update_lifecycle();
    return false;
}

unsigned Ddr3Controller::uncontained_contexts(unsigned port) const {
    unsigned n = 0;
    for (const PortContext& c : ports_[port].live) n += c.contained ? 0u : 1u;
    return n;
}

bool Ddr3Controller::wait_until_tck(std::uint64_t tck, bool stop_on_quarantine, const MemGuard& g) {
    for (;;) {
        if (!mem_alive(g)) return false;
        if (stop_on_quarantine && quarantine_active_) return true;
        const sc_time target = tck_time(tck);
        const sc_time now = sc_core::sc_time_stamp();
        if (now >= target) return true;
        sc_core::wait(target - now, mem_event_);
    }
}

void Ddr3Controller::retire_context(unsigned port, std::uint64_t seq) {
    auto& live = ports_[port].live;
    live.erase(std::remove_if(live.begin(), live.end(), [seq](const PortContext& c) { return c.seq == seq; }),
               live.end());
    mem_event_.notify(sc_core::SC_ZERO_TIME);
    update_lifecycle();
}

void Ddr3Controller::core_work_done() {
    --core_work_;
    mem_event_.notify(sc_core::SC_ZERO_TIME);
    update_lifecycle();
    if (core_work_ != 0) return;
    notify_lifecycle();  // a runtime training command waits for the backend to drain
    if (power_drain_pending_) {
        power_drain_pending_ = false;
        if (power_transition_) power_run_entry(power_target_);
    }
}

void Ddr3Controller::lock_release(std::uint64_t ticket, const std::vector<std::uint64_t>& granules) {
    for (std::uint64_t d : granules) {
        auto it = lock_queue_.find(d);
        if (it == lock_queue_.end()) continue;
        auto& q = it->second;
        q.erase(std::remove(q.begin(), q.end(), ticket), q.end());
        if (q.empty()) lock_queue_.erase(it);
    }
    mem_event_.notify(sc_core::SC_ZERO_TIME);
}

// ---------------------------------------------------------------- AXI memory ports
void Ddr3Controller::mem_b_transport(int port, tlm::tlm_generic_payload& tx, sc_time& delay) {
    auto* ext = tx.get_extension<AxiExtension>();
    if (!ext) {
        // The IP boundary requires the AXI contract; generic traffic needs the bridge.
        tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        return;
    }
    if (static_cast<unsigned>(port) >= cfg_.n_ports) {
        SC_REPORT_ERROR(name(), "access on a disabled memory port");
        tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        return;
    }
    const unsigned p = static_cast<unsigned>(port);
    const AxiRequest& req = ext->req;
    // Model-usage errors (GAP-FE-18): rejected before any buffer access, not AXI responses.
    const bool cmd_ok = (tx.is_read() && !req.write) || (tx.is_write() && req.write);
    if (!cmd_ok || tx.get_address() != req.addr || req.id >= (1u << cfg_.id_width)) {
        warn_once("memory payload disagrees with its AXI extension (command, address or ID width)");
        tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        return;
    }
    if (!tx.get_data_ptr()) {
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    if (tx.get_data_length() != req.beats() * cfg_.axi_beat_bytes()) {
        warn_once("memory payload length must be (LEN+1) x AXI data bytes (lane image)");
        tx.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE);
        return;
    }
    if (tx.get_byte_enable_ptr() && tx.get_byte_enable_length() != tx.get_data_length()) {
        tx.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
        return;
    }
    // One owner per delay (architecture.md §4): consume the incoming annotation here.
    if (delay != sc_core::SC_ZERO_TIME) {
        sc_core::wait(delay);
        delay = sc_core::SC_ZERO_TIME;
    }
    auto reset_abort = [&](const char* why) {
        SC_REPORT_WARNING(name(), why);
        if (!req.write) std::memset(tx.get_data_ptr(), 0, tx.get_data_length());
        ext->resp = AxiResp::SlvErr;
        ext->beat_resp.assign(req.write ? 0u : req.beats(), AxiResp::SlvErr);
        tx.set_response_status(to_tlm_status(ext->resp));
    };
    if (port_in_reset(p)) return reset_abort("memory access while the port domain is in reset");
    // Frontend stop conditions block acceptance until the last one is released (HAS §7.1,
    // §6.1.14, §8.3, FE05; GAP-FE-04/05), and so does a port with every transaction
    // context in use (N_TAG, FE-6.1.1-02). A blocked call is not owned work.
    auto blocked = [&] { return admission_stopped(p) || ports_[p].live.size() >= cfg_.tags_per_port; };
    if (blocked()) {
        ++waiting_callers_;
        pchannel_update_pactive();
        while (blocked()) {
            sc_core::wait(mem_or_lifecycle_);
            if (port_in_reset(p)) {
                --waiting_callers_;
                pchannel_update_pactive();
                return reset_abort("memory access aborted: port domain entered reset");
            }
        }
        --waiting_callers_;
        pchannel_update_pactive();
    }
    // Accepted: the transaction owns a frontend context until its terminal response.
    const MemGuard g{scheduler_.epoch(kDomainCore), port_reset_gen_[p], p};
    PortState& ps = ports_[p];
    const std::uint64_t seq = ++ps.next_seq;
    ps.live.push_back({seq, req.id, req.write});
    update_lifecycle();
    AxiResp resp = AxiResp::SlvErr;
    const bool done = mem_transaction(p, seq, tx, *ext, g, resp);
    retire_context(p, seq);
    if (!done) return reset_abort("memory transaction aborted by reset (no response promised, GAP-FE-21)");
    ext->resp = resp;
    tx.set_response_status(to_tlm_status(resp));
}

bool Ddr3Controller::mem_transaction(unsigned port, std::uint64_t seq, tlm::tlm_generic_payload& tx,
                                     AxiExtension& ext, const MemGuard& g, AxiResp& resp) {
    const AxiRequest req = ext.req;
    const unsigned bus = cfg_.axi_beat_bytes();
    std::uint8_t* data = tx.get_data_ptr();
    const QosClass cls = qos_lut_[port][req.qos & 0xFu];  // captured at acceptance (FE-6.1.10-01)
    const DecodeResult dec = decode_burst(req, effective_map(port), bus);
    if (!wait_same_id(port, seq, req.id, g)) return false;

    if (dec.resp != AxiResp::Okay) {
        // Errored request: no backend atom, memory unchanged, one terminal response
        // (FE-6.1.3-12/13). A region decode miss raises IRQ_RAW[4] (SOQ-30).
        if (dec.resp == AxiResp::DecErr) irq_set(4);
        if (!req.write) {
            if (!wait_read_order(port, seq, g)) return false;
            std::memset(data, 0, tx.get_data_length());
            ext.beat_resp.assign(req.beats(), dec.resp);
        } else {
            ext.beat_resp.clear();
        }
        resp = dec.resp;
        return true;
    }

    std::vector<Atom> atoms = plan_atoms(req, dec.region_base, bus);
    std::vector<AtomWork> work(atoms.size());
    if (req.write) {
        // Enabled bytes = WSTRB within the beat lanes (FE-6.1.6-01/02/03).
        const std::uint8_t* be = tx.get_byte_enable_ptr();
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            for (const AtomByte& b : atoms[i].bytes) {
                if (be && be[b.payload] != TLM_BYTE_ENABLED) continue;
                work[i].mask |= 1u << b.offset;
                work[i].buf[b.offset] = data[b.payload];
            }
        }
    }
    const bool exclusive =
        req.exclusive && cfg_.exclusive_enable && ((cfg_.exclusive_port_mask >> port) & 1u) != 0;
    bool excl_ok = false;
    if (req.write) {
        bool emits = false;
        for (const AtomWork& w : work) emits |= w.mask != 0;
        if (!emits) {
            // Every strobe zero: no write atom is emitted (masked ordinals, MAS §6.1.6); the
            // B completes locally with OKAY in its same-ID turn. An exclusive write cannot
            // take a reservation without a taken atom: EXOKAY when it matches (GAP-FE-03),
            // and the reservation stays (S3-R2).
            const bool match = exclusive && excl_.exclusive_matches(port, req.id, atoms[0].granule,
                                                                     atoms[0].coverage, atoms.size() == 1);
            ext.beat_resp.clear();
            resp = match ? AxiResp::ExOkay : AxiResp::Okay;
            return true;
        }
    }
    if (!wait_backend(port, seq, g)) return false;
    if (!backend_service(port, seq, req, cls, atoms, work, exclusive, excl_ok, g)) return false;

    // Response construction (HAS §6.1.13): worst status, EXOKAY only for a successful
    // exclusive access with no error.
    if (req.write) {
        AxiResp r = AxiResp::Okay;
        for (const AtomWork& w : work) r = worst(r, w.status);
        if (exclusive && excl_ok && r == AxiResp::Okay) r = AxiResp::ExOkay;
        ext.beat_resp.clear();
        resp = r;
    } else {
        if (!wait_read_order(port, seq, g)) return false;
        std::memset(data, 0, tx.get_data_length());
        std::vector<AxiResp> beat(req.beats(), AxiResp::Okay);
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            const AtomWork& w = work[i];
            for (const AtomByte& b : atoms[i].bytes) {
                const unsigned n = b.payload / bus;
                beat[n] = worst(beat[n], w.status);
                if (w.status == AxiResp::Okay) data[b.payload] = w.buf[b.offset];
            }
        }
        // Sticky forward (SOQ-23): once a beat carries an error, later beats carry at least
        // that error; an error beat returns zero data (RS05).
        AxiResp carried = AxiResp::Okay;
        for (unsigned n = 0; n < req.beats(); ++n) {
            carried = worst(carried, beat[n]);
            beat[n] = carried;
            if (carried != AxiResp::Okay) std::memset(data + n * bus, 0, bus);
        }
        AxiResp r = carried;
        if (exclusive && excl_ok && r == AxiResp::Okay) {
            r = AxiResp::ExOkay;
            beat.assign(req.beats(), AxiResp::ExOkay);
        }
        ext.beat_resp = beat;
        resp = r;
    }
    return true;
}

bool Ddr3Controller::backend_service(unsigned port, std::uint64_t seq, const AxiRequest& req, QosClass cls,
                                     std::vector<Atom>& atoms, std::vector<AtomWork>& work, bool exclusive,
                                     bool& excl_ok, const MemGuard& g) {
    const bool ecc = cfg_.ecc_enable;
    ++core_work_;
    if (!req.write) ++open_reads_;
    update_lifecycle();
    // Request slot with a 4-bit generation (HAS §6.2.3/§6.2.4); full table = backpressure.
    unsigned slot = kRequestSlots;
    for (;;) {
        for (unsigned i = 0; i < kRequestSlots && slot == kRequestSlots; ++i)
            if (!slot_busy_[i]) slot = i;
        if (slot != kRequestSlots || !mem_alive(g) || fatal_) break;
        sc_core::wait(mem_or_lifecycle_);
    }
    std::uint64_t ticket = 0;
    std::vector<std::uint64_t> locked;
    bool ecc_write = false;
    auto finish = [&](bool alive) {
        if (ticket) lock_release(ticket, locked);
        if (slot != kRequestSlots) slot_busy_[slot] = false;
        if (ecc_write) --ecc_writes_;
        if (!req.write) --open_reads_;
        core_work_done();
        return alive;
    };
    if (slot == kRequestSlots) return finish(fatal_ && mem_alive(g) ? contain(port, seq, true, g) : false);
    slot_busy_[slot] = true;
    slot_gen_[slot] = static_cast<std::uint8_t>((slot_gen_[slot] + 1u) & 0xFu);  // incremented at install
    const unsigned gen = slot_gen_[slot];

    // Take boundary (S3-R3a). From here to the granule-lock enqueue nothing waits, so every
    // exclusive-monitor decision (exclusive-write verdict, ordinary-write invalidation,
    // exclusive-read reservation) happens in install (ticket) order, which is also the order
    // the data effects take (HAS §6.1.12, MAS §6.1.12 shared take boundary).
    // Exclusive-write verdict: whole take or whole fail (FE-6.1.12-13). A miss completes
    // locally with OKAY and no backend take, so no backend status (quarantine) applies.
    if (req.write && exclusive && map_granule(geom_, ecc, atoms[0].granule).status == MapStatus::Ok) {
        excl_ok = excl_.exclusive_write(port, req.id, atoms[0].granule, atoms[0].coverage, atoms.size() == 1);
        if (!excl_ok) return finish(true);
    }
    service_count_[service_bucket(cls)] += static_cast<std::uint32_t>(atoms.size());

    // Read quarantine closes read admission: new reads answer SLVERR with zero data and are
    // never mapped (MAS §6.2.1, Table 6-136). ECC writes need an internal read and end the
    // same way, with no write. Writes needing no old-data read continue.
    if (quarantine_active_ && (!req.write || ecc)) {
        for (AtomWork& w : work) w.status = AxiResp::SlvErr;
        return finish(true);
    }
    // Mapping (HAS Table 6-33): poisoned atoms issue no DRAM command.
    for (std::size_t i = 0; i < atoms.size(); ++i) {
        AtomWork& w = work[i];
        w.map = map_granule(geom_, ecc, atoms[i].granule);
        switch (w.map.status) {
        case MapStatus::Ok: break;
        case MapStatus::OutOfCapacity:
        case MapStatus::RowOverflow: w.status = AxiResp::DecErr; break;
        case MapStatus::GeomImpossible:
        case MapStatus::RangeFault:
            w.status = AxiResp::SlvErr;
            irq_set(4);  // mapping/configuration error event (GAP-BE-28)
            break;
        }
    }
    // Granule locks taken in install order (HAS §6.2.5 hazard, §6.2.18 lock): a request
    // waits until it heads the queue of every granule it touches.
    ticket = ++lock_ticket_;
    for (std::size_t i = 0; i < atoms.size(); ++i) {
        if (work[i].status != AxiResp::Okay) continue;
        if (req.write && work[i].mask == 0) continue;  // masked ordinal: no backend write
        const std::uint64_t d = atoms[i].granule;
        // An ordinary taken write atom clears the granule's reservations (FE-6.1.12-05).
        if (req.write && !exclusive) excl_.write_taken(d);
        if (std::find(locked.begin(), locked.end(), d) != locked.end()) continue;
        locked.push_back(d);
        lock_queue_[d].push_back(ticket);
    }
    if (!req.write && exclusive) {
        // An exclusive read that fits one granule creates or refreshes the reservation; a
        // multi-granule one creates none and clears the owner's entry (GAP-FE-03). EXOKAY
        // still needs an error-free read.
        const bool shape = atoms.size() == 1 && work[0].status == AxiResp::Okay;
        excl_ok = excl_.exclusive_read(port, req.id, shape ? atoms[0].granule : 0, shape ? atoms[0].coverage : 0,
                                       shape);
    }
    for (;;) {
        if (!mem_alive(g)) return finish(false);
        // Fatal containment: queued work never launches (S3-R1).
        if (fatal_) return finish(contain(port, seq, true, g));
        bool head = true;
        for (std::uint64_t d : locked) {
            head &= lock_queue_[d].front() == ticket && fatal_locks_.count(d) == 0;
        }
        if (head) break;
        sc_core::wait(mem_or_lifecycle_);
    }
    if (req.write && ecc && !locked.empty()) {
        ecc_write = true;
        ++ecc_writes_;
    }

    // Bank timing (DEC-02) from the service start; every granule access is one BL8 column
    // command. ECC reads fetch the companion; ECC writes read the companion (and the old
    // data for a partial write) before writing data and record (MAS Table 6-160).
    if (fatal_) return finish(contain(port, seq, true, g));  // a fatal event while queued
    const std::uint64_t t0 = now_tck();
    const BankTimingParams tp = BankTimingParams::from(timing_active_);
    // Refresh windows of each active rank (DEC-02): the lifecycle's grid, unless the
    // rank's refresh is blocked or held; repayment of debt keeps the rank busy.
    for (unsigned r = 0; r < geom_.ranks && r < 2; ++r) {
        const RefreshRank& rr = refresh_[r];
        const std::uint32_t trfc = timing_active_[kTRfc];
        std::uint64_t busy = 0;
        if (rr.repaying)
            busy = sc_tck(rr.repay_since) + std::uint64_t{trfc} * static_cast<unsigned>(std::max(0, rr.debt_at_repay));
        bank_.set_refresh(r, refresh_running_ && !rr.blocked && !rr.held, sc_tck(rr.grid_origin), timing_active_[kTRefi],
                          trfc, busy);
    }
    std::uint64_t rd_done = t0, end_all = t0;
    for (std::size_t i = 0; i < atoms.size(); ++i) {
        AtomWork& w = work[i];
        if (w.status != AxiResp::Okay || (req.write && w.mask == 0)) continue;
        if (!req.write) {
            w.end = bank_.access(t0, w.map.data, false, tp);
            if (ecc) w.end = std::max(w.end, bank_.access(t0, w.map.ecc, false, tp));
        } else if (ecc) {
            w.rd_end = bank_.access(t0, w.map.ecc, false, tp);
            if (w.mask != 0xFFFFFFFFu) w.rd_end = std::max(w.rd_end, bank_.access(t0, w.map.data, false, tp));
            w.end = bank_.access(w.rd_end, w.map.data, true, tp);
            w.end = std::max(w.end, bank_.access(w.rd_end, w.map.ecc, true, tp));
        } else {
            w.rd_end = t0;
            w.end = bank_.access(t0, w.map.data, true, tp);
        }
        rd_done = std::max(rd_done, w.rd_end);
        end_all = std::max(end_all, w.end);
    }

    if (!req.write) {
        // Read-return faults apply to the first issued read of the transaction.
        std::uint64_t wait_to = end_all;
        for (AtomWork& w : work) {
            if (w.status != AxiResp::Okay || return_faults_.empty()) continue;
            const ReturnFault f = return_faults_.front();
            return_faults_.pop_front();
            const unsigned kind = ecc ? FX1_DDR3_QUAR_KIND_RD_ECC : FX1_DDR3_QUAR_KIND_RD_DATA;
            const std::uint64_t deadline = t0 + storage(FX1_DDR3_READ_TIMEOUT);
            unsigned reason = FX1_DDR3_QUAR_REASON_TIMEOUT;
            std::uint64_t at = deadline;
            if (f == ReturnFault::Timeout) {
                w.end = kNever;  // the return never arrives
            } else {
                at = w.end;
                reason = f == ReturnFault::Malformed           ? FX1_DDR3_QUAR_REASON_MALFORMED
                         : f == ReturnFault::ContextMismatch   ? FX1_DDR3_QUAR_REASON_CONTEXT
                         : f == ReturnFault::GenerationMismatch ? FX1_DDR3_QUAR_REASON_GENERATION
                                                                : FX1_DDR3_QUAR_REASON_UNEXPECTED;
                w.killed = true;  // the rejected return completes nothing
            }
            wait_to = std::max(wait_to, at);
            const sc_time when = tck_time(at), now = sc_core::sc_time_stamp();
            const sc_time delay = when > now ? when - now : sc_core::SC_ZERO_TIME;
            scheduler_.schedule(delay, kDomainCore, [this, reason, slot, gen, kind, deadline] {
                quarantine_start(reason, slot, gen, kind, deadline);
            });
            break;
        }
        if (!wait_until_tck(wait_to, true, g)) return finish(false);
        // A return that arrived no later than the quarantine set point completed (a good pop
        // in the same cycle wins, MAS §6.2.11); later ones are discarded.
        const std::uint64_t tq = quarantine_active_ ? quarantine_tck_ : kNever;
        scrub_sync();
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            AtomWork& w = work[i];
            if (w.status != AxiResp::Okay) continue;
            if (w.killed || w.end > tq) {
                w.status = AxiResp::SlvErr;
                continue;
            }
            store_.read(atoms[i].granule, w.buf.data());
            if (ecc) {
                const GranuleDecode dec = decode_granule(w.buf.data(), read_record(w.map));
                if (dec.ue_lanes || dec.ce_lanes)
                    ecc_event(dec.ue_lanes != 0, popcount8(dec.ce_lanes), atoms[i].granule, dec);
                if (dec.ue_lanes) w.status = AxiResp::SlvErr;  // UE: SLVERR, zero data (BE01, RS05)
            }
        }
    } else {
        // ECC writes apply after their internal reads; a quarantine set before then ends the
        // parent with SLVERR and no write (MAS Table 6-136).
        if (ecc && !locked.empty()) {
            if (!wait_until_tck(rd_done, true, g)) return finish(false);
            if (quarantine_active_ && quarantine_tck_ <= rd_done) {
                for (AtomWork& w : work)
                    if (w.status == AxiResp::Okay && w.mask != 0) w.status = AxiResp::SlvErr;
                return finish(true);
            }
            // The irreversible data and record writes launch now: not after a fatal event
            // (the internal reads already issued are harmless).
            if (fatal_) return finish(contain(port, seq, true, g));
        }
        scrub_sync();
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            AtomWork& w = work[i];
            if (w.status != AxiResp::Okay || w.mask == 0) continue;
            const std::uint64_t d = atoms[i].granule;
            if (!ecc) {
                store_.write(d, w.buf.data(), w.mask);
            } else {
                std::array<std::uint8_t, kGranuleBytes> merged{};
                if (w.mask == 0xFFFFFFFFu) {
                    merged = w.buf;  // full write: no old-data read, no decode (GAP-BE-17)
                } else {
                    store_.read(d, merged.data());
                    const GranuleDecode dec = decode_granule(merged.data(), read_record(w.map));
                    unsigned needed = 0;  // lanes not fully overwritten
                    for (unsigned l = 0; l < kEccLanes; ++l)
                        if (((w.mask >> (8 * l)) & 0xFFu) != 0xFFu) needed |= 1u << l;
                    if (dec.ue_lanes & needed) {
                        // BE02 / ECC-CT-01: UE in a needed lane aborts before any write.
                        ecc_event(true, popcount8(dec.ce_lanes), d, dec);
                        w.status = AxiResp::SlvErr;
                        continue;
                    }
                    // A UE only in fully overwritten lanes is healed and counted (ECC-CT-02).
                    const unsigned ce = popcount8(dec.ce_lanes) + popcount8(dec.ue_lanes);
                    if (ce) ecc_event(false, ce, d, dec);
                    for (unsigned b = 0; b < kGranuleBytes; ++b)
                        if ((w.mask >> b) & 1u) merged[b] = w.buf[b];
                }
                store_.write(d, merged.data());
                write_record(w.map, ecc_record(merged.data()));
            }
            w.written = true;  // reservations were cleared at the take boundary
        }
        if (!wait_until_tck(end_all, false, g)) return finish(false);
        // DFI write results (injected): SLVERR, or the ECC fatal hold (lock kept, BE02).
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            AtomWork& w = work[i];
            if (!w.written || write_faults_ == 0) continue;
            --write_faults_;
            w.status = AxiResp::SlvErr;
            if (ecc) {
                fatal_locks_.insert(atoms[i].granule);
                fatal_event(FX1_DDR3_FATAL_ECC_HOLD);
            }
        }
    }
    finish(true);
    // Response path through the port (lumped port/CDC latency).
    sc_core::wait(cycles(kFrontendCycles));
    return mem_alive(g);
}

// ---------------------------------------------------------------- ECC storage and events
std::uint32_t Ddr3Controller::read_record(const MapResult& m) {
    std::array<std::uint8_t, kGranuleBytes> c{};
    store_.read(m.ecc_granule, c.data());
    return le32(c.data() + 4 * m.slot);
}

void Ddr3Controller::write_record(const MapResult& m, std::uint32_t record) {
    // Byte enables for the 4 bytes of slot s only: sibling records are unchanged (BE-6.2.19-08).
    std::array<std::uint8_t, kGranuleBytes> c{};
    for (unsigned b = 0; b < 4; ++b) c[4 * m.slot + b] = static_cast<std::uint8_t>(record >> (8 * b));
    store_.write(m.ecc_granule, c.data(), 0xFu << (4 * m.slot));
}

void Ddr3Controller::ecc_event(bool ue, unsigned ce_codewords, std::uint64_t granule, const GranuleDecode& dec) {
    // One event per parent (MAS ECC-EV-01..03, Table 6-156).
    if (ce_codewords) {
        const std::uint32_t before = ce_count_;
        const std::uint64_t sum = std::uint64_t{ce_count_} + ce_codewords;
        ce_count_ = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(sum);
        const std::uint32_t thr = storage(FX1_DDR3_CE_THRESHOLD);
        if (thr != 0 && before < thr && ce_count_ >= thr) irq_set(0);  // reaches (GAP-BE-16)
    }
    if (ue) {
        if (ue_count_ != 0xFFFFFFFFu) ++ue_count_;
        irq_set(1);
    }
    // ECC_LAST_*: last event wins; physical data granule; lowest affected lane (SOQ-06).
    const unsigned affected = dec.ue_lanes | dec.ce_lanes;
    unsigned lane = 0;
    while (lane < kEccLanes && !((affected >> lane) & 1u)) ++lane;
    if (lane == kEccLanes) lane = 0;
    const std::uint32_t syn = (dec.lane[lane].mismatch ? 0x80u : 0u) | (dec.lane[lane].syndrome & 0x7Fu);
    ecc_last_addr_ = granule;
    ecc_last_info_ = (ue ? FX1_DDR3_ECC_INFO_TYPE_UE : FX1_DDR3_ECC_INFO_TYPE_CE) |
                     (lane << FX1_DDR3_ECC_INFO_CODEWORD_SHIFT) | (syn << FX1_DDR3_ECC_INFO_SYNDROME_SHIFT);
}

// ---------------------------------------------------------------- quarantine and fatal
void Ddr3Controller::quarantine_start(unsigned reason, unsigned slot, unsigned gen, unsigned kind,
                                      std::uint64_t deadline) {
    if (quarantine_active_) return;  // the first cause is latched once (RS10)
    quarantine_active_ = true;
    quarantine_reason_ = reason;
    quarantine_tck_ = now_tck();
    // MAS Table 6-137 first-cause record.
    quarantine_lo_ = (slot & 0x3Fu) | ((gen & 0xFFFFu) << 8) | ((kind & 7u) << 24) | ((reason & 7u) << 28) | (1u << 31);
    quarantine_hi_ = static_cast<std::uint32_t>(deadline);
    irq_set(reason == FX1_DDR3_QUAR_REASON_TIMEOUT ? 2 : 3);
    mem_event_.notify(sc_core::SC_ZERO_TIME);  // reads in flight lose their returns
}

void Ddr3Controller::fatal_event(std::uint32_t bits) {
    // Sticky until cold reset; closes admission (HAS §6.2.10, BE-6.2.10-05).
    fatal_status_ |= bits;
    fatal_ = true;
    irq_set(11);
    update_readiness();
    notify_lifecycle();
}

// ---------------------------------------------------------------- startup scrub data effects
void Ddr3Controller::scrub_materialize(std::uint64_t writes) {
    // Scrub write order (HAS BE03, MAS Table 6-164): per group of 8 data granules, writes
    // 0..7 zero the data granules and write 8 zeroes their companion granule.
    if (writes <= scrub_applied_) return;
    const ScrubFrontier from = scrub_frontier(scrub_applied_), to = scrub_frontier(writes);
    store_.zero_range(from.data, to.data - from.data);
    store_.zero_range(scrub_ecc_base_ + from.companions, to.companions - from.companions);
    scrub_applied_ = writes;
}

void Ddr3Controller::scrub_sync() {
    if (scrub_running_) scrub_materialize(scrub_progress());
}

// ---------------------------------------------------------------- debug transport and host access
unsigned Ddr3Controller::mem_transport_dbg(int port, tlm::tlm_generic_payload& tx) {
    // GAP-FE-17: the effective region map selects the region; the whole range must lie in
    // it. Admission, timing, ordering and the exclusive monitor are bypassed.
    if (static_cast<unsigned>(port) >= cfg_.n_ports || !tx.get_data_ptr()) return 0;
    const std::uint64_t a = tx.get_address();
    const std::uint64_t len = tx.get_data_length();
    for (const RegionEntry& e : effective_map(static_cast<unsigned>(port))) {
        if (e.size == 0 || a < e.base || a - e.base >= e.size) continue;
        if (len > std::uint64_t{e.base} + e.size - a) return 0;
        const std::uint64_t off = a - e.base;
        const bool ok = tx.is_write() ? debug_write(off, tx.get_data_ptr(), static_cast<std::size_t>(len))
                                      : debug_read(off, tx.get_data_ptr(), static_cast<std::size_t>(len));
        return ok ? static_cast<unsigned>(len) : 0u;
    }
    return 0;
}

bool Ddr3Controller::debug_write(std::uint64_t off, const std::uint8_t* src, std::size_t length) {
    if (!src || !user_range(off, length)) return false;
    if (cfg_.ecc_enable && !scrub_complete_)
        warn_once("preload before the startup scrub completed: the scrub erases data it has not passed yet");
    scrub_sync();
    std::size_t done = 0;
    while (done < length) {
        const std::uint64_t d = (off + done) / kGranuleBytes;
        const unsigned first = static_cast<unsigned>((off + done) % kGranuleBytes);
        const auto n = static_cast<unsigned>(std::min<std::uint64_t>(kGranuleBytes - first, length - done));
        std::array<std::uint8_t, kGranuleBytes> g{};
        store_.read(d, g.data());
        std::memcpy(g.data() + first, src + done, n);
        store_.write(d, g.data());
        if (cfg_.ecc_enable) write_record(map_granule(geom_, true, d), ecc_record(g.data()));
        done += n;
    }
    return true;
}

bool Ddr3Controller::debug_read(std::uint64_t off, std::uint8_t* dst, std::size_t length) {
    if (!dst || !user_range(off, length)) return false;
    scrub_sync();
    std::size_t done = 0;
    while (done < length) {
        const std::uint64_t d = (off + done) / kGranuleBytes;
        const unsigned first = static_cast<unsigned>((off + done) % kGranuleBytes);
        const auto n = static_cast<unsigned>(std::min<std::uint64_t>(kGranuleBytes - first, length - done));
        std::array<std::uint8_t, kGranuleBytes> g{};
        store_.read(d, g.data());
        std::memcpy(dst + done, g.data() + first, n);
        done += n;
    }
    return true;
}

// ---------------------------------------------------------------- fault injection
bool Ddr3Controller::fault_flip_data_bit(std::uint64_t user_granule, unsigned bit) {
    if (bit >= 8 * kGranuleBytes || !user_range(user_granule * kGranuleBytes, kGranuleBytes)) return false;
    scrub_sync();
    std::array<std::uint8_t, kGranuleBytes> g{};
    store_.read(user_granule, g.data());
    g[bit / 8] = static_cast<std::uint8_t>(g[bit / 8] ^ (1u << (bit % 8)));
    store_.write(user_granule, g.data());
    return true;
}

bool Ddr3Controller::fault_flip_check_bit(std::uint64_t user_granule, unsigned lane, unsigned bit) {
    if (!cfg_.ecc_enable || lane >= kEccLanes || bit >= 8 ||
        !user_range(user_granule * kGranuleBytes, kGranuleBytes))
        return false;
    scrub_sync();
    const MapResult m = map_granule(geom_, true, user_granule);
    if (m.status != MapStatus::Ok) return false;
    write_record(m, read_record(m) ^ (1u << (8 * lane + bit)));
    return true;
}

void Ddr3Controller::fault_read_return(ReturnFault fault, unsigned count) {
    if (fault == ReturnFault::Unexpected) {
        // A tokenless return: reason and zero identity, valid = 1 (MAS §6.2.11).
        quarantine_start(FX1_DDR3_QUAR_REASON_UNEXPECTED, 0, 0, 0, 0);
        return;
    }
    for (unsigned i = 0; i < count; ++i) return_faults_.push_back(fault);
}

void Ddr3Controller::fault_write_result(unsigned count) { write_faults_ += count; }

void Ddr3Controller::fault_fatal(FatalFault fault) {
    switch (fault) {
    case FatalFault::DfiProtocol: fatal_event(FX1_DDR3_FATAL_DFI_PROTOCOL); break;
    case FatalFault::WriteDataProtocol: fatal_event(FX1_DDR3_FATAL_WS_PROTOCOL); break;
    case FatalFault::WriteResultUnmatched: fatal_event(FX1_DDR3_FATAL_WR_UNMATCHED); break;
    case FatalFault::StaleLockRelease: fatal_event(FX1_DDR3_FATAL_ECC_HOLD); break;
    case FatalFault::ScrubMapping:
        if (scrub_running_) {
            scrub_sync();
            scrub_abort();
            fatal_event(FX1_DDR3_FATAL_SCRUB);  // BE03: scrub fatal, readiness stays 0
        } else {
            scrub_fault_armed_ = true;
        }
        break;
    }
}

void Ddr3Controller::fault_clear() {
    return_faults_.clear();
    write_faults_ = 0;
    scrub_fault_armed_ = false;
}

} // namespace fx1::ddr3
