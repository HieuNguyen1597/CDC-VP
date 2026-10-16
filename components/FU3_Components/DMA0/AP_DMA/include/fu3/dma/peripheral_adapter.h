#pragma once
#include <systemc>

namespace fu3::dma {
// Functional wrapper contract; these enum values are model encodings, not
// asserted to be the RTL DTYPE bit encoding absent from the supplied HDS.
enum class RequestKind : unsigned { Single = 0, Burst = 1, Flush = 2 };
class PeripheralAdapter : public sc_core::sc_module {
public:
    sc_core::sc_in<bool> clock{"clock"}, reset_n{"reset_n"};
    sc_core::sc_in<bool> request_valid{"request_valid"};
    sc_core::sc_out<bool> request_ready{"request_ready"};
    sc_core::sc_in<sc_dt::sc_uint<2>> request_kind{"request_kind"};
    sc_core::sc_in<bool> drlast{"drlast"};
    sc_core::sc_out<bool> ack_valid{"ack_valid"};
    sc_core::sc_in<bool> ack_ready{"ack_ready"};
    sc_core::sc_out<sc_dt::sc_uint<2>> ack_kind{"ack_kind"};
    sc_core::sc_out<bool> ack_last{"ack_last"};
    sc_core::sc_out<bool> dma_request{"dma_request"};
    sc_core::sc_in<bool> dma_clear{"dma_clear"};

    SC_HAS_PROCESS(PeripheralAdapter);
    explicit PeripheralAdapter(sc_core::sc_module_name name) : sc_module(name) {
        SC_METHOD(tick);
        sensitive << clock.pos() << reset_n.neg();
        SC_METHOD(outputs);
        sensitive << changed_ << request_kind << reset_n;
    }
private:
    enum class State { Idle, Service, Ack };
    State state_ = State::Idle;
    sc_dt::sc_uint<2> kind_ = 0;
    bool last_ = false;
    sc_core::sc_event changed_;
    void tick() {
        if (!reset_n.read()) { state_=State::Idle; kind_=0; last_=false; }
        else if (state_==State::Idle && request_valid.read() && request_kind.read()!=3) {
            kind_=request_kind.read(); last_=drlast.read();
            state_=kind_==static_cast<unsigned>(RequestKind::Flush)?State::Ack:State::Service;
        } else if (state_==State::Service && dma_clear.read()) state_=State::Ack;
        else if (state_==State::Ack && ack_ready.read()) state_=State::Idle;
        changed_.notify(sc_core::SC_ZERO_TIME);
    }
    void outputs() {
        const bool enabled=reset_n.read();
        request_ready.write(enabled && state_==State::Idle && request_kind.read()!=3);
        dma_request.write(enabled && state_==State::Service);
        ack_valid.write(enabled && state_==State::Ack);
        ack_kind.write(kind_); ack_last.write(last_);
    }
};
}
