// SPDX-License-Identifier: Apache-2.0
#pragma once
// AXI4 request/response vocabulary of the memory ports (HAS §2.2, Table 1-3).
// Pure C++17 so the transaction contract and burst planning are unit-testable.
#include <cstdint>
#include <vector>

namespace fx1::ddr3 {

enum class AxiBurst : std::uint8_t { Fixed = 0, Incr = 1, Wrap = 2, Reserved = 3 };
enum class AxiResp : std::uint8_t { Okay = 0, ExOkay = 1, SlvErr = 2, DecErr = 3 };

const char* to_string(AxiResp r) noexcept;

// One AXI4 address-channel request as seen at a memory port.
struct AxiRequest {
    bool write = false;
    std::uint32_t addr = 0;       // AxADDR (global AXI address, DEC-07)
    std::uint8_t len = 0;         // AxLEN: beats - 1 (0..255)
    std::uint8_t size = 0;        // AxSIZE: log2(bytes per beat)
    AxiBurst burst = AxiBurst::Incr;
    std::uint8_t id = 0;          // AxID, up to DDR_SYS_ID_W bits
    std::uint8_t qos = 0;         // AxQOS
    bool exclusive = false;       // AxLOCK
    std::uint8_t cache = 0;       // AxCACHE, preserved as metadata (FE04)
    std::uint8_t prot = 0;        // AxPROT, preserved as metadata (FE04)

    unsigned beats() const noexcept { return len + 1u; }
    unsigned beat_bytes() const noexcept { return 1u << size; }
    // Total byte span of the burst (HAS §6.1.3: span_B = (LEN+1) * 2^S).
    std::uint32_t span_bytes() const noexcept { return beats() * beat_bytes(); }
};

// Plan for carrying an arbitrary byte access (as issued by a CPU or DMA over the
// 32-bit FX1 bus) as legal AXI bursts on a memory port of `bus_bytes` width.
struct PlannedBurst {
    AxiRequest req;
    std::uint64_t byte_offset = 0;  // offset of this burst's first wanted byte in the access
    std::uint32_t byte_count = 0;   // wanted bytes carried by this burst
};

// Splits [addr, addr + length) into INCR bursts that never cross a 4 KiB boundary and
// never exceed 256 beats. A naturally aligned power-of-two access no wider than the bus
// becomes one narrow single-beat burst (AxSIZE = log2(length)), like a CPU master;
// anything else uses full-width beats with strobes limited to the wanted bytes.
// Returns an empty vector when length is 0 or the range overflows 32 bits.
std::vector<PlannedBurst> plan_generic_access(std::uint64_t addr, std::uint64_t length,
                                              unsigned bus_bytes, bool write);

} // namespace fx1::ddr3
