// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/ecc_codec.h"

namespace fx1::ddr3 {

namespace {
struct PositionTable {
    std::array<std::uint8_t, 64> data_pos{};  // codeword position of data bit j
    std::array<std::int8_t, 72> data_bit{};   // data bit at a position, -1 for a check position
    // Syndrome bit i = parity of the data bits whose position has bit i set.
    std::array<std::uint64_t, 7> syndrome_mask{};
    constexpr PositionTable() {
        for (auto& b : data_bit) b = -1;
        unsigned j = 0;
        for (unsigned p = 1; p <= 71 && j < 64; ++p) {
            if ((p & (p - 1)) == 0) continue;  // powers of two hold check bits 0..6
            data_pos[j] = static_cast<std::uint8_t>(p);
            data_bit[p] = static_cast<std::int8_t>(j);
            for (unsigned i = 0; i < 7; ++i)
                if ((p >> i) & 1u) syndrome_mask[i] |= std::uint64_t{1} << j;
            ++j;
        }
    }
};
// Built at compile time: no initialization order or guard on the decode path.
constexpr PositionTable kTable{};
unsigned parity64(std::uint64_t v) noexcept { return static_cast<unsigned>(__builtin_parityll(v)); }
// XOR of the positions of the set data bits.
unsigned data_syndrome(std::uint64_t data) noexcept {
    unsigned s = 0;
    for (unsigned i = 0; i < 7; ++i) s |= parity64(data & kTable.syndrome_mask[i]) << i;
    return s;
}
} // namespace

std::uint8_t ecc_encode(std::uint64_t data) noexcept {
    // Check bits 0..6 make the syndrome of the data positions zero.
    const unsigned c = data_syndrome(data) & 0x7Fu;
    const unsigned overall = parity64(data) ^ parity64(c);
    return static_cast<std::uint8_t>(c | (overall << 7));
}

LaneDecode ecc_decode(std::uint64_t data, std::uint8_t check) noexcept {
    LaneDecode r;
    r.data = data;
    // Syndrome = XOR of the indices of all positions holding a one.
    const unsigned s = data_syndrome(data) ^ (check & 0x7Fu);
    r.syndrome = static_cast<std::uint8_t>(s);
    r.mismatch = (parity64(data) ^ parity64(check)) != 0;
    if (s == 0 && !r.mismatch) {
        r.cls = EccClass::Clean;
    } else if (s > 71) {
        r.cls = EccClass::Uncorrectable;
    } else if (s != 0 && r.mismatch) {
        r.cls = EccClass::CorrectedData;
        const int bit = kTable.data_bit[s];
        if (bit >= 0) r.data ^= std::uint64_t{1} << bit;  // a check-bit position flips no data
    } else if (s == 0) {
        r.cls = EccClass::CorrectedParity;
    } else {
        r.cls = EccClass::Uncorrectable;
    }
    return r;
}

std::uint64_t granule_lane(const std::uint8_t* granule, unsigned lane) noexcept {
    std::uint64_t v = 0;
    for (unsigned b = 0; b < 8; ++b) v |= std::uint64_t{granule[8 * lane + b]} << (8 * b);
    return v;
}

void set_granule_lane(std::uint8_t* granule, unsigned lane, std::uint64_t value) noexcept {
    for (unsigned b = 0; b < 8; ++b) granule[8 * lane + b] = static_cast<std::uint8_t>(value >> (8 * b));
}

std::uint32_t ecc_record(const std::uint8_t* granule) noexcept {
    std::uint32_t r = 0;
    for (unsigned l = 0; l < kEccLanes; ++l) r |= std::uint32_t{ecc_encode(granule_lane(granule, l))} << (8 * l);
    return r;
}

GranuleDecode decode_granule(std::uint8_t* data, std::uint32_t record) noexcept {
    GranuleDecode g;
    for (unsigned l = 0; l < kEccLanes; ++l) {
        g.lane[l] = ecc_decode(granule_lane(data, l), record_check(record, l));
        switch (g.lane[l].cls) {
        case EccClass::Clean: break;
        case EccClass::CorrectedData:
        case EccClass::CorrectedParity:
            g.ce_lanes = static_cast<std::uint8_t>(g.ce_lanes | (1u << l));
            set_granule_lane(data, l, g.lane[l].data);
            break;
        case EccClass::Uncorrectable: g.ue_lanes = static_cast<std::uint8_t>(g.ue_lanes | (1u << l)); break;
        }
    }
    return g;
}

} // namespace fx1::ddr3
