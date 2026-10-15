// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/profile.h"

#include "fx1_ddr3/speed_bin.h"

#include <sstream>
#include <stdexcept>

namespace fx1::ddr3 {

const char* to_string(Profile p) noexcept {
    switch (p) {
    case Profile::NativeR2: return "NATIVE_R2";
    case Profile::NativeR1: return "NATIVE_R1";
    case Profile::Legacy: return "LEGACY";
    }
    return "?";
}

namespace {
bool is_pow2(unsigned v) { return v != 0 && (v & (v - 1)) == 0; }
bool known_bin(SpeedBin b) {
    return b == SpeedBin::DDR3_800 || b == SpeedBin::DDR3_1066 || b == SpeedBin::DDR3_1333 ||
           b == SpeedBin::DDR3_1600;
}
} // namespace

bool device_geometry(DeviceWidth w, Density d, DeviceGeometry& out) noexcept {
    // MAS Table 5-10.
    const bool x8 = w == DeviceWidth::X8;
    switch (d) {
    case Density::Gb1: out = {14, x8 ? 10u : 11u, x8 ? 1024u : 2048u, 0, 2}; break;
    case Density::Gb2: out = {15, x8 ? 10u : 11u, x8 ? 1024u : 2048u, 0, 2}; break;
    case Density::Gb4: out = {16, x8 ? 10u : 11u, x8 ? 1024u : 2048u, 0, x8 ? 2u : 1u}; break;
    case Density::Gb8:
        if (!x8) return false;  // page would need 4096 words
        out = {16, 11, 2048, 0, 1};
        break;
    default: return false;
    }
    out.device_page_bytes = out.page_words * static_cast<unsigned>(w) / 8u;
    return true;
}

std::uint64_t rank_bytes(unsigned row_bits, unsigned page_words) noexcept {
    return (std::uint64_t{1} << row_bits) * page_words * 4u * 8u;
}

std::vector<ConfigError> validate(const BuildConfig& c) {
    std::vector<ConfigError> e;
    auto err = [&](const char* f, const std::string& m) { e.push_back({f, m}); };
    if (c.profile != Profile::NativeR2 && c.profile != Profile::NativeR1 &&
        c.profile != Profile::Legacy)
        err("profile", "unknown product profile");
    if (c.n_ports < 1 || c.n_ports > 4) err("n_ports", "DDR_SYS_N_PORT must be 1..4");
    if (c.id_width < 1 || c.id_width > 8) err("id_width", "DDR_SYS_ID_W must be 1..8");
    if (!known_bin(c.speed_bin)) err("speed_bin", "speed bin must be 800, 1066, 1333 or 1600");
    if (c.device_width != DeviceWidth::X4 && c.device_width != DeviceWidth::X8)
        err("device_width", "device width must be x4 or x8");
    DeviceGeometry g{};
    if (!device_geometry(c.device_width, c.density, g)) {
        err("density", "density/width is not a supported organisation (MAS Table 5-10)");
    } else if (g.row_bits > c.row_bits_max) {
        // VP rule: the build could never address the fitted device.
        err("row_bits_max", "device row bits exceed ROW_BITS_MAX");
    }
    if (c.map_profile_mask < 1 || c.map_profile_mask > 3)
        err("map_profile_mask", "MAP_PROFILE_MASK must be 1, 2 or 3");
    if (c.row_bits_max < 12 || c.row_bits_max > 16) err("row_bits_max", "ROW_BITS_MAX must be 12..16");
    if (c.refresh_max_debt < 1 || c.refresh_max_debt > 8)
        err("refresh_max_debt", "REFRESH_MAX_DEBT_RAW must be 1..8");
    if (!is_pow2(c.tags_per_port) || c.tags_per_port < 2 || c.tags_per_port > 32)
        err("tags_per_port", "N_TAG must be a power of two in 2..32");
    if (c.exclusive_enable) {
        const unsigned port_bits = c.n_ports >= 1 && c.n_ports <= 4 ? (1u << c.n_ports) - 1u : 0xFu;
        if (c.exclusive_port_mask == 0)
            err("exclusive_port_mask", "EXCL_PORT_MASK must not be 0 when EXCL_ENABLE = 1");
        else if (c.exclusive_port_mask & ~port_bits)
            err("exclusive_port_mask", "EXCL_PORT_MASK selects a port that is not enabled");
        if (c.exclusive_entries < 1) err("exclusive_entries", "EXCL_N_ENTRY must be at least 1");
    }
    if (c.phy_read_latency == 0) err("phy_read_latency", "PHY read latency must not be 0");
    // Timing words must stay below 2^(BE_TIME_W-1) = 2^31 (MAS Table 5-20).
    if (c.t_rtrs >= 0x80000000u) err("t_rtrs", "T_RTRS must be below 2^31");
    if (c.dfi_wrdata_delay > 255) err("dfi_wrdata_delay", "tphy_wrdata must be below 256");
    // MAS Table 5-19 rule 6: the reference images must pass their commit checks. The PHY
    // latencies, read-gate offset and tphy_wrdata feed the DFI enable delays, which must be
    // representable and legal for DFI_TIMING_COMMIT (no wrap-around, no clamping).
    if (e.empty()) {
        DfiImage dfi{};
        std::string why;
        if (!derive_dfi_image(c, dfi, &why)) err("phy_latency", why);
    }
    return e;
}

void require_valid(const BuildConfig& cfg) {
    const auto errors = validate(cfg);
    if (errors.empty()) return;
    std::ostringstream os;
    os << "illegal DDR3 build configuration (" << to_string(cfg.profile) << "):";
    for (const auto& x : errors) os << "\n  " << x.field << ": " << x.message;
    throw std::invalid_argument(os.str());
}

std::uint32_t cap_feature(const BuildConfig& c) noexcept {
    // HAS Table 8-6.
    std::uint32_t v = c.ecc_enable ? 1u : 0u;
    v |= (c.dfi_ratio() & 3u) << 1;
    v |= (c.max_ranks() & 3u) << 3;
    v |= (c.max_ranks() & 3u) << 5;          // PHY ranks follow the profile PHY
    v |= (c.map_profile_mask & 3u) << 7;
    // [9] live DFI update: always 0 in this profile.
    v |= (c.dfi_ratio() == 2 ? 1u : 0u) << 10;
    v |= (c.irq_test_enable ? 1u : 0u) << 11;
    return v;
}

std::uint32_t cap_width0(const BuildConfig& c) noexcept {
    return c.dfi_ratio() == 2 ? caps::kCapWidth0Ratio2 : caps::kCapWidth0Ratio1;
}

} // namespace fx1::ddr3
