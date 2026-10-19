#pragma once
#include <cstdint>

namespace fu3::aes::reg {
// Byte offsets for the VP. PDF local CSR indices are multiplied by four.
constexpr std::uint32_t NAME0 = 0x00, NAME1 = 0x04, VERSION = 0x08;
constexpr std::uint32_t CTRL = 0x20, STATUS = 0x24, CONFIG = 0x28;
constexpr std::uint32_t KEY0 = 0x40, BLOCK0 = 0x80, RESULT0 = 0xc0;
constexpr std::uint32_t APERTURE = 0x400;
constexpr std::uint32_t INIT = 1, NEXT = 2, READY = 1, VALID = 2;
constexpr std::uint32_t ENCRYPT = 1, KEY256 = 2;
constexpr std::uint32_t key(unsigned i) { return KEY0 + 4 * i; }
constexpr std::uint32_t block(unsigned i) { return BLOCK0 + 4 * i; }
constexpr std::uint32_t result(unsigned i) { return RESULT0 + 4 * i; }
}
