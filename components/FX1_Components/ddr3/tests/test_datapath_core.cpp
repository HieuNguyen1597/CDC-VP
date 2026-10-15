// SPDX-License-Identifier: Apache-2.0
// Data-path core regressions (no SystemC). Golden values are transcribed from the audit
// worked examples (docs/audit/backend_requirements.md §6 G1-G9, frontend_requirements.md
// §5 E1-E10), which copy MAS/HAS tables. The SECDED cross-check uses a bit-array
// reference encoder written independently of the implementation's syndrome arithmetic.
#include "ddr3_check.h"

#include "fx1_ddr3/address_map.h"
#include "fx1_ddr3/bank_timer.h"
#include "fx1_ddr3/ecc_codec.h"
#include "fx1_ddr3/exclusive_monitor.h"
#include "fx1_ddr3/frontend.h"
#include "fx1_ddr3/granule_store.h"

#include <array>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace fx1::ddr3;
using fx1_ddr3_test::expect;
using fx1_ddr3_test::expect_eq;

namespace {

// ------------------------------------------------------------ SECDED
// Reference: an explicit 72-entry codeword, positions 1..71 plus the overall parity.
std::uint8_t ref_encode(std::uint64_t data) {
    std::array<unsigned, 72> cw{};
    unsigned j = 0;
    for (unsigned p = 1; p <= 71; ++p) {
        const bool pow2 = (p & (p - 1)) == 0;
        if (!pow2) cw[p] = static_cast<unsigned>((data >> j++) & 1u);
    }
    std::uint8_t check = 0;
    for (unsigned i = 0; i < 7; ++i) {
        unsigned par = 0;
        for (unsigned p = 1; p <= 71; ++p)
            if (p != (1u << i) && (p >> i) & 1u) par ^= cw[p];
        cw[1u << i] = par;
        check = static_cast<std::uint8_t>(check | (par << i));
    }
    unsigned overall = 0;
    for (unsigned p = 1; p <= 71; ++p) overall ^= cw[p];
    return static_cast<std::uint8_t>(check | (overall << 7));
}

// Position of data bit j (independent of the implementation table).
unsigned data_position(unsigned j) {
    unsigned p = 0, n = 0;
    for (p = 1; p <= 71; ++p) {
        if ((p & (p - 1)) == 0) continue;
        if (n++ == j) break;
    }
    return p;
}

void test_ecc_codec() {
    // MAS Table 6-154 golden vector and derived regressions (audit G5).
    expect_eq(ecc_encode(0x0123456789ABCDEFull), 0x9Cu, "enc(0x0123456789ABCDEF) = 0x9C (MAS T6-154)");
    expect_eq(ecc_encode(0), 0x00u, "enc(0) = 0x00 (scrubbed zero is a valid codeword)");
    expect_eq(ecc_encode(~0ull), 0xFFu, "enc(all ones) = 0xFF");
    expect_eq(ecc_encode(1), 0x83u, "enc(1) = 0x83");
    expect_eq(ecc_encode(0x8000000000000000ull), 0xC7u, "enc(bit 63) = 0xC7");
    const std::uint64_t v = 0x0123456789ABCDEFull;
    const std::uint8_t c = 0x9C;
    LaneDecode d = ecc_decode(v, c);
    expect(d.cls == EccClass::Clean && d.syndrome == 0 && !d.mismatch && d.data == v, "T6-154 no error: CLEAN");
    d = ecc_decode(v ^ (1ull << 10), c);
    expect(d.cls == EccClass::CorrectedData && d.syndrome == 15 && d.mismatch && d.data == v,
           "T6-154 data bit 10 (pos 15): syndrome 15, corrected");
    d = ecc_decode(v, static_cast<std::uint8_t>(c ^ (1u << 3)));
    expect(d.cls == EccClass::CorrectedData && d.syndrome == 8 && d.mismatch && d.data == v,
           "T6-154 check bit 3 (pos 8): CORRECTED_DATA with no data flip");
    d = ecc_decode(v, static_cast<std::uint8_t>(c ^ 0x80u));
    expect(d.cls == EccClass::CorrectedParity && d.syndrome == 0 && d.mismatch, "T6-154 overall parity: CORRECTED_PARITY");
    d = ecc_decode(v ^ 3ull, c);
    expect(d.cls == EccClass::Uncorrectable && d.syndrome == 6 && !d.mismatch, "T6-154 data bits 0,1: syndrome 6, UE");
    d = ecc_decode(v, static_cast<std::uint8_t>(c ^ 0x49u));
    expect(d.cls == EccClass::Uncorrectable && d.syndrome == 73 && d.mismatch, "T6-154 check bits 0,3,6: syndrome 73, UE");

    // Independent reference encoder over random data; every single-bit error corrected,
    // every double-bit data error detected.
    std::mt19937_64 rng(0xDD3ull);
    unsigned enc_ok = 0, single_ok = 0, double_ok = 0;
    const unsigned n = 2000;
    for (unsigned i = 0; i < n; ++i) {
        const std::uint64_t x = rng();
        const std::uint8_t e = ecc_encode(x);
        enc_ok += e == ref_encode(x);
        const unsigned bit = static_cast<unsigned>(rng() % 72);
        LaneDecode r = bit < 64 ? ecc_decode(x ^ (1ull << bit), e) : ecc_decode(x, static_cast<std::uint8_t>(e ^ (1u << (bit - 64))));
        const unsigned want_syn = bit < 64 ? data_position(bit) : (bit == 71 ? 0u : (1u << (bit - 64)));
        single_ok += (r.cls == (bit == 71 ? EccClass::CorrectedParity : EccClass::CorrectedData)) && r.data == x &&
                     r.syndrome == want_syn;
        const unsigned a = static_cast<unsigned>(rng() % 64);
        unsigned b = static_cast<unsigned>(rng() % 64);
        if (b == a) b = (a + 1) % 64;
        r = ecc_decode(x ^ (1ull << a) ^ (1ull << b), e);
        double_ok += r.cls == EccClass::Uncorrectable && !r.mismatch;
    }
    expect_eq(enc_ok, n, "encoder equals the reference encoder on random data");
    expect_eq(single_ok, n, "every single-bit error is corrected with the right syndrome");
    expect_eq(double_ok, n, "every double data-bit error is uncorrectable");

    // Record layout {E3, E2, E1, E0}; lane i = bytes 8i..8i+7 little endian.
    std::array<std::uint8_t, 32> g{};
    for (unsigned b = 0; b < 32; ++b) g[b] = static_cast<std::uint8_t>(b * 7 + 1);
    const std::uint32_t rec = ecc_record(g.data());
    bool lanes_ok = true;
    for (unsigned l = 0; l < 4; ++l) {
        std::uint64_t lane = 0;
        for (unsigned b = 0; b < 8; ++b) lane |= std::uint64_t{g[8 * l + b]} << (8 * b);
        lanes_ok &= record_check(rec, l) == ref_encode(lane);
    }
    expect(lanes_ok, "record byte i is the check field of lane i (bytes 8i..8i+7)");
    std::array<std::uint8_t, 32> bad = g;
    bad[0] ^= 1;          // lane 0: single bit
    bad[17] ^= 0x11;      // lane 2: two bits
    const GranuleDecode gd = decode_granule(bad.data(), rec);
    expect(gd.ce_lanes == 1 && gd.ue_lanes == 4 && bad[0] == g[0] && bad[17] != g[17],
           "granule decode corrects lane 0 in place and flags lane 2 uncorrectable");
}

// ------------------------------------------------------------ address map
Geometry geo(unsigned ranks, unsigned rows, unsigned page_words, unsigned profile, bool ecc) {
    const unsigned page_code = page_words == 256 ? 0 : page_words == 512 ? 1 : page_words == 1024 ? 2 : 3;
    const std::uint32_t topo = (ranks - 1u) | (rows << 4) | (page_code << 9) | (profile << 12);
    return decode_topology(topo, ecc);
}

void test_address_map() {
    // G1: P = 9 -> U = 8 (capacity formula, independent of the geometry decode).
    Geometry tiny{};
    tiny.physical_granules = 9;
    expect_eq(8 * (tiny.physical_granules / 9), 8u, "G1 P=9 gives U=8");

    // G2 reference system: 2 ranks, R_row 16, Wp 1024, ECC on.
    Geometry g = geo(2, 16, 1024, 0, true);
    expect_eq(g.physical_granules, 0x8000000u, "G2 P = 2^27");
    expect_eq(g.usable_granules, 119304640u, "G2 usable = 8 floor(P/9) = 119 304 640");
    expect_eq(g.ecc_base, 0x71C71C0u, "G2 ECC base = usable");
    expect_eq(column_bits(g), 7u, "Wp 1024 -> C = 7");
    MapResult m = map_granule(g, true, 0x1234567);
    expect(m.status == MapStatus::Ok && m.data == DramCoord{1, 2, 0x2468, 103}, "G2 linear data: rank 1 bank 2 row 0x2468 col 103");
    expect(m.ecc_granule == 0x740DA6Cu && m.slot == 7, "G2 ECC granule 0x740DA6C slot 7");
    expect(m.ecc == DramCoord{0, 4, 0xE81B, 108}, "G2 linear ECC coordinates (derived): rank 0 bank 4 row 0xE81B col 108");
    g.map_profile = 1;
    m = map_granule(g, true, 0x1234567);
    expect(m.data == DramCoord{1, 7, 0x2468, 44} && m.ecc == DramCoord{0, 4, 0xE81B, 77},
           "G2 interleaved: data rank 1 bank 7 col 44; ECC rank 0 bank 4 col 77");
    m = map_granule(g, true, 0x71C71C0);
    expect(m.status == MapStatus::OutOfCapacity && m.data == DramCoord{} && m.ecc_granule == 0,
           "G2 d = usable: out of capacity, poisoned to zero");
    Geometry g0 = geo(2, 16, 1024, 0, false);
    expect(g0.usable_granules == 0x8000000u && g0.ecc_base == 0, "ECC off: U0 = P, ECC_BASE 0");
    m = map_granule(g0, false, 0x7FFFFFF);
    expect(m.status == MapStatus::Ok && m.ecc_granule == 0 && m.slot == 0, "G2 d = 2^27-1 without ECC maps (MAS T6-81)");

    // G3 consecutive granules (MAS Table 6-78): linear / interleaved.
    struct Row {
        std::uint64_t d;
        DramCoord lin, il;
    };
    const Row rows[] = {{0, {0, 0, 0, 0}, {0, 0, 0, 0}},     {1, {0, 0, 0, 1}, {0, 1, 0, 0}},
                        {2, {0, 0, 0, 2}, {0, 2, 0, 0}},     {7, {0, 0, 0, 7}, {0, 7, 0, 0}},
                        {8, {0, 0, 0, 8}, {0, 0, 0, 1}},     {9, {0, 0, 0, 9}, {0, 1, 0, 1}},
                        {127, {0, 0, 0, 127}, {0, 7, 0, 15}}, {128, {0, 1, 0, 0}, {0, 0, 0, 16}},
                        {1024, {1, 0, 0, 0}, {1, 0, 0, 0}}};
    Geometry lin = geo(2, 16, 1024, 0, true), il = geo(2, 16, 1024, 1, true);
    bool g3 = true;
    for (const Row& r : rows) g3 &= decode_coord(lin, r.d) == r.lin && decode_coord(il, r.d) == r.il;
    expect(g3, "G3 MAS Table 6-78 consecutive granules, both profiles");

    // G4 capacity per organisation.
    expect(geo(1, 14, 1024, 0, true).physical_granules == 0x1000000u && geo(1, 14, 1024, 0, true).usable_granules == 14913080u,
           "G4 1Gb x8 x1: P 0x1000000, U 14 913 080");
    const Geometry big = geo(2, 16, 2048, 0, true);
    expect(big.physical_granules == 0x10000000u && big.usable_granules == 134217728u,
           "G4 4Gb x4 x2: P 2^28, U clamped to 2^27 (C-05)");
    expect_eq(geo(2, 16, 2048, 0, false).usable_granules, 0x8000000u, "U0 clamped to 2^27");

    // One-to-one property over a whole small geometry: every data and companion granule has
    // its own coordinate, and companions never collide with data (BE-6.2.2-09).
    for (unsigned profile = 0; profile < 2; ++profile) {
        const Geometry s = geo(2, 12, 256, profile, true);  // P = 2^21
        std::vector<bool> seen(s.physical_granules, false);
        bool unique = true, in_range = true;
        auto key = [&](const DramCoord& c) {
            return ((std::uint64_t{c.rank} * 8 + c.bank) << (12 + column_bits(s))) | (std::uint64_t{c.row} << column_bits(s)) |
                   c.column;
        };
        for (std::uint64_t d = 0; d < s.usable_granules; ++d) {
            const MapResult r = map_granule(s, true, d);
            in_range &= r.status == MapStatus::Ok;
            const std::uint64_t k = key(r.data);
            unique &= k < seen.size() && !seen[k];
            if (k < seen.size()) seen[k] = true;
            if ((d & 7u) == 0) {
                const std::uint64_t e = key(r.ecc);
                unique &= e < seen.size() && !seen[e];
                if (e < seen.size()) seen[e] = true;
            }
        }
        expect(in_range && unique, std::string("bijective data + companion decode, ") + (profile ? "interleaved" : "linear"));
    }
    Geometry one = geo(1, 12, 256, 0, false);
    expect(decode_coord(one, 1u << 8).rank == 0 && decode_coord(one, 1u << 8).row == 1,
           "one rank: no rank field, row = g >> (C + 3)");
    // G7 (MAS Table 6-164): P = 18 -> U = 16; data 0..7, companion 16, data 8..15, companion 17.
    Geometry g7{};
    g7.physical_granules = 18;
    const std::uint64_t u7 = 8 * (g7.physical_granules / 9);
    const ScrubFrontier f8 = scrub_frontier(8), f9 = scrub_frontier(9), f17 = scrub_frontier(17),
                        f18 = scrub_frontier(u7 + u7 / 8);
    expect(u7 == 16 && f8.data == 8 && f8.companions == 0 && f9.data == 8 && f9.companions == 1 && f17.data == 16 &&
               f17.companions == 1 && f18.data == 16 && f18.companions == 2,
           "G7 scrub order: 8 data writes, companion, 8 data writes, companion (18 = 16 + 16/8)");
    Geometry bad = one;
    bad.page_words = 300;
    expect(map_granule(bad, false, 0).status == MapStatus::GeomImpossible, "impossible geometry: SLVERR poison status");
}

// ------------------------------------------------------------ frontend decode / atoms
AxiRequest req(std::uint32_t a, AxiBurst b, unsigned len, unsigned size, bool write = false) {
    AxiRequest r;
    r.addr = a;
    r.burst = b;
    r.len = static_cast<std::uint8_t>(len);
    r.size = static_cast<std::uint8_t>(size);
    r.write = write;
    return r;
}

void test_frontend_decode() {
    RegionMap map{};
    map[0] = {0x80000000u, 0x40000000u};
    map[1] = {0xC0000000u, 256u};
    struct Row {
        AxiRequest r;
        AxiResp resp;
        int region;
        const char* check;
        std::uint64_t norm;
    };
    const Row rows[] = {
        {req(0x80000100, AxiBurst::Incr, 15, 4), AxiResp::Okay, 0, nullptr, 0x100},
        {req(0x70000000, AxiBurst::Incr, 0, 4), AxiResp::DecErr, -1, nullptr, 0},
        {req(0x80000FC0, AxiBurst::Incr, 7, 4), AxiResp::SlvErr, 0, "cross_4k_ok", 0xFC0},
        {req(0x80000008, AxiBurst::Wrap, 3, 4), AxiResp::SlvErr, 0, "wrap_algn", 0x8},
        {req(0xC00000F0, AxiBurst::Incr, 1, 4), AxiResp::SlvErr, 1, "span_contained", 0xF0},
        {req(0x80000000, AxiBurst::Incr, 0, 5), AxiResp::SlvErr, 0, "size_ok", 0},
        {req(0x80000040, AxiBurst::Fixed, 3, 4), AxiResp::SlvErr, 0, "fixed_ok", 0x40},
    };
    unsigned i = 0;
    for (const Row& r : rows) {
        const DecodeResult d = decode_burst(r.r, map, 16);
        const bool check_ok = r.check ? (d.failed && std::string(d.failed) == r.check) : d.failed == nullptr;
        expect(d.resp == r.resp && d.region == r.region && check_ok && (r.region < 0 || d.norm_addr == r.norm),
               "E1 MAS Table 6-23 row " + std::to_string(++i));
    }
    // FE-6.1.3-11: an unmapped start with a reserved burst is still DECERR.
    expect(decode_burst(req(0x10, AxiBurst::Reserved, 0, 2), map, 16).resp == AxiResp::DecErr, "DECERR wins over BURST=0b11");
    expect(decode_burst(req(0x80000000, AxiBurst::Reserved, 0, 2), map, 16).failed == std::string("burst_ok"),
           "mapped BURST=0b11 is SLVERR");
    expect(decode_burst(req(0x80000000, AxiBurst::Wrap, 2, 2), map, 16).failed == std::string("wrap_ok"), "WRAP of 3 beats is SLVERR");
    expect(decode_burst(req(0x80000FF0, AxiBurst::Incr, 0, 4), map, 16).resp == AxiResp::Okay,
           "INCR ending on the 4 KiB page's last byte is legal");
    expect(decode_burst(req(0x80000000, AxiBurst::Incr, 0, 4), map, 8).failed == std::string("size_ok"),
           "AXI64: a 16-byte beat is SLVERR");
    // E2: HAS §6.1.3 example.
    RegionMap e2{};
    e2[0] = {0x1000, 0x1000};
    expect(decode_burst(req(0x1FFC, AxiBurst::Incr, 0, 2), e2, 16).resp == AxiResp::Okay &&
               decode_burst(req(0x2000, AxiBurst::Incr, 0, 2), e2, 16).resp == AxiResp::DecErr,
           "E2 base 0x1000 size 0x1000: 0x1FFC hit, 0x2000 miss");
    // Lowest-index hit when bootstrap regions overlap (FE-6.1.3-02).
    RegionMap ov{};
    ov[0] = {0x2000, 0x1000};
    ov[1] = {0x1000, 0x4000};
    const DecodeResult o = decode_burst(req(0x2040, AxiBurst::Incr, 0, 2), ov, 16);
    expect(o.region == 0 && o.norm_addr == 0x40, "overlap: the lowest-index region normalizes");
    // Region end exactly at 2^32 and a burst ending on its last byte.
    RegionMap top{};
    top[0] = {0xFFFFFF00u, 0x100u};
    expect(decode_burst(req(0xFFFFFFF0u, AxiBurst::Incr, 0, 4), top, 16).resp == AxiResp::Okay,
           "burst ending at the last byte below 2^32 is legal");
}

void test_atom_plan() {
    // E3: MAS Table 6-41 (normalized addresses; region base 0).
    struct Row {
        AxiRequest r;
        std::size_t atoms;
        std::uint32_t first, last;
    };
    const Row rows[] = {
        {req(0x104, AxiBurst::Incr, 15, 2), 3, 0xFFFFFFF0u, 0x0000000Fu},
        {req(0x130, AxiBurst::Wrap, 7, 3), 3, 0xFFFF0000u, 0x0000FFFFu},
        {req(0x120, AxiBurst::Wrap, 7, 3), 2, 0xFFFFFFFFu, 0xFFFFFFFFu},
        {req(0x000, AxiBurst::Incr, 255, 4), 128, 0xFFFFFFFFu, 0xFFFFFFFFu},
        {req(0x110, AxiBurst::Wrap, 15, 4), 9, 0xFFFF0000u, 0x0000FFFFu},
        {req(0x108, AxiBurst::Incr, 1, 2), 1, 0x0000FF00u, 0x0000FF00u},
    };
    unsigned i = 0;
    for (const Row& r : rows) {
        const auto a = plan_atoms(r.r, 0, 16);
        expect(a.size() == r.atoms && a.front().coverage == r.first && a.back().coverage == r.last,
               "E3 MAS Table 6-41 row " + std::to_string(++i));
    }
    // E4: WRAP 0x130, 8-byte beats, 8 beats: beat walk 0x130, 0x138, 0x100 ... 0x128.
    const AxiRequest w = req(0x130, AxiBurst::Wrap, 7, 3);
    const std::uint64_t want[] = {0x130, 0x138, 0x100, 0x108, 0x110, 0x118, 0x120, 0x128};
    bool walk = true;
    for (unsigned n = 0; n < 8; ++n) walk &= beat_address(w, n) == want[n];
    expect(walk, "E4 WRAP beat walk");
    // E6: a 4-byte beat 4 bytes into a granule covers bytes 4..7; lanes follow the address.
    const auto e6 = plan_atoms(req(0x80000024, AxiBurst::Incr, 0, 2), 0x80000000, 16);
    expect(e6.size() == 1 && e6[0].granule == 1 && e6[0].coverage == 0xF0u && e6[0].bytes.front().payload == 4,
           "E6 BE bits 4..7, lane 4 of beat 0");
    // E5 packing: INCR 0x10, 16-byte beats, 4 beats -> granules 0, 1, 1, 2 (three atoms).
    const auto e5 = plan_atoms(req(0x10, AxiBurst::Incr, 3, 4), 0, 16);
    expect(e5.size() == 3 && e5[0].coverage == 0xFFFF0000u && e5[1].coverage == 0xFFFFFFFFu && e5[2].granule == 2,
           "E5 MAS Table 6-32 packing");
    // FIXED repeats the same bytes; payload index follows the beat.
    const auto fx = plan_atoms(req(0x8, AxiBurst::Fixed, 0, 3), 0, 8);
    expect(fx.size() == 1 && fx[0].coverage == 0xFF00u && fx[0].bytes.front().payload == 0, "FIXED single beat, AXI64 lane 0");
    // Unaligned INCR first beat: bytes from the address to the end of its size container.
    const auto un = plan_atoms(req(0x3, AxiBurst::Incr, 1, 2), 0, 16);
    expect(un.size() == 1 && un[0].coverage == 0xF8u, "unaligned INCR: beat 0 bytes 3, beat 1 bytes 4..7");
}

// ------------------------------------------------------------ exclusive monitor
void test_exclusive_monitor() {
    // E10: MAS Table 6-54 allocation with four entries.
    ExclusiveMonitor m(4);
    m.exclusive_read(0, 0, 0x100, 0xFF, true);
    m.exclusive_read(0, 1, 0x100, 0xFF, true);
    m.exclusive_read(0, 0, 0x200, 0xFF, true);  // owner's own entry reused
    m.exclusive_read(0, 2, 0x300, 0xFF, true);
    m.exclusive_read(0, 3, 0x400, 0xFF, true);
    expect(m.entries()[0].granule == 0x200 && m.entries()[3].id == 3 && m.victim() == 0, "E10 XR0..XR4 entries 0,1,0,2,3");
    m.exclusive_read(0, 4, 0x500, 0xFF, true);
    expect(m.entries()[0].id == 4 && m.victim() == 1, "E10 XR5 replaces entry 0, victim_q -> 1");
    expect(!m.exclusive_write(0, 0, 0x200, 0xFF, true), "E10 {0,0} evicted: exclusive write fails (OKAY)");
    expect(m.exclusive_write(0, 1, 0x100, 0xFF, true), "E10 {0,1} at 0x100: EXOKAY");
    // E8: a pass clears the reservation; a second exclusive write fails.
    expect(!m.exclusive_write(0, 1, 0x100, 0xFF, true), "E8 XW2 after a passing XW1 fails");
    // E9: ID 3 reservation at 0x40; ID 4 write misses.
    m.clear();
    m.exclusive_read(0, 3, 0x40, 0xF, true);
    expect(!m.exclusive_write(0, 4, 0x40, 0xF, true) && m.exclusive_write(0, 3, 0x40, 0xF, true), "E9 ID 4 misses, ID 3 passes");
    // Coverage must match; normal writes and quiesce clear.
    m.exclusive_read(0, 3, 0x40, 0xF, true);
    expect(!m.exclusive_write(0, 3, 0x40, 0xFF, true), "different byte coverage fails (FE-6.1.12-09)");
    m.exclusive_read(1, 5, 0x80, 0xF, true);
    m.write_taken(0x80);
    expect(!m.exclusive_write(1, 5, 0x80, 0xF, true), "a normal write to the granule clears the reservation");
    m.exclusive_read(1, 5, 0x80, 0xF, true);
    expect(m.port_owns(1), "port 1 owns a reservation");
    m.invalidate_port(1);
    expect(!m.port_owns(1) && !m.exclusive_write(1, 5, 0x80, 0xF, true), "quiesce invalidates the port's reservations");
    m.exclusive_read(0, 6, 0x90, 0xF, true);
    expect(!m.exclusive_read(0, 6, 0, 0, false) && !m.exclusive_write(0, 6, 0x90, 0xF, true),
           "shape-fail read clears the owner's entry (GAP-FE-03)");
}

// ------------------------------------------------------------ sparse store
void test_granule_store() {
    GranuleStore s;
    std::array<std::uint8_t, 32> z{}, d{}, out{};
    s.write(5, z.data());
    expect(s.resident_pages() == 0, "writing zeros to an absent page allocates nothing");
    for (unsigned b = 0; b < 32; ++b) d[b] = static_cast<std::uint8_t>(b + 1);
    s.write(5, d.data(), 0x0000FFFFu);
    s.read(5, out.data());
    expect(out[0] == 1 && out[15] == 16 && out[16] == 0 && s.resident_pages() == 1, "masked write updates enabled bytes only");
    for (std::uint64_t g = 0; g < 1000; ++g) s.write(g, d.data());
    expect_eq(s.resident_pages(), 8u, "1000 granules occupy 8 pages of 128 granules");
    s.zero_range(100, 700);  // pages 1..5 dropped, pages 0 and 6 partially cleared
    s.read(99, out.data());
    const bool kept = out[0] == 1;
    s.read(100, out.data());
    const bool cleared = out[0] == 0;
    s.read(800, out.data());
    expect(kept && cleared && out[0] == 1 && s.resident_pages() == 3, "zero_range drops whole pages and clears partial ones");
    s.zero_range(0, std::uint64_t{1} << 27);
    expect(s.resident_pages() == 0, "a 4 GiB erase drops resident pages only");
}

// ------------------------------------------------------------ bank timer (MAS Table 6-102)
void test_bank_timer() {
    BankTimingParams p;  // DDR3-1600 11-11-11: tRCD 11, CL 11, CWL 8, tRRD 6, tFAW 32, tWTR 6, tRTRS 2
    BankTimer t;
    // ACT at 100 gives rd_dl = 111: the read data ends at 111 + AL + CL + BL/2.
    expect_eq(t.access(100, {0, 0, 5, 0}, false, p), 111u + 11u + 4u, "G9 ACT at 100 -> RD at 111");
    t.reset();
    // tRRD 6 / tFAW 32: ACTs at 100, 106, 112, 118; the fifth waits for 132.
    std::uint64_t end = 0;
    for (unsigned b = 0; b < 5; ++b) end = t.access(100, {0, b, 1, 0}, false, p);
    expect_eq(end - 15u - 11u, 132u, "G9 fifth ACT illegal at 124, legal at 132 (tFAW)");
    expect_eq(t.row_misses(), 5u, "five closed-bank accesses");
    t.reset();
    // WR on rank 0 at 100: a read waits for 118 (CWL + AL + BL/2 + tWTR), a WR on rank 1
    // is legal at 106 (BL/2 + tRTRS).
    (void)t.access(0, {1, 0, 0, 0}, true, p);   // open rank 1 bank 0 (col 11)
    (void)t.access(89, {0, 0, 0, 0}, true, p);  // ACT 89, WR at 100
    BankTimer t2 = t;
    expect_eq(t.access(100, {1, 0, 0, 0}, true, p), 106u + 8u + 4u, "G9 WR on rank 1 legal at 106");
    expect_eq(t2.access(100, {0, 0, 0, 0}, false, p), 118u + 11u + 4u, "G9 RD after WR legal at 118");
    expect(t2.row_hits() == 1, "same row: row hit");
    // Conflict: PRE waits for tRAS/write recovery, then tRP + tRCD.
    BankTimer c;
    (void)c.access(0, {0, 3, 1, 0}, false, p);
    const std::uint64_t hit = c.access(100, {0, 3, 1, 1}, false, p);
    const std::uint64_t conflict = c.access(200, {0, 3, 2, 0}, false, p);
    expect(hit == 100u + 15u && conflict == 200u + 11u + 11u + 15u && c.row_conflicts() == 1, "row hit vs conflict latency");
    c.close_all();
    expect_eq(c.access(300, {0, 3, 2, 0}, false, p), 300u + 11u + 15u, "close_all: the next access activates again");
    // Refresh (DEC-02): tREFI 6240, tRFC 208 (DDR3-1600, 4 Gb). The first window opens at
    // origin + tREFI, closes the rank's rows and delays commands to its end.
    BankTimer r;
    r.set_refresh(0, true, 0, 6240, 208, 0);
    (void)r.access(6000, {0, 1, 7, 0}, false, p);
    const std::uint64_t in_window = r.access(6250, {0, 1, 7, 1}, false, p);
    expect(in_window == 6240u + 208u + 11u + 15u && r.row_misses() == 2 && r.refresh_delays() == 1,
           "access inside a REF window: waits tRFC, row closed by PREA");
    expect_eq(r.access(6500, {0, 1, 7, 2}, false, p), 6500u + 15u, "after the window the reopened row hits");
    BankTimer debt;
    debt.set_refresh(0, true, 0, 6240, 208, 1000);
    expect_eq(debt.access(100, {0, 0, 0, 0}, false, p), 1000u + 11u + 15u, "debt repayment keeps the rank busy");
}

} // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    const bool all = which == "all";
    bool ran = false;
    if (all || which == "ecc_codec") { test_ecc_codec(); ran = true; }
    if (all || which == "address_map") { test_address_map(); ran = true; }
    if (all || which == "frontend_decode") { test_frontend_decode(); ran = true; }
    if (all || which == "atom_plan") { test_atom_plan(); ran = true; }
    if (all || which == "exclusive_monitor") { test_exclusive_monitor(); ran = true; }
    if (all || which == "granule_store") { test_granule_store(); ran = true; }
    if (all || which == "bank_timer") { test_bank_timer(); ran = true; }
    if (!ran) {
        std::cerr << "unknown case " << which << '\n';
        return 2;
    }
    return fx1_ddr3_test::summary("fx1_ddr3 datapath core " + which);
}
