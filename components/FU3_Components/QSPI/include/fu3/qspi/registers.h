#pragma once
#include <array>
#include <cstdint>

namespace fu3::qspi::reg {
inline constexpr unsigned CR=0x00, DCR=0x04, SR=0x08, FCR=0x0c,
    DLR=0x10, CCR=0x14, AR=0x18, ABR=0x1c, DR=0x20,
    PSMKR=0x24, PSMAR=0x28, PIR=0x2c, PTR=0x30,
    DMACR=0x34, DMASR=0x38, DMAFCR=0x3c;
inline constexpr std::uint32_t EN=1, ABORT=2, PTCEN=4,
    TEIE=1u<<16, TCIE=1u<<17, TXFTIE=1u<<18, RXFTIE=1u<<19,
    SMIE=1u<<20, STOIE=1u<<21, APMS=1u<<22, PMM=1u<<23;
inline constexpr std::uint32_t TEF=1, TCF=2, TXFTF=4, RXFTF=8,
    SMF=16, STOF=32, BUSY=64;
inline constexpr std::uint32_t TXDMAEN=1, RXDMAEN=2, TXOVRF=4, RXUDRF=8,
    SIOO=1u<<28, DDRM=1u<<31;
inline constexpr std::array<std::uint32_t,16> masks = {
    0xffff1fff, 0x001f0701, 0, 0x3f, 0xffffffff, 0x9f7fffff,
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    0xffff, 0xffffffff, 0xf, 0, 0xc};
inline constexpr std::uint64_t AP_QSPIC_BASE=0x44000000, XIP_BASE=0x30000000,
    XIP_SIZE=0x20000;
}
