// SPDX-License-Identifier: Apache-2.0
// Memory data-path regressions (subtask 3): data placement against an independent byte
// scoreboard (AXI4 byte walk written here, not taken from the model), decode verdicts
// (MAS Table 6-23), ordering, exclusive access, admission/quiesce with live traffic,
// multi-port traffic, capacity edges, the platform bridge and the debug transport.
#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "ddr3_bench.h"

#include "fx1_ddr3/generic_axi_bridge.h"

#include <map>
#include <random>

using namespace fx1_ddr3_test;
using namespace fx1::ddr3;

namespace {

constexpr std::uint32_t kBase = 0x80000000u;

// ------------------------------------------------------------ independent reference model
// AXI4 (IHI0022 A3.4.1) byte walk: payload index and address of every byte of every beat.
std::vector<std::pair<std::size_t, std::uint64_t>> axi_walk(const AxiRequest& r, unsigned bus) {
    std::vector<std::pair<std::size_t, std::uint64_t>> out;
    const std::uint64_t size = 1u << r.size, n = r.len + 1u;
    const std::uint64_t aligned = r.addr / size * size;
    const std::uint64_t lower_wrap = r.addr / (size * n) * (size * n);
    for (std::uint64_t i = 0; i < n; ++i) {
        std::uint64_t a = r.addr;
        if (r.burst == AxiBurst::Incr) a = i == 0 ? r.addr : aligned + i * size;
        if (r.burst == AxiBurst::Wrap) {
            a = r.addr + i * size;
            if (a >= lower_wrap + size * n) a -= size * n;
        }
        const std::uint64_t end = a / size * size + size;
        for (std::uint64_t b = a; b < end; ++b) out.emplace_back(static_cast<std::size_t>(i * bus + b % bus), b);
    }
    return out;
}

struct Scoreboard {
    std::map<std::uint64_t, std::uint8_t> mem;  // global address -> byte (absent = 0)
    std::uint8_t at(std::uint64_t a) const {
        auto it = mem.find(a);
        return it == mem.end() ? 0 : it->second;
    }
};

AxiRequest make(std::uint32_t addr, AxiBurst burst, unsigned len, unsigned size, bool write, unsigned id = 0) {
    AxiRequest r;
    r.addr = addr;
    r.burst = burst;
    r.len = static_cast<std::uint8_t>(len);
    r.size = static_cast<std::uint8_t>(size);
    r.write = write;
    r.id = static_cast<std::uint8_t>(id);
    return r;
}

unsigned log2u(unsigned v) {
    unsigned r = 0;
    while ((1u << r) < v) ++r;
    return r;
}

// Random legal request inside [lo, lo + window).
AxiRequest random_request(std::mt19937& rng, unsigned bus, std::uint32_t lo, std::uint32_t window, bool write, unsigned id) {
    const unsigned size = rng() % (log2u(bus) + 1u);
    const unsigned kind = rng() % 10;
    AxiRequest r;
    r.write = write;
    r.id = static_cast<std::uint8_t>(id);
    r.size = static_cast<std::uint8_t>(size);
    if (kind < 7) {
        r.burst = AxiBurst::Incr;
        r.len = static_cast<std::uint8_t>(rng() % 8 == 0 ? rng() % 64 : rng() % 16);
    } else if (kind < 9) {
        r.burst = AxiBurst::Wrap;
        static const unsigned lens[] = {1, 3, 7, 15};
        r.len = static_cast<std::uint8_t>(lens[rng() % 4]);
    } else {
        r.burst = AxiBurst::Fixed;
        r.len = 0;
    }
    const std::uint32_t span = r.span_bytes();
    std::uint32_t a = lo + static_cast<std::uint32_t>(rng() % (window - 2 * span - 64));
    if (r.burst == AxiBurst::Wrap) a &= ~((1u << size) - 1u);
    if (r.burst == AxiBurst::Incr && ((a & ~((1u << size) - 1u)) & 0xFFFu) + span > 4096u) a &= ~0xFFFu;
    r.addr = a;
    return r;
}

// Issues one random transaction and checks it against (and updates) the scoreboard.
bool scoreboard_step(MemPort& m, Scoreboard& sb, std::mt19937& rng, std::uint32_t lo, std::uint32_t window, unsigned id,
                     std::string* why) {
    const bool write = rng() % 2 == 0;
    const AxiRequest r = random_request(rng, m.bus, lo, window, write, id);
    const auto walk = axi_walk(r, m.bus);
    if (write) {
        std::vector<std::uint8_t> img(std::size_t{r.beats()} * m.bus), be(img.size());
        for (auto& b : img) b = static_cast<std::uint8_t>(rng());
        for (auto& e : be) e = rng() % 5 ? TLM_BYTE_ENABLED : TLM_BYTE_DISABLED;
        const AxiOut o = m.axi(r, img, be);
        if (o.resp != AxiResp::Okay || o.status != tlm::TLM_OK_RESPONSE) {
            if (why) *why = "write response";
            return false;
        }
        for (const auto& [idx, a] : walk)
            if (be[idx] == TLM_BYTE_ENABLED) sb.mem[a] = img[idx];
        return true;
    }
    const AxiOut o = m.axi(r);
    if (o.resp != AxiResp::Okay || o.beats.size() != r.beats()) {
        if (why) *why = "read response";
        return false;
    }
    std::vector<bool> active(o.data.size(), false);
    for (const auto& [idx, a] : walk) {
        active[idx] = true;
        if (o.data[idx] != sb.at(a)) {
            if (why) *why = "read data";
            return false;
        }
    }
    for (std::size_t i = 0; i < o.data.size(); ++i)
        if (!active[i] && o.data[i] != 0) {
            if (why) *why = "inactive lane not zero";
            return false;
        }
    return true;
}

bool verify_window(MemPort& m, const Scoreboard& sb, std::uint32_t lo, std::uint32_t window) {
    std::vector<std::uint8_t> got;
    if (m.read(lo, got, window) != AxiResp::Okay) return false;
    for (std::uint32_t i = 0; i < window; ++i)
        if (got[i] != sb.at(lo + i)) return false;
    return true;
}

struct Join {
    int pending = 0;
    sc_core::sc_event ev;
    void spawn(std::function<void()> fn) {
        ++pending;
        sc_core::sc_spawn([this, fn] {
            fn();
            --pending;
            ev.notify(sc_core::SC_ZERO_TIME);
        });
    }
    void wait() {
        while (pending) sc_core::wait(ev);
    }
};

std::vector<std::uint8_t> pattern(std::size_t n, unsigned seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i * 31u + seed * 17u + 1u);
    return v;
}

// ------------------------------------------------------------ data placement
void scenario_data(Bench& b) {
    expect(boot_memory(b), "boot to mission readiness");
    MemPort& m = *b.mem[0];
    const unsigned bus = m.bus;
    // E4: WRAP 0x130, 8-byte beats, 8 beats with beat n = n; read back with INCR at 0x100.
    {
        const AxiRequest w = make(kBase + 0x130, AxiBurst::Wrap, 7, 3, true);
        std::vector<std::uint8_t> img(8 * bus, 0);
        for (const auto& [idx, a] : axi_walk(w, bus)) img[idx] = static_cast<std::uint8_t>(idx / bus);
        expect(m.axi(w, img).resp == AxiResp::Okay, "E4 WRAP write OKAY");
        std::vector<std::uint8_t> got;
        m.read(kBase + 0x100, got, 64);
        const unsigned want[] = {2, 3, 4, 5, 6, 7, 0, 1};
        bool ok = true;
        for (unsigned word = 0; word < 8; ++word)
            for (unsigned k = 0; k < 8; ++k) ok &= got[word * 8 + k] == want[word];
        expect(ok, "E4 WRAP beats land at 0x130, 0x138, 0x100 ... 0x128");
    }
    // E5 (AXI128): INCR 0x10, 16-byte beats, strobes FFFF/FFFF/00FF/0000.
    if (bus == 16) {
        m.write(kBase, std::vector<std::uint8_t>(0x60, 0xEE));
        const AxiRequest w = make(kBase + 0x10, AxiBurst::Incr, 3, 4, true);
        std::vector<std::uint8_t> img(64, 0x5A), be(64, TLM_BYTE_ENABLED);
        for (unsigned i = 40; i < 64; ++i) be[i] = TLM_BYTE_DISABLED;
        expect(m.axi(w, img, be).resp == AxiResp::Okay, "E5 write B = OKAY");
        std::vector<std::uint8_t> got;
        m.read(kBase, got, 0x60);
        bool ok = true;
        for (unsigned i = 0; i < 0x60; ++i) ok &= got[i] == ((i >= 0x10 && i < 0x38) ? 0x5A : 0xEE);
        expect(ok, "E5 bytes 0x10-0x37 updated, 0x38-0x4F unchanged (masked ordinal)");
    }
    // FE-2.2-13: CACHE / PROT are metadata only.
    {
        AxiRequest w = make(kBase + 0x200, AxiBurst::Incr, 0, log2u(bus), true);
        w.cache = 0xF;
        w.prot = 7;
        std::vector<std::uint8_t> img = pattern(bus, 3);
        const AxiOut wo = m.axi(w, img);
        AxiRequest r = make(kBase + 0x200, AxiBurst::Incr, 0, log2u(bus), false);
        r.cache = 0x3;
        r.prot = 2;
        const AxiOut ro = m.axi(r);
        expect(wo.resp == AxiResp::Okay && ro.resp == AxiResp::Okay && ro.data == img, "CACHE/PROT do not change data or response");
    }
    // Random traffic against the scoreboard, low window and a window near 16 MiB (rank 1
    // when two ranks are active: ranks alternate every 8 KiB in this geometry).
    Scoreboard sb;
    // Seed the scoreboard with the bytes written above.
    std::vector<std::uint8_t> seedbytes;
    m.read(kBase, seedbytes, 0x400);
    for (unsigned i = 0; i < 0x400; ++i)
        if (seedbytes[i]) sb.mem[kBase + i] = seedbytes[i];
    std::mt19937 rng(1234);
    unsigned ok = 0;
    std::string why;
    const unsigned n = 600;
    for (unsigned i = 0; i < n; ++i) {
        const std::uint32_t lo = i % 2 ? kBase + 0x01000000u : kBase;
        if (scoreboard_step(m, sb, rng, lo, 0x10000, i % 4, &why)) ++ok;
        else if (ok + 10 > i) expect(false, "random step " + std::to_string(i) + ": " + why);
    }
    expect_eq(ok, n, "random INCR/WRAP/FIXED, narrow, unaligned, strobed traffic matches the scoreboard");
    expect(verify_window(m, sb, kBase, 0x10000) && verify_window(m, sb, kBase + 0x01000000u, 0x10000),
           "final read-back of both windows");
    expect(b.ctrl->ce_count() == 0 && b.ctrl->ue_count() == 0, "no ECC event on clean traffic");
    expect(b.ctrl->resident_pages() < 64, "sparse store: only touched pages are resident");
}

// ------------------------------------------------------------ decode verdicts
void scenario_decode(Bench& b) {
    b.ctrl->set_bootstrap_region(0, 1, {0xC0000000u, 256u});
    expect(boot_memory(b, 12, 0, FX1_DDR3_TOPO_MAP_LINEAR, kBase, 0x40000000u), "boot (region 0 = 1 GiB)");
    MemPort& m = *b.mem[0];
    CsrPort& c = b.csr;
    const std::vector<std::uint8_t> fill = pattern(0x2000, 7);
    m.write(kBase, fill);
    // E1 MAS Table 6-23.
    expect(m.axi(make(kBase + 0x100, AxiBurst::Incr, 15, 4, true), pattern(256, 1)).resp == AxiResp::Okay, "E1 row 1 OKAY");
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
    AxiOut o = m.axi(make(0x70000000u, AxiBurst::Incr, 0, 4, false));
    expect(o.resp == AxiResp::DecErr && o.status == tlm::TLM_ADDRESS_ERROR_RESPONSE && o.beats.size() == 1 &&
               o.beats[0] == AxiResp::DecErr,
           "E1 row 2 unmapped -> DECERR, one beat");
    expect(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_MAPPING, "region decode miss raises IRQ_RAW[4] (SOQ-30)");
    struct Bad {
        AxiRequest r;
        const char* what;
    };
    const Bad bad[] = {{make(kBase + 0xFC0, AxiBurst::Incr, 7, 4, true), "E1 row 3 4 KiB crossing"},
                       {make(kBase + 0x8, AxiBurst::Wrap, 3, 4, true), "E1 row 4 WRAP misaligned"},
                       {make(0xC00000F0u, AxiBurst::Incr, 1, 4, true), "E1 row 5 span past region 1"},
                       {make(kBase, AxiBurst::Incr, 0, 5, true), "E1 row 6 32-byte beat"},
                       {make(kBase + 0x40, AxiBurst::Fixed, 3, 4, true), "E1 row 7 FIXED len 3"}};
    for (const Bad& x : bad) {
        o = m.axi(x.r, std::vector<std::uint8_t>(std::size_t{x.r.beats()} * 16, 0xA5));
        expect(o.resp == AxiResp::SlvErr && o.status == tlm::TLM_GENERIC_ERROR_RESPONSE, std::string(x.what) + ": SLVERR");
    }
    std::vector<std::uint8_t> got;
    m.read(kBase, got, 0x2000);
    bool unchanged = true;
    for (unsigned i = 0x200; i < 0x2000; ++i) unchanged &= got[i] == fill[i];
    expect(unchanged, "errored writes leave memory unchanged (FE-6.1.3-12)");
    AxiRequest rr = make(kBase + 0x40, AxiBurst::Fixed, 3, 4, false);
    o = m.axi(rr);
    bool zero = true;
    for (auto v : o.data) zero &= v == 0;
    expect(o.resp == AxiResp::SlvErr && o.beats.size() == 4 && zero && o.beats[3] == AxiResp::SlvErr,
           "errored read: LEN+1 beats, error on every beat, zero data (FE-6.1.3-13)");
    // Model-usage errors (VPL-34): TLM statuses, no AXI response, no memory change.
    {
        AxiRequest bad_id = make(kBase, AxiBurst::Incr, 0, 4, true, 16);  // ID width 4
        expect(m.axi(bad_id, std::vector<std::uint8_t>(16, 1)).status == tlm::TLM_COMMAND_ERROR_RESPONSE,
               "ID beyond DDR_SYS_ID_W: TLM command error");
        std::vector<std::uint8_t> buf(16), be(8, TLM_BYTE_ENABLED);
        tlm::tlm_generic_payload tx;
        AxiExtension ext;
        ext.req = make(kBase, AxiBurst::Incr, 1, 4, true);  // two beats need 32 bytes
        tx.set_extension(&ext);
        tx.set_command(tlm::TLM_WRITE_COMMAND);
        tx.set_address(kBase);
        tx.set_data_ptr(buf.data());
        tx.set_data_length(16);
        tx.set_streaming_width(16);
        sc_time t = sc_core::SC_ZERO_TIME;
        m.socket->b_transport(tx, t);
        const bool burst = tx.get_response_status() == tlm::TLM_BURST_ERROR_RESPONSE;
        ext.req.len = 0;
        tx.set_byte_enable_ptr(be.data());
        tx.set_byte_enable_length(8);
        m.socket->b_transport(tx, t);
        const bool bel = tx.get_response_status() == tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE;
        tx.set_byte_enable_ptr(nullptr);
        tx.set_byte_enable_length(0);
        tx.set_command(tlm::TLM_READ_COMMAND);  // disagrees with req.write
        m.socket->b_transport(tx, t);
        const bool cmd = tx.get_response_status() == tlm::TLM_COMMAND_ERROR_RESPONSE;
        tx.clear_extension(&ext);
        std::vector<std::uint8_t> g;
        m.read(kBase, g, 16);
        expect(burst && bel && cmd && g == std::vector<std::uint8_t>(fill.begin(), fill.begin() + 16),
               "length, byte-enable length and command mismatches: TLM errors, memory unchanged");
    }
    // Backend poison: user offset at USABLE capacity -> DECERR, no mapping IRQ (HAS T6-33).
    const std::uint64_t usable = b.ctrl->geometry().usable_granules;
    c.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
    const std::uint32_t top = kBase + static_cast<std::uint32_t>(usable * 32u);
    o = m.axi(make(top, AxiBurst::Incr, 0, 4, true), std::vector<std::uint8_t>(16, 1));
    const AxiOut last = m.axi(make(top - 16, AxiBurst::Incr, 0, 4, true), std::vector<std::uint8_t>(16, 2));
    expect(o.resp == AxiResp::DecErr && last.resp == AxiResp::Okay && !(c.rd(FX1_DDR3_IRQ_RAW) & FX1_DDR3_IRQ_MAPPING),
           "d >= USABLE_GRANULES: DECERR poison without a mapping event; the last usable granule is OKAY");
    // A burst whose granules straddle the capacity edge: per-granule poison, worst response.
    o = m.axi(make(top - 16, AxiBurst::Incr, 1, 4, false));
    expect(o.resp == AxiResp::DecErr && o.beats[0] == AxiResp::Okay && o.beats[1] == AxiResp::DecErr && o.data[0] == 2,
           "read straddling capacity: first beat OKAY with data, second DECERR (sticky forward)");
}

// ------------------------------------------------------------ ordering
void scenario_ordering(Bench& b) {
    expect(boot_memory(b), "boot");
    MemPort& m0 = *b.mem[0];
    MemPort& m1 = *b.mem[1];
    const std::vector<std::uint8_t> big = pattern(4096, 9);
    // 1. Same ID: a read issued after a long write waits for it and observes the new data.
    {
        Join j;
        AxiOut w, r;
        j.spawn([&] { w = m0.axi(make(kBase + 0x10000, AxiBurst::Incr, 255, 4, true, 1), big); });
        j.spawn([&] {
            sc_core::wait(1, SC_NS);
            r = m0.axi(make(kBase + 0x10FF0, AxiBurst::Incr, 0, 4, false, 1));
        });
        j.wait();
        bool same = true;
        for (unsigned i = 0; i < 16; ++i) same &= r.data[i] == big[4096 - 16 + i];
        expect(w.resp == AxiResp::Okay && r.resp == AxiResp::Okay && same && r.end >= w.end,
               "same-ID W then R: R waits and returns the new data (FE-6.1.5-01/02)");
    }
    // 2. R data leaves in AR acceptance order across IDs; a write of another ID may finish
    //    first (FE-6.1.8-01, FE-6.1.5-04).
    {
        Join j;
        AxiOut w0, r1, r2, w3;
        j.spawn([&] { w0 = m0.axi(make(kBase + 0x20000, AxiBurst::Incr, 255, 4, true, 1), big); });
        j.spawn([&] {
            sc_core::wait(1, SC_NS);
            r1 = m0.axi(make(kBase + 0x20000, AxiBurst::Incr, 0, 4, false, 1));
        });
        j.spawn([&] {
            sc_core::wait(2, SC_NS);
            r2 = m0.axi(make(kBase + 0x30000, AxiBurst::Incr, 0, 4, false, 2));
        });
        j.spawn([&] {
            sc_core::wait(3, SC_NS);
            w3 = m0.axi(make(kBase + 0x31000, AxiBurst::Incr, 0, 4, true, 3), pattern(16, 2));
        });
        j.wait();
        expect(r1.end >= w0.end && r2.end >= r1.end, "R2 (ID 2) leaves after R1 (ID 1), which waited for W0");
        expect(w3.end < r1.end && w3.resp == AxiResp::Okay, "a write of another ID completes before the older read");
    }
    // 3. An errored write keeps its same-ID position (FE-6.1.5-03).
    {
        Join j;
        AxiOut a, e, c;
        j.spawn([&] { a = m0.axi(make(kBase + 0x40000, AxiBurst::Incr, 255, 4, true, 5), big); });
        j.spawn([&] {
            sc_core::wait(1, SC_NS);
            e = m0.axi(make(0x10000000u, AxiBurst::Incr, 0, 4, true, 5), pattern(16, 1));
        });
        j.spawn([&] {
            sc_core::wait(2, SC_NS);
            c = m0.axi(make(kBase + 0x50000, AxiBurst::Incr, 0, 4, true, 5), pattern(16, 1));
        });
        j.wait();
        expect(a.resp == AxiResp::Okay && e.resp == AxiResp::DecErr && c.resp == AxiResp::Okay && e.end >= a.end &&
                   c.end >= e.end,
               "B order W(OK), W(DECERR), W(OK) with one ID");
    }
    // 4. Same-granule hazard across ports (BE-6.2.5-01): a read installed before a write
    //    returns the old data; one installed after returns the new data.
    {
        const std::vector<std::uint8_t> old_v = pattern(32, 20), new_v = pattern(32, 21);
        m0.write(kBase + 0x70000, old_v);
        Join j;
        AxiOut r_before, w, r_after;
        j.spawn([&] { r_before = m0.axi(make(kBase + 0x70000, AxiBurst::Incr, 1, 4, false, 1)); });
        j.spawn([&] {
            sc_core::wait(1, SC_NS);
            std::vector<std::uint8_t> img = new_v;
            w = m1.axi(make(kBase + 0x70000, AxiBurst::Incr, 1, 4, true, 1), img);
        });
        j.spawn([&] {
            sc_core::wait(2, SC_NS);
            r_after = m0.axi(make(kBase + 0x70000, AxiBurst::Incr, 1, 4, false, 2));
        });
        j.wait();
        expect(r_before.data == old_v && w.resp == AxiResp::Okay && r_after.data == new_v,
               "same-granule requests complete in install order across ports");
    }
    // 5. Per-port context limit (N_TAG = 32): the 33rd concurrent transaction is not
    //    accepted, so its QoS class is captured only when a context frees (FE-6.1.10-01):
    //    a LUT change while it waits applies to it.
    {
        Join j;
        std::vector<AxiOut> outs(33);
        const std::uint32_t hrt0 = b.snap(FX1_DDR3_SERVICE_COUNT_HRT);
        for (unsigned i = 0; i < 32; ++i)
            j.spawn([&, i] { outs[i] = m1.axi(make(kBase + 0x80000 + i * 0x1000, AxiBurst::Incr, 255, 4, false, i % 16)); });
        sc_core::wait(1, SC_NS);
        j.spawn([&] {
            AxiRequest r = make(kBase + 0x7F000, AxiBurst::Incr, 0, 4, false, 15);
            r.qos = 9;
            outs[32] = m1.axi(r);
        });
        sc_core::wait(5, SC_NS);
        expect(outs[32].status == tlm::TLM_INCOMPLETE_RESPONSE, "33rd transaction not accepted while 32 contexts live");
        b.ctrl->qos_write(1, 9, FX1_DDR3_QOS_CLASS_HRT);
        j.wait();
        expect_eq(b.snap(FX1_DDR3_SERVICE_COUNT_HRT), hrt0 + 1, "class captured at acceptance, after the LUT change");
        b.ctrl->qos_write(1, 9, FX1_DDR3_QOS_CLASS_NORMAL);
        bool all = true;
        sc_time first_end = outs[0].end;
        for (unsigned i = 0; i < 32; ++i) {
            all &= outs[i].resp == AxiResp::Okay;
            first_end = std::min(first_end, outs[i].end);
        }
        expect(all && outs[32].resp == AxiResp::Okay && outs[32].end > first_end, "it is accepted once a context retires");
    }
    // 6. No cross-port order, but a B means the data is observable everywhere (FE-6.1.13-02).
    {
        const std::vector<std::uint8_t> v = pattern(64, 4);
        expect(m0.write(kBase + 0x60000, v) == AxiResp::Okay, "port 0 write");
        std::vector<std::uint8_t> got;
        expect(m1.read(kBase + 0x60000, got, 64) == AxiResp::Okay && got == v, "port 1 reads the data right after the B");
    }
}

// ------------------------------------------------------------ exclusive access
void scenario_exclusive(Bench& b) {
    expect(boot_memory(b), "boot");
    MemPort& m0 = *b.mem[0];
    MemPort& m1 = *b.mem[1];
    auto xr = [&](MemPort& m, std::uint32_t off, unsigned size, unsigned id) {
        AxiRequest r = make(kBase + off, AxiBurst::Incr, 0, size, false, id);
        r.exclusive = true;
        return m.axi(r);
    };
    auto xw = [&](MemPort& m, std::uint32_t off, unsigned size, unsigned id, std::uint8_t v) {
        AxiRequest r = make(kBase + off, AxiBurst::Incr, 0, size, true, id);
        r.exclusive = true;
        return m.axi(r, std::vector<std::uint8_t>(m.bus, v));
    };
    auto byte = [&](std::uint32_t off) {
        std::vector<std::uint8_t> g;
        m0.read(kBase + off, g, 1);
        return g[0];
    };
    AxiOut o = xr(m0, 0x2000, 3, 0);
    expect(o.resp == AxiResp::ExOkay && o.status == tlm::TLM_OK_RESPONSE && o.beats[0] == AxiResp::ExOkay,
           "exclusive read creating a reservation: EXOKAY");
    expect(xw(m0, 0x2000, 3, 0, 0x11).resp == AxiResp::ExOkay && byte(0x2000) == 0x11, "matching exclusive write: EXOKAY, updated");
    expect(xw(m0, 0x2000, 3, 0, 0x22).resp == AxiResp::Okay && byte(0x2000) == 0x11,
           "second exclusive write without a new read: OKAY, no update (MAS T6-55)");
    // E9: ID 3 reserves granule 0x40; ID 4 misses.
    xr(m0, 0x800, 2, 3);
    expect(xw(m0, 0x800, 2, 4, 0x44).resp == AxiResp::Okay && byte(0x800) != 0x44, "E9 ID 4 miss: OKAY, no update");
    expect(xw(m0, 0x800, 2, 3, 0x33).resp == AxiResp::ExOkay && byte(0x800) == 0x33, "E9 ID 3: EXOKAY");
    // A normal write from another port clears; a fully masked one does not.
    xr(m0, 0x3000, 3, 1);
    AxiRequest masked = make(kBase + 0x3000, AxiBurst::Incr, 0, 3, true);
    m1.axi(masked, std::vector<std::uint8_t>(16, 0x99), std::vector<std::uint8_t>(16, TLM_BYTE_DISABLED));
    expect(xw(m0, 0x3000, 3, 1, 0x55).resp == AxiResp::ExOkay, "fully masked write issues no atom and keeps the reservation");
    xr(m0, 0x3000, 3, 1);
    m1.write(kBase + 0x3004, {0x77});
    expect(xw(m0, 0x3000, 3, 1, 0x66).resp == AxiResp::Okay && byte(0x3004) == 0x77,
           "normal write from port 1 clears the reservation (FE-6.1.12-05)");
    // Port outside EXCL_PORT_MASK: ordinary access.
    expect(xr(m1, 0x3100, 3, 0).resp == AxiResp::Okay, "port 1 (not in the mask): exclusive read is OKAY");
    expect(xw(m1, 0x3100, 3, 0, 0x5A).resp == AxiResp::Okay && byte(0x3100) == 0x5A,
           "port 1 exclusive write updates memory with OKAY (FE-6.1.12-07)");
    // Coverage mismatch and multi-granule reads.
    xr(m0, 0x3200, 3, 2);
    expect(xw(m0, 0x3200, 2, 2, 0x12).resp == AxiResp::Okay, "different byte coverage: OKAY, no update");
    xr(m0, 0x3300, 3, 2);
    AxiRequest wide = make(kBase + 0x3300, AxiBurst::Incr, 3, 4, false, 2);
    wide.exclusive = true;
    expect(m0.axi(wide).resp == AxiResp::Okay, "64-byte exclusive read: OKAY, no reservation (GAP-FE-03)");
    expect(xw(m0, 0x3300, 3, 2, 0x13).resp == AxiResp::Okay, "... and it cleared the owner's earlier entry");
    // Quiesce invalidates the port's reservations (FE-6.1.12-10).
    xr(m0, 0x3400, 3, 6);
    b.ctrl->port_quiesce_request(0, true);
    sc_core::wait(1, SC_NS);
    b.ctrl->port_quiesce_request(0, false);
    expect(xw(m0, 0x3400, 3, 6, 0x14).resp == AxiResp::Okay, "reservation invalidated by a port quiesce");
    // Decode error: no reservation (FE-6.1.12-11).
    AxiRequest dec = make(0x10000000u, AxiBurst::Incr, 0, 3, false, 7);
    dec.exclusive = true;
    expect(m0.axi(dec).resp == AxiResp::DecErr, "exclusive read with DECERR returns DECERR");
    // Debug writes leave reservations alone (GAP-FE-17).
    xr(m0, 0x3500, 3, 8);
    std::uint8_t v = 0xAB;
    b.ctrl->debug_write(0x3500, &v, 1);
    expect(xw(m0, 0x3500, 3, 8, 0x15).resp == AxiResp::ExOkay, "debug write does not clear a reservation");
    // S3-R2: an exclusive write with every strobe zero emits no write atom, so it takes and
    // clears no reservation (its own, or another owner's on the same granule).
    auto xw_masked = [&](std::uint32_t off, unsigned id) {
        AxiRequest r = make(kBase + off, AxiBurst::Incr, 0, 3, true, id);
        r.exclusive = true;
        return m0.axi(r, std::vector<std::uint8_t>(m0.bus, 0xEE), std::vector<std::uint8_t>(m0.bus, TLM_BYTE_DISABLED));
    };
    xr(m0, 0x3600, 3, 9);
    expect(xw_masked(0x3600, 9).resp == AxiResp::ExOkay && byte(0x3600) != 0xEE, "masked exclusive write: EXOKAY, no data");
    expect(xw(m0, 0x3600, 3, 9, 0x16).resp == AxiResp::ExOkay && byte(0x3600) == 0x16,
           "the reservation survives the masked write; the next exclusive write takes it");
    xr(m0, 0x3700, 3, 9);
    xr(m0, 0x3700, 3, 10);
    expect(xw_masked(0x3700, 9).resp == AxiResp::ExOkay, "masked exclusive write by owner 9");
    expect(xw(m0, 0x3700, 3, 10, 0x17).resp == AxiResp::ExOkay && byte(0x3700) == 0x17,
           "another owner's reservation on the granule is untouched");
    expect(xw_masked(0x3800, 11).resp == AxiResp::Okay, "masked exclusive write without a reservation: OKAY");
}

// S3-R3a: exclusive verdicts, ordinary-write invalidation and reservations are decided at
// the backend take boundary in install order, so the outcome is always one of the two legal
// serializations: ordinary write first -> the exclusive write misses and the ordinary value
// stays; exclusive write first -> EXOKAY and the later ordinary value is final.
void scenario_exclusive_race(Bench& b) {
    expect(boot_memory(b), "boot");
    Ddr3Controller& d = *b.ctrl;
    MemPort& m0 = *b.mem[0];
    MemPort& m1 = *b.mem[1];
    const std::uint32_t off = 0x4000;
    auto raw0 = [&] {
        std::uint8_t v = 0;
        d.debug_read(off, &v, 1);
        return v;
    };
    auto xr = [&](unsigned id) {
        AxiRequest r = make(kBase + off, AxiBurst::Incr, 0, 3, false, id);
        r.exclusive = true;
        return m0.axi(r);
    };
    auto xw = [&](unsigned id, std::uint8_t v) {
        AxiRequest r = make(kBase + off, AxiBurst::Incr, 0, 3, true, id);
        r.exclusive = true;
        return m0.axi(r, std::vector<std::uint8_t>(m0.bus, v));
    };
    auto ow = [&](std::uint8_t v) { return m1.axi(make(kBase + off, AxiBurst::Incr, 0, 3, true, 1), std::vector<std::uint8_t>(m1.bus, v)); };
    // 1. Ordinary write in service (ECC internal reads) when the exclusive write arrives.
    {
        expect(xr(1).resp == AxiResp::ExOkay, "1: reservation");
        Join j;
        AxiOut o, x;
        j.spawn([&] { o = ow(0x22); });
        j.spawn([&] {
            sc_core::wait(1, SC_NS);
            x = xw(1, 0x33);
        });
        j.wait();
        expect(o.resp == AxiResp::Okay && x.resp == AxiResp::Okay && raw0() == 0x22,
               "1: ordinary write first: exclusive write misses, 0x22 kept");
    }
    // 2. Exclusive write first, ordinary write 1 ns later: EXOKAY, the ordinary value is final.
    {
        expect(xr(1).resp == AxiResp::ExOkay, "2: reservation");
        Join j;
        AxiOut o, x;
        j.spawn([&] { x = xw(1, 0x44); });
        j.spawn([&] {
            sc_core::wait(1, SC_NS);
            o = ow(0x55);
        });
        j.wait();
        expect(x.resp == AxiResp::ExOkay && o.resp == AxiResp::Okay && raw0() == 0x55,
               "2: exclusive write first: EXOKAY, then 0x55 is final");
    }
    // 3. Lock contention: a long read owns the granule; the ordinary write queues first.
    {
        expect(xr(1).resp == AxiResp::ExOkay, "3: reservation");
        Join j;
        AxiOut r, o, x;
        j.spawn([&] { r = m1.axi(make(kBase + off, AxiBurst::Incr, 255, 4, false, 7)); });
        j.spawn([&] {
            sc_core::wait(1, SC_NS);
            o = ow(0x66);
        });
        j.spawn([&] {
            sc_core::wait(2, SC_NS);
            x = xw(1, 0x77);
        });
        j.wait();
        expect(o.resp == AxiResp::Okay && x.resp == AxiResp::Okay && raw0() == 0x66, "3: queued behind a lock: miss, 0x66 kept");
    }
    // 4. Slot contention: 16 long reads fill the request table; both writes wait for slots,
    //    which are not granted in arrival order, so either serialization may win. Both leave
    //    0x88 as the final value; EXOKAY with 0x99 final would be the lost update.
    {
        expect(xr(1).resp == AxiResp::ExOkay, "4: reservation");
        Join j;
        std::vector<AxiOut> reads(16);
        AxiOut o, x;
        for (unsigned i = 0; i < 16; ++i)
            j.spawn([&, i] { reads[i] = m1.axi(make(kBase + 0x10000 * (i + 1), AxiBurst::Incr, 255, 4, false, i)); });
        j.spawn([&] {
            sc_core::wait(1, SC_NS);
            o = m0.axi(make(kBase + off, AxiBurst::Incr, 0, 3, true, 3), std::vector<std::uint8_t>(m0.bus, 0x88));
        });
        j.spawn([&] {
            sc_core::wait(2, SC_NS);
            x = xw(1, 0x99);
        });
        j.wait();
        expect(o.resp == AxiResp::Okay && (x.resp == AxiResp::Okay || x.resp == AxiResp::ExOkay) && raw0() == 0x88,
               "4: queued for slots: a legal serialization, 0x88 final");
    }
    // 5. An exclusive read installed after an ordinary write observes its data and keeps a
    //    valid reservation (no spurious clear by the earlier write's data landing).
    {
        Join j;
        AxiOut o, r;
        j.spawn([&] { o = ow(0xAA); });
        j.spawn([&] {
            sc_core::wait(1, SC_NS);
            r = xr(2);
        });
        j.wait();
        expect(r.resp == AxiResp::ExOkay && r.data[off % m0.bus] == 0xAA, "5: exclusive read after the write sees 0xAA");
        expect(xw(2, 0xBB).resp == AxiResp::ExOkay && raw0() == 0xBB, "5: its reservation is valid");
    }
}

// S3-R4: an acknowledged power stop invalidates every port's reservations; a denied entry
// (never stopped) or a same-state request keeps them.
void scenario_exclusive_power(Bench& b) {
    expect(boot_memory(b), "boot");
    Ddr3Controller& d = *b.ctrl;
    MemPort& m0 = *b.mem[0];
    MemPort& m1 = *b.mem[1];
    auto xr = [&](unsigned id) {
        AxiRequest r = make(kBase + 0x100, AxiBurst::Incr, 0, 3, false, id);
        r.exclusive = true;
        return m0.axi(r).resp;
    };
    auto xw = [&](unsigned id) {
        AxiRequest r = make(kBase + 0x100, AxiBurst::Incr, 0, 3, true, id);
        r.exclusive = true;
        return m0.axi(r, std::vector<std::uint8_t>(m0.bus, static_cast<std::uint8_t>(id))).resp;
    };
    auto pch = [&](std::uint8_t state) {
        d.pchannel_request(true, state);
        const bool ok = b.wait_until([&] { return d.pchannel().paccept || d.pchannel().pdeny; }, sc_time(200, SC_US),
                                     sc_time(5, SC_NS)) &&
                        d.pchannel().paccept;
        d.pchannel_request(false, state);
        sc_core::wait(5, SC_NS);
        return ok;
    };
    struct S {
        std::uint8_t state;
        const char* name;
    };
    for (const S& s : {S{FX1_DDR3_PSTATE_ACTIVE_POWER_DOWN, "APD"}, S{FX1_DDR3_PSTATE_PRECHARGE_POWER_DOWN, "PPD"},
                       S{FX1_DDR3_PSTATE_SELF_REFRESH, "SR"}}) {
        expect(xr(1) == AxiResp::ExOkay, std::string(s.name) + ": exclusive read");
        expect(pch(s.state) && pch(FX1_DDR3_PSTATE_ACTIVE), std::string(s.name) + ": entry and wake accepted");
        expect(xw(1) == AxiResp::Okay, std::string(s.name) + ": the power stop invalidated the reservation");
    }
    expect(xr(2) == AxiResp::ExOkay && pch(FX1_DDR3_PSTATE_ACTIVE) && xw(2) == AxiResp::ExOkay,
           "same-state request (no stop): reservation kept");
    // Denied before the stop completes: a drain longer than POWER_TIMEOUT (pre-cutoff).
    expect(xr(3) == AxiResp::ExOkay, "exclusive read before a denied entry");
    Join j;
    AxiOut w;
    j.spawn([&] { w = m1.axi(make(kBase + 0x10000, AxiBurst::Incr, 255, 4, true), pattern(4096, 1)); });
    sc_core::wait(2, SC_NS);
    expect(!pch(FX1_DDR3_PSTATE_SELF_REFRESH) && (b.csr.rd(FX1_DDR3_POWER_ERROR) & FX1_DDR3_POWER_ERR_TIMEOUT_PRE_CUTOFF),
           "SR entry denied before the stop was acknowledged");
    j.wait();
    expect(xw(3) == AxiResp::ExOkay, "denied entry: reservation kept");
}

// ------------------------------------------------------------ admission, quiesce, publication
void scenario_admission(Bench& b) {
    Ddr3Controller& d = *b.ctrl;
    CsrPort& c = b.csr;
    MemPort& m0 = *b.mem[0];
    MemPort& m1 = *b.mem[1];
    const std::vector<std::uint8_t> big = pattern(4096, 5);
    // 1. A legal request before readiness is accepted and waits at the backend gate; a
    //    decode error completes locally.
    for (unsigned p = 0; p < 2; ++p) {
        d.set_bootstrap_region(p, 0, {kBase, 0x02000000u});
        d.qos_set_configured(p, true);
    }
    Join early;
    AxiOut pre;
    early.spawn([&] { pre = m0.axi(make(kBase + 0x100, AxiBurst::Incr, 0, 4, false)); });
    sc_core::wait(5, SC_NS);
    expect(!d.fe_drained(), "request accepted before readiness (frontend owns it)");
    expect(m1.axi(make(0x10000000u, AxiBurst::Incr, 0, 4, false)).resp == AxiResp::DecErr,
           "a decode error completes locally before readiness");
    expect(boot_memory(b), "boot with an accepted request waiting");
    const sc_time ready_at = sc_core::sc_time_stamp();
    early.wait();
    expect(pre.resp == AxiResp::Okay && pre.end >= ready_at - sc_time(1, SC_US), "the early request completes after readiness");

    // 2. GLOBAL_CTRL.enable gates the backend (MAS Table 6-73).
    c.ok(FX1_DDR3_GLOBAL_CTRL, 0);
    b.at(sc_time(200, SC_NS), [&c] { c.ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE); });
    AxiOut o = m0.axi(make(kBase, AxiBurst::Incr, 0, 4, false));
    expect(o.resp == AxiResp::Okay && o.end - o.start >= sc_time(200, SC_NS), "enable = 0 holds the request; enable resumes it");

    // 3. Global quiesce: accepted work completes; new requests are held at the port.
    {
        Join j;
        AxiOut w;
        j.spawn([&] { w = m0.axi(make(kBase + 0x1000, AxiBurst::Incr, 255, 4, true), big); });
        sc_core::wait(2, SC_NS);
        expect(c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST), "QUIESCE_CTRL request");
        j.wait();
        expect(w.resp == AxiResp::Okay, "a transaction accepted before the stop completes (no stranding)");
        expect(c.rd(FX1_DDR3_QUIESCE_STATUS) & FX1_DDR3_QUIESCE_ST_COMPLETE, "QUIESCE_STATUS complete once idle");
        b.at(sc_time(100, SC_NS), [&c] { c.ok(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_RELEASE); });
        o = m1.axi(make(kBase, AxiBurst::Incr, 0, 4, false));
        expect(o.end - o.start >= sc_time(100, SC_NS), "a new request is held at the port until the release");
    }

    // 4. E15: a port quiesce is not acknowledged while a read is in flight.
    {
        Join j;
        AxiOut r;
        bool ack_during = true;
        j.spawn([&] { r = m0.axi(make(kBase + 0x1000, AxiBurst::Incr, 255, 4, false)); });
        sc_core::wait(2, SC_NS);
        d.port_quiesce_request(0, true);
        sc_core::wait(5, SC_NS);
        ack_during = d.port_quiesce_ack(0);
        j.wait();
        sc_core::wait(sc_core::SC_ZERO_TIME);
        expect(!ack_during && d.port_quiesce_ack(0) && r.resp == AxiResp::Okay, "E15 ack only after the live read completed");
        d.port_quiesce_request(0, false);
        // FE_PORT_CMD quiesce waits for the drain too.
        AxiOut w;
        j.spawn([&] { w = m0.axi(make(kBase + 0x1000, AxiBurst::Incr, 255, 4, true), big); });
        sc_core::wait(2, SC_NS);
        expect(c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_QUIESCE, 0)), "FE_PORT_CMD quiesce");
        const sc_time cmd_done = sc_core::sc_time_stamp();
        j.wait();
        expect(cmd_done >= w.end, "FE_PORT_CMD quiesce returns after the in-flight write drained");
        expect(FX1_DDR3_FE_STATE_DRAINED(c.rd(FX1_DDR3_FE_PORT_STATE)) & 1u, "FE_PORT_STATE: port 0 drained");
        c.ok(FX1_DDR3_FE_PORT_CMD, FX1_DDR3_FE_PORT_CMD_VAL(FX1_DDR3_FE_OP_RELEASE, 0));
    }

    // 5. Region publication with traffic: quiesce, drain, atomic install (HAS §6.1.15).
    {
        m0.write(kBase + 0x100, pattern(32, 8));
        for (unsigned i = 0; i < 32; ++i) c.ok(FX1_DDR3_FE_REGION_SHADOW(i), 0);
        c.ok(FX1_DDR3_FE_REGION_SHADOW(0), 0x90000000u);  // port 0 region 0
        c.ok(FX1_DDR3_FE_REGION_SHADOW(1), 0x01000000u);
        c.ok(FX1_DDR3_FE_REGION_SHADOW(8), 0x80000000u);  // port 1 region 0
        c.ok(FX1_DDR3_FE_REGION_SHADOW(9), 0x01000000u);
        Join j;
        AxiOut r;
        j.spawn([&] { r = m0.axi(make(kBase + 0x1000, AxiBurst::Incr, 255, 4, false)); });
        sc_core::wait(2, SC_NS);
        expect(c.ok(FX1_DDR3_FE_REGION_COMMIT, 1), "FE_REGION_COMMIT accepted");
        const sc_time installed = sc_core::sc_time_stamp();
        j.wait();
        expect(installed >= r.end && r.resp == AxiResp::Okay, "install waits for the in-flight read");
        const std::uint32_t st = c.rd(FX1_DDR3_FE_REGION_STATUS);
        expect((st & FX1_DDR3_FE_REGION_ST_DONE) && (st & FX1_DDR3_FE_REGION_ST_VALID), "FE_REGION_STATUS done + valid");
        std::vector<std::uint8_t> got;
        expect(m0.read(0x90000100u, got, 32) == AxiResp::Okay && got == pattern(32, 8),
               "new map: 0x9000_0100 reaches user offset 0x100 (normalized addressing, SOQ-20)");
        expect(m0.read(kBase + 0x100, got, 32) == AxiResp::DecErr, "the old base is unmapped on port 0");
        expect(m1.read(kBase + 0x100, got, 32) == AxiResp::Okay && got == pattern(32, 8),
               "port 1 region aliases the same user offset (GAP-FE-01)");
    }

    // 6. Power: self-refresh entry drains the backend first. A drain longer than
    //    POWER_TIMEOUT is denied before the cutoff; with enough time it is accepted. A
    //    request while resident raises PACTIVE and waits for the wake (RD-PWR-01).
    {
        Join j;
        AxiOut w;
        j.spawn([&] { w = m1.axi(make(kBase + 0x2000, AxiBurst::Incr, 255, 4, true), big); });
        sc_core::wait(2, SC_NS);
        d.pchannel_request(true, FX1_DDR3_PSTATE_SELF_REFRESH);
        expect(b.wait_until([&] { return d.pchannel().pdeny; }, sc_time(100, SC_US), sc_time(5, SC_NS)) &&
                   (c.rd(FX1_DDR3_POWER_ERROR) & FX1_DDR3_POWER_ERR_TIMEOUT_PRE_CUTOFF),
               "a drain longer than POWER_TIMEOUT: denied before the cutoff");
        d.pchannel_request(false, FX1_DDR3_PSTATE_SELF_REFRESH);
        j.wait();
        expect(w.resp == AxiResp::Okay && !d.recovery_required(), "the write still completes; no recovery needed");
        c.ok(FX1_DDR3_POWER_ERROR, 0xFFFFFFu);
        c.ok(FX1_DDR3_POWER_TIMEOUT, 0x100000u);
        sc_core::wait(5, SC_NS);
        j.spawn([&] { w = m1.axi(make(kBase + 0x2000, AxiBurst::Incr, 255, 4, true), big); });
        sc_core::wait(2, SC_NS);
        d.pchannel_request(true, FX1_DDR3_PSTATE_SELF_REFRESH);
        expect(b.wait_until([&] { return d.pchannel().paccept; }, sc_time(200, SC_US), sc_time(5, SC_NS)), "SR accepted");
        const sc_time accepted = sc_core::sc_time_stamp();
        j.wait();
        expect(accepted >= w.end, "SR entry waited for the in-flight write");
        d.pchannel_request(false, FX1_DDR3_PSTATE_SELF_REFRESH);
        sc_core::wait(5, SC_NS);
        AxiOut r;
        j.spawn([&] { r = m1.axi(make(kBase + 0x2000, AxiBurst::Incr, 0, 4, false)); });
        sc_core::wait(20, SC_NS);
        expect(d.pchannel().pactive && r.status == tlm::TLM_INCOMPLETE_RESPONSE, "request waits in SR with PACTIVE high");
        d.pchannel_request(true, FX1_DDR3_PSTATE_ACTIVE);
        expect(b.wait_until([&] { return d.pchannel().paccept; }, sc_time(100, SC_US), sc_time(5, SC_NS)), "wake accepted");
        d.pchannel_request(false, FX1_DDR3_PSTATE_ACTIVE);
        j.wait();
        expect(r.resp == AxiResp::Okay && r.data[0] == big[0], "the request completes after the wake, data retained");
    }

    // 7. Runtime training drains the backend before loading the PHY.
    {
        Join j;
        AxiOut w;
        j.spawn([&] { w = m1.axi(make(kBase + 0x3000, AxiBurst::Incr, 255, 4, true), big); });
        sc_core::wait(2, SC_NS);
        const std::uint64_t loads = d.functional_phy()->loads_received();
        expect(c.ok(FX1_DDR3_TRAIN_CMD, FX1_DDR3_TRAIN_CMD_VAL(FX1_DDR3_TRAIN_PRIM_LOAD_GATE, 0, 15, 0)), "TRAIN_CMD");
        const sc_time trained = sc_core::sc_time_stamp();
        j.wait();
        expect(d.functional_phy()->loads_received() == loads + 1 && w.resp == AxiResp::Okay && trained >= w.end,
               "training loads the PHY only after the backend drained");
    }
}

// ------------------------------------------------------------ multi-port traffic
void scenario_multiport(Bench& b) {
    // Port 1 also maps 0xA000_0000 onto the same user space (bootstrap region 1).
    b.ctrl->set_bootstrap_region(1, 1, {0xA0000000u, 0x01000000u});
    expect(boot_memory(b), "boot");
    const unsigned ports = b.ctrl->config().n_ports;
    std::vector<Scoreboard> sb(ports);
    std::vector<unsigned> ok(ports, 0);
    const unsigned n = 250;
    Join j;
    for (unsigned p = 0; p < ports; ++p) {
        j.spawn([&, p] {
            std::mt19937 rng(77 + p);
            const std::uint32_t lo = kBase + p * 0x00100000u;
            for (unsigned i = 0; i < n; ++i) ok[p] += scoreboard_step(*b.mem[p], sb[p], rng, lo, 0x8000, i % 3, nullptr);
        });
    }
    j.wait();
    bool all = true;
    for (unsigned p = 0; p < ports; ++p) all &= ok[p] == n && verify_window(*b.mem[(p + 1) % ports], sb[p], kBase + p * 0x00100000u, 0x8000);
    expect(all, "four ports of concurrent random traffic match their scoreboards; each window read back by another port");
    std::vector<std::uint8_t> a, alias;
    b.mem[0]->read(kBase + 0x00100000u, a, 256);
    b.mem[1]->read(0xA0100000u, alias, 256);
    expect(a == alias, "region aliasing: port 1 region 1 sees the same user offsets (SOQ-20)");
    std::uint32_t total = 0;
    for (std::uint32_t off : {FX1_DDR3_SERVICE_COUNT_BE, FX1_DDR3_SERVICE_COUNT_SRT, FX1_DDR3_SERVICE_COUNT_HRT})
        total += b.snap(off);
    expect(total > 0 && b.csr.rd(FX1_DDR3_SERVICE_COUNT_SRT) == 0, "SERVICE_COUNT_BE counts NORMAL-class atoms");
}

// ------------------------------------------------------------ capacity: 2 ranks, 4 Gb x8, high addresses
void scenario_capacity(Bench& b) {
    // Reference system (MAS Table 6-79), interleaved map; region 0 covers 0 .. 4 GiB - 32.
    expect(boot_memory(b, 16, 2, FX1_DDR3_TOPO_MAP_INTERLEAVED, 0x0, 0xFFFFFFE0u), "boot the 2-rank reference geometry");
    const Geometry& g = b.ctrl->geometry();
    expect(g.usable_granules == 119304640u && g.ranks == 2, "USABLE = 119 304 640 granules");
    expect_eq(b.snap(FX1_DDR3_USABLE_GRANULES_LO), 119304640u, "USABLE_GRANULES_LO snapshot");
    MemPort& m = *b.mem[0];
    const std::uint32_t top = static_cast<std::uint32_t>(g.usable_granules * 32u);
    const std::vector<std::uint8_t> v = pattern(32, 6);
    expect(m.write(top - 32, v) == AxiResp::Okay, "write the last usable granule (0xE38E_37E0)");
    std::vector<std::uint8_t> got;
    expect(m.read(top - 32, got, 32) == AxiResp::Okay && got == v, "read it back");
    expect(m.write(top, v) == AxiResp::DecErr && m.read(top, got, 32) == AxiResp::DecErr,
           "first hidden granule (ECC companion space) is DECERR (BE-6.2.2-10)");
    expect(m.write(0xFFFFFFC0u, v) == AxiResp::DecErr, "high address above capacity: DECERR");
    // Rank 1 with C = 7: ranks alternate every 1024 granules (32 KiB).
    expect(m.write(0x8000, v) == AxiResp::Okay && m.read(0x8000, got, 32) == AxiResp::Okay && got == v, "rank 1 granule");
    expect(map_granule(g, true, 0x8000 / 32).data.rank == 1, "granule 1024 decodes to rank 1");
    expect(m.write(0xE0000000u, v) == AxiResp::Okay && m.read(0xE0000000u, got, 32) == AxiResp::Okay && got == v,
           "high address 0xE000_0000");
    expect(b.ctrl->resident_pages() < 16, "scrub of 119 M granules allocated no storage");
}

// ------------------------------------------------------------ timing sanity
void scenario_timing(Bench& b) {
    expect(boot_memory(b), "boot");
    MemPort& m = *b.mem[0];
    const BankTimer& t = b.ctrl->bank_timer();
    const std::uint64_t hits0 = t.row_hits();
    const AxiOut first = m.axi(make(kBase + 0x4000, AxiBurst::Incr, 0, 4, false));
    const AxiOut second = m.axi(make(kBase + 0x4010, AxiBurst::Incr, 0, 4, false));
    expect(first.end - first.start > sc_time(10, SC_NS) && first.end - first.start < sc_time(200, SC_NS),
           "single read latency is tens of ns");
    expect(second.end - second.start < first.end - first.start && t.row_hits() > hits0,
           "second read of the open row is faster (row hit)");
    const AxiOut burst = m.axi(make(kBase + 0x8000, AxiBurst::Incr, 255, 4, false));
    expect(burst.end - burst.start > sc_time(128 * 5, SC_NS) - sc_time(1, SC_NS),
           "a 4 KiB burst takes at least 128 x tCCD (BL8 per granule)");
    // Continuous traffic for longer than tREFI (7.8 us) runs into REF windows (DEC-02).
    const std::uint64_t delayed = t.refresh_delays();
    const sc_time t0 = sc_core::sc_time_stamp();
    while (sc_core::sc_time_stamp() - t0 < sc_time(20, SC_US)) m.axi(make(kBase + 0x8000, AxiBurst::Incr, 255, 4, false));
    expect(t.refresh_delays() > delayed, "refresh windows delay traffic (tRFC)");
}

// ------------------------------------------------------------ debug transport and host API
void scenario_debug(Bench& b) {
    MemPort& m = *b.mem[0];
    std::uint8_t buf[64];
    expect(!b.ctrl->debug_write(0, buf, 4), "debug access needs a committed topology");
    expect(boot_memory(b), "boot");
    b.csr.ok(FX1_DDR3_IRQ_CLEAR, FX1_DDR3_IRQ_ALL);
    const std::vector<std::uint8_t> v = pattern(64, 2);
    tlm::tlm_generic_payload tx;
    tx.set_command(tlm::TLM_WRITE_COMMAND);
    tx.set_address(kBase + 0x500);
    std::vector<std::uint8_t> data = v;
    tx.set_data_ptr(data.data());
    tx.set_data_length(64);
    expect(m.socket->transport_dbg(tx) == 64, "transport_dbg write through the region map");
    std::vector<std::uint8_t> got;
    expect(m.read(kBase + 0x500, got, 64) == AxiResp::Okay && got == v, "AXI read sees the debug write");
    expect(b.ctrl->ce_count() == 0, "debug write produced consistent ECC (no CE)");
    tx.set_command(tlm::TLM_READ_COMMAND);
    std::fill(data.begin(), data.end(), 0);
    expect(m.socket->transport_dbg(tx) == 64 && data == v, "transport_dbg read");
    tx.set_address(0x10000000u);
    expect(m.socket->transport_dbg(tx) == 0, "unmapped debug access transfers nothing");
    tx.set_address(kBase + 0x02000000u - 32);
    expect(m.socket->transport_dbg(tx) == 0, "a debug access running past the region transfers nothing");
    const std::uint64_t limit = b.ctrl->geometry().usable_granules * 32u;
    expect(!b.ctrl->debug_write(limit - 2, buf, 4) && b.ctrl->debug_read(limit - 4, buf, 4), "user range is checked");
    expect(b.csr.rd(FX1_DDR3_IRQ_RAW) == 0, "debug traffic raises no interrupt");
}

// ------------------------------------------------------------ platform bridge
struct GenInit : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<GenInit> socket{"socket"};
    explicit GenInit(sc_core::sc_module_name n) : sc_module(n) {}
    tlm::tlm_response_status access(bool write, std::uint64_t addr, std::uint8_t* data, unsigned len,
                                    std::uint8_t* be = nullptr, unsigned be_len = 0, unsigned stream = 0) {
        tlm::tlm_generic_payload tx;
        tx.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        tx.set_address(addr);
        tx.set_data_ptr(data);
        tx.set_data_length(len);
        tx.set_streaming_width(stream ? stream : len);
        tx.set_byte_enable_ptr(be);
        tx.set_byte_enable_length(be_len);
        tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sc_time d = sc_core::SC_ZERO_TIME;
        socket->b_transport(tx, d);
        return tx.get_response_status();
    }
};

GenInit* g_gen = nullptr;
GenericAxiBridge* g_bridge = nullptr;

void scenario_bridge(Bench& b) {
    expect(boot_memory(b), "boot");
    GenInit& cpu = *g_gen;
    Ddr3Controller& d = *b.ctrl;
    auto dbg = [&](std::uint64_t off, unsigned n) {
        std::vector<std::uint8_t> v(n);
        d.debug_read(off, v.data(), n);
        return v;
    };
    // CPU accesses of 1, 2, 4, 8 bytes, aligned: one narrow single-beat transaction each.
    bool ok = true;
    unsigned off = 0x100;
    for (unsigned n : {1u, 2u, 4u, 8u}) {
        std::vector<std::uint8_t> v = pattern(n, n), r(n, 0);
        const std::uint64_t before = g_bridge->transactions();
        ok &= cpu.access(true, kBase + off, v.data(), n) == tlm::TLM_OK_RESPONSE;
        ok &= cpu.access(false, kBase + off, r.data(), n) == tlm::TLM_OK_RESPONSE && r == v && dbg(off, n) == v;
        ok &= g_bridge->transactions() == before + 2;
        off += 16;
    }
    expect(ok, "CPU 1/2/4/8-byte accesses: one AXI transaction each, data placed");
    std::vector<std::uint8_t> v = pattern(8, 1), r(8, 0);
    expect(cpu.access(true, kBase + 0x20C, v.data(), 8) == tlm::TLM_OK_RESPONSE &&
               cpu.access(false, kBase + 0x20C, r.data(), 8) == tlm::TLM_OK_RESPONSE && r == v && dbg(0x20C, 8) == v,
           "unaligned 8-byte access across an AXI beat");
    // DMA: 10 KiB across 4 KiB boundaries.
    std::vector<std::uint8_t> dma = pattern(10240, 9), back(10240, 0);
    const std::uint64_t before = g_bridge->transactions();
    // AXI128: 16 B + 3 pages; AXI64 also splits each full page at 256 beats.
    const std::uint64_t bursts = b.ctrl->config().axi_beat_bytes() == 16 ? 4 : 6;
    expect(cpu.access(true, kBase + 0xFF0, dma.data(), 10240) == tlm::TLM_OK_RESPONSE &&
               g_bridge->transactions() - before == bursts,
           "10 KiB DMA write split at 4 KiB boundaries (and 256 beats)");
    expect(cpu.access(false, kBase + 0xFF0, back.data(), 10240) == tlm::TLM_OK_RESPONSE && back == dma, "DMA read back");
    // Generic byte enables (repeat rule): only even bytes written.
    std::vector<std::uint8_t> ones(16, 0xFF);
    std::uint8_t be[2] = {TLM_BYTE_ENABLED, TLM_BYTE_DISABLED};
    cpu.access(true, kBase + 0x5000, ones.data(), 16, be, 2);
    const auto m = dbg(0x5000, 16);
    bool even = true;
    for (unsigned i = 0; i < 16; ++i) even &= m[i] == (i % 2 ? 0 : 0xFF);
    expect(even, "generic byte enables become WSTRB");
    std::uint8_t x[4] = {};
    expect(cpu.access(false, 0x10000000u, x, 4) == tlm::TLM_ADDRESS_ERROR_RESPONSE, "DECERR -> TLM_ADDRESS_ERROR_RESPONSE");
    expect(cpu.access(false, kBase, x, 4, nullptr, 0, 2) == tlm::TLM_BURST_ERROR_RESPONSE, "streaming width < length is rejected");
    tlm::tlm_generic_payload tx;
    tx.set_command(tlm::TLM_READ_COMMAND);
    tx.set_address(kBase + 0x100);
    tx.set_data_ptr(x);
    tx.set_data_length(1);
    expect(cpu.socket->transport_dbg(tx) == 1 && x[0] == pattern(1, 1)[0], "transport_dbg forwarded through the bridge");
    tlm::tlm_dmi dmi;
    expect(!cpu.socket->get_direct_mem_ptr(tx, dmi), "no DMI");
}

int run_bridge(BuildConfig cfg, const std::string& name) {
    sc_core::sc_report_handler::set_actions(sc_core::SC_WARNING, sc_core::SC_DO_NOTHING);
    cfg.n_ports = 1;
    Bench b("b", cfg, 0);  // port 0 left for the bridge
    GenInit gen("cpu");
    GenericAxiBridge bridge("bridge", cfg.axi_beat_bytes());
    gen.socket.bind(bridge.target);
    bridge.initiator.bind(b.ctrl->mem_socket[0]);
    g_gen = &gen;
    g_bridge = &bridge;
    b.scenario = scenario_bridge;
    sc_core::sc_start();
    expect(completed(), "scenario ran to completion (no hung thread)");
    return summary(name);
}

} // namespace

int sc_main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    BuildConfig cfg = BuildConfig::native_r2();
    cfg.n_ports = 2;
    if (which == "data_r2") return run_bench(cfg, scenario_data, "memory data_r2");
    if (which == "data_r1") {
        BuildConfig c = BuildConfig::native_r1();
        c.n_ports = 1;
        return run_bench(c, scenario_data, "memory data_r1");
    }
    if (which == "data_legacy") {
        BuildConfig c = BuildConfig::legacy();
        c.n_ports = 1;
        return run_bench(c, scenario_data, "memory data_legacy");
    }
    if (which == "data_noecc") {
        cfg.ecc_enable = false;
        return run_bench(cfg, scenario_data, "memory data_noecc");
    }
    if (which == "decode") return run_bench(cfg, scenario_decode, "memory decode");
    if (which == "ordering") return run_bench(cfg, scenario_ordering, "memory ordering");
    if (which == "ordering_noecc") {
        cfg.ecc_enable = false;  // writes apply without an internal read: hazards rely on the locks
        return run_bench(cfg, scenario_ordering, "memory ordering_noecc");
    }
    if (which == "exclusive") return run_bench(cfg, scenario_exclusive, "memory exclusive");
    if (which == "exclusive_power") return run_bench(cfg, scenario_exclusive_power, "memory exclusive_power");
    if (which == "exclusive_race") return run_bench(cfg, scenario_exclusive_race, "memory exclusive_race");
    if (which == "exclusive_race_noecc") {
        cfg.ecc_enable = false;
        return run_bench(cfg, scenario_exclusive_race, "memory exclusive_race_noecc");
    }
    if (which == "admission") return run_bench(cfg, scenario_admission, "memory admission");
    if (which == "multiport") {
        cfg.n_ports = 4;
        return run_bench(cfg, scenario_multiport, "memory multiport");
    }
    if (which == "capacity") {
        cfg.n_ports = 1;
        return run_bench(cfg, scenario_capacity, "memory capacity");
    }
    if (which == "timing") {
        cfg.ecc_enable = false;  // the companion of these granules shares their bank
        return run_bench(cfg, scenario_timing, "memory timing");
    }
    if (which == "debug") return run_bench(cfg, scenario_debug, "memory debug");
    if (which == "bridge") return run_bridge(BuildConfig::native_r2(), "memory bridge");
    if (which == "bridge_legacy") return run_bridge(BuildConfig::legacy(), "memory bridge_legacy");
    std::cerr << "unknown case '" << which << "'\n";
    return 2;
}
