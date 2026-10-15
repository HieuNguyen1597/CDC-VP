// SPDX-License-Identifier: Apache-2.0
#pragma once
// Build-time configuration of the DDR3 controller model: product profile, build
// options and derived constants (HAS §5, MAS §5.1-5.3, §5.7.1). Pure C++17, no SystemC.
#include <cstdint>
#include <string>
#include <vector>

namespace fx1::ddr3 {

// MAS Table 1-1 / 5-2. Width, ratio and maximum rank count are fixed per profile.
enum class Profile { NativeR2, NativeR1, Legacy };
enum class SpeedBin { DDR3_800 = 800, DDR3_1066 = 1066, DDR3_1333 = 1333, DDR3_1600 = 1600 };
enum class DeviceWidth { X4 = 4, X8 = 8 };
enum class Density { Gb1 = 1, Gb2 = 2, Gb4 = 4, Gb8 = 8 };
enum class Temperature { Normal, Extended };

const char* to_string(Profile p) noexcept;

// Build options (MAS Tables 5-4, 5-6, 5-7, 5-8). Values outside the profile-fixed
// fields are run-time-independent model parameters; CSR-visible run-time state lives
// elsewhere.
struct BuildConfig {
    Profile profile = Profile::NativeR2;
    unsigned n_ports = 4;              // DDR_SYS_N_PORT, 1..4; bundle count stays 4
    unsigned id_width = 4;             // DDR_SYS_ID_W, 1..8
    bool ecc_enable = true;            // DDR_SYS_ECC_ENABLE
    SpeedBin speed_bin = SpeedBin::DDR3_1600;
    DeviceWidth device_width = DeviceWidth::X8;
    Density density = Density::Gb4;    // DDR_SYS_DENSITY (x8 -> 4Gb, x4 -> 2Gb by default)
    Temperature temperature = Temperature::Normal;
    unsigned map_profile_mask = 3;     // MAP_PROFILE_MASK: bit0 linear, bit1 interleaved
    unsigned row_bits_max = 16;        // ROW_BITS_MAX, 12..16
    bool irq_test_enable = true;       // IRQ_TEST_ENABLE_RAW
    bool exclusive_enable = true;      // EXCL_ENABLE
    unsigned exclusive_port_mask = 1;  // EXCL_PORT_MASK, port 0 by default
    unsigned exclusive_entries = 4;    // EXCL_N_ENTRY (MAS; HAS silent)
    unsigned tags_per_port = 32;       // N_TAG: transaction contexts per port
    unsigned refresh_max_debt = 8;     // REFRESH_MAX_DEBT_RAW, 1..8
    // PHY-dependent build defines with no JEDEC source (MAS §5.6, Table 5-15 note).
    unsigned phy_write_latency = 2;    // FTEL_PHY_PWL
    unsigned phy_read_latency = 6;     // FTEL_PHY_PRL
    unsigned read_gate_offset = 0;     // FTEL_PHY_G
    unsigned t_rtrs = 2;               // FTEL_DEF_T_RTRS
    unsigned dfi_wrdata_delay = 1;     // FTEL_DEF_TPHY_WRDATA (tphy_wrdata, DEC-08)

    // Profile-fixed values (MAS Table 5-2).
    unsigned dfi_ratio() const noexcept { return profile == Profile::Legacy ? 1u : 2u; }
    unsigned axi_data_bits() const noexcept { return profile == Profile::Legacy ? 64u : 128u; }
    unsigned axi_beat_bytes() const noexcept { return axi_data_bits() / 8u; }
    unsigned max_ranks() const noexcept { return profile == Profile::NativeR2 ? 2u : 1u; }

    static BuildConfig native_r2() { return BuildConfig{}; }
    static BuildConfig native_r1() {
        BuildConfig c;
        c.profile = Profile::NativeR1;
        return c;
    }
    static BuildConfig legacy() {
        BuildConfig c;
        c.profile = Profile::Legacy;
        return c;
    }
};

struct ConfigError {
    std::string field;
    std::string message;
};

// Build-time legality (MAS §5.7.1 Table 5-19 and option ranges). Empty when legal.
std::vector<ConfigError> validate(const BuildConfig& cfg);
// Throws std::invalid_argument listing every error when the configuration is illegal.
void require_valid(const BuildConfig& cfg);

// Device organisation (MAS Table 5-10).
struct DeviceGeometry {
    unsigned row_bits;
    unsigned column_bits;
    unsigned page_words;          // DDR words (32-bit data bus) per rank row
    unsigned device_page_bytes;   // per-device page, selects the tRRD/tFAW column
    unsigned max_ranks_within_4g; // 0 when the organisation is unsupported
};
// Returns false for an organisation outside Table 5-10 limits (8Gb x4).
bool device_geometry(DeviceWidth w, Density d, DeviceGeometry& out) noexcept;
// Bytes per rank: 2^row_bits * page_words * 4 bytes * 8 banks.
std::uint64_t rank_bytes(unsigned row_bits, unsigned page_words) noexcept;

// CSR capability constants (HAS Table 8-6).
namespace caps {
constexpr std::uint32_t kIpId = 0x44445233u;
constexpr std::uint32_t kAbiVersion = 0x00010000u;
constexpr std::uint32_t kCapDepth0 = 0x04101008u;
constexpr std::uint32_t kCapWidth0Ratio2 = 0x00208410u;
constexpr std::uint32_t kCapWidth0Ratio1 = 0x00204410u;
} // namespace caps
std::uint32_t cap_feature(const BuildConfig& cfg) noexcept;
std::uint32_t cap_width0(const BuildConfig& cfg) noexcept;

} // namespace fx1::ddr3
