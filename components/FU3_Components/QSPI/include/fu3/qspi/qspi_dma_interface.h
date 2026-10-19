#pragma once
#include "registers.h"

namespace fu3::qspi {
class QspiDmaInterface {
public:
    struct Input {
        std::uint32_t cr=0,dmacr=0,dlr=0;
        unsigned mode=0,data_mode=0,rx_bytes=0,tx_free=0;
        bool writing=false,busy=false,serial_done=false,rx_clear=false,tx_clear=false;
        std::uint64_t total=0,accepted=0;
    };
    void reset() { rx_=tx_=armed_=false; }
    void arm(bool value=true) { armed_=value; }
    bool rx_request() const { return rx_; }
    bool tx_request() const { return tx_; }
    void tick(const Input& in) {
        const unsigned rx_threshold=((in.cr>>3)&31)+1,tx_threshold=((in.cr>>8)&31)+1;
        // Register description 1.14, TXDMAEN (p.13): free FIFO words and
        // "not all DLR+1 bytes written" are the two request conditions.
        // Do not invent an additional remaining-length >= burst-size gate.
        const bool remaining=in.writing ? in.total>in.accepted :
            !in.busy && !in.serial_done && in.data_mode;
        rx_=(in.cr&reg::EN) && in.mode==1 && (in.dmacr&reg::RXDMAEN) &&
            (in.rx_bytes+3)/4>=rx_threshold && !in.rx_clear;
        tx_=armed_ && (in.cr&reg::EN) && in.mode==0 && (in.dmacr&reg::TXDMAEN) &&
            in.tx_free/4>=tx_threshold && !in.tx_clear && remaining;
    }
private:
    bool rx_=false,tx_=false,armed_=false;
};
}
