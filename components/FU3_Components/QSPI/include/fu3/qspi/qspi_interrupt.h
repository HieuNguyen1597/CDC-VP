#pragma once
#include <cstdint>

namespace fu3::qspi {
class QspiInterrupt {
public:
    bool evaluate(std::uint32_t cr,std::uint32_t sr,std::uint32_t dmacr,std::uint32_t dmasr) const {
        return (((cr>>16)&sr&0x3f)!=0) || ((dmacr&dmasr&0xc)!=0);
    }
};
}
