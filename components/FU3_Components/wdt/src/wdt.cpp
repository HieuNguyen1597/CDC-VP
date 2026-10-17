#include "fu3_wdt/wdt.h"
#include <algorithm>
#include <stdexcept>

namespace fu3_wdt {
Watchdog::Watchdog(sc_core::sc_module_name name,sc_core::sc_time tick,sc_core::sc_time latency)
    :sc_module(name),tick_(tick),latency_(latency) {
    if(tick==sc_core::SC_ZERO_TIME || latency==sc_core::SC_ZERO_TIME)
        throw std::invalid_argument("WDT tick and access latency must be positive");
    socket.register_b_transport(this,&Watchdog::transport);
    socket.register_transport_dbg(this,&Watchdog::debug);
    SC_THREAD(run);
    SC_METHOD(publish);sensitive<<outputs_;
}
void Watchdog::timeout() {
    raw_=true;
    if(seen_) {
        seen_=false;
        if(control_&FU3_WDT_RESEN) {
            const unsigned div=(control_&FU3_WDT_RESDIV_MASK)>>FU3_WDT_RESDIV_SHIFT;
            // Reserved encodings use one tick in this VP contract.
            pulse_until_=last_tick_+tick_*double(div<=8 ? (1u<<div) : 1u);
        }
    } else seen_=true;
    count_=load_;
}
void Watchdog::synchronize() {
    const auto now=sc_core::sc_time_stamp();
    auto ticks=(now.value()-last_tick_.value())/tick_.value();
    // Skip inactive ticks and counter values between externally visible events.
    while(ticks && running_) {
        const std::uint64_t distance=count_ ? count_ : 1;
        const auto step=std::min<std::uint64_t>(ticks,distance);
        last_tick_+=sc_core::sc_time::from_value(step*tick_.value());
        ticks-=step;
        if(step==distance) timeout();
        else count_-=static_cast<std::uint32_t>(step);
    }
    if(ticks) last_tick_+=sc_core::sc_time::from_value(ticks*tick_.value());
    if(!preset_n.read()) {load_=UINT32_MAX;control_=FU3_WDT_RESEN;locked_=false;}
    if(!wdogres_n.read()) {count_=UINT32_MAX;raw_=false;seen_=false;pulse_until_=sc_core::SC_ZERO_TIME;}
    pause_=preset_n.read() && (control_&FU3_WDT_DBGEN) && debug_halt.read();
    running_=preset_n.read() && wdogres_n.read() && clock_enabled.read() &&
        (control_&FU3_WDT_INTEN) && !pause_;
    outputs_.notify(sc_core::SC_ZERO_TIME);
}
void Watchdog::run() {
    while(true) {
        synchronize();
        auto events=changed_|preset_n.value_changed_event()|wdogres_n.value_changed_event()|
            clock_enabled.value_changed_event()|debug_halt.value_changed_event();
        const auto now=sc_core::sc_time_stamp();
        auto next=sc_core::sc_max_time();
        if(running_) next=last_tick_+tick_*double(count_ ? count_ : 1);
        if(pulse_until_>now) next=std::min(next,pulse_until_);
        if(next==sc_core::sc_max_time()) wait(events);
        else wait(next-now,events);
    }
}
void Watchdog::publish() {
    irq.write(raw_ && (control_&FU3_WDT_INTEN));
    reset_request.write(wdogres_n.read() && (control_&FU3_WDT_RESEN) && sc_core::sc_time_stamp()<pulse_until_);
    paused.write(pause_);
}
std::uint32_t Watchdog::read(unsigned offset) const {
    switch(offset) {
    case FU3_WDT_LOCK:return locked_;
    case FU3_WDT_LOAD:return load_;
    case FU3_WDT_VALUE:return count_;
    case FU3_WDT_CONTROL:return control_;
    case FU3_WDT_RAW:return unsigned(raw_)|(unsigned(seen_)<<1);
    case FU3_WDT_MASKED:return raw_ && (control_&FU3_WDT_INTEN);
    case FU3_WDT_DEBUG:return pause_;
    default:return 0; // Write-only CLEAR reads as zero.
    }
}
void Watchdog::write(unsigned offset,std::uint32_t value,std::uint32_t mask) {
    if(!mask) return;
    if(offset==FU3_WDT_LOCK) {
        // Unlock requires the complete key in a single write; any other active write locks.
        locked_=mask!=UINT32_MAX || value!=FU3_WDT_UNLOCK_KEY;return;
    }
    if(locked_) return;
    if(offset==FU3_WDT_LOAD) {
        load_=(load_&~mask)|(value&mask);count_=load_;seen_=false;
        if(!load_) {raw_=true;seen_=true;} // Raw event even if INTEN masks the output.
    } else if(offset==FU3_WDT_CONTROL) {
        control_=((control_&~mask)|(value&mask))&0x7f;
    } else if(offset==FU3_WDT_CLEAR) {
        raw_=false;seen_=false;count_=load_;
    }
}
bool Watchdog::validate(tlm::tlm_generic_payload& tx) {
    tx.set_dmi_allowed(false);
    auto fail=[&](tlm::tlm_response_status s){tx.set_response_status(s);return false;};
    if(!tx.is_read() && !tx.is_write())return fail(tlm::TLM_COMMAND_ERROR_RESPONSE);
    if(!tx.get_data_ptr())return fail(tlm::TLM_GENERIC_ERROR_RESPONSE);
    const auto n=tx.get_data_length();const auto a=tx.get_address();
    if((n!=1 && n!=2 && n!=4)||tx.get_streaming_width()<n)return fail(tlm::TLM_BURST_ERROR_RESPONSE);
    if(a<0x1000 || a>=0x1020 || a%n || (a&3)+n>4)return fail(tlm::TLM_ADDRESS_ERROR_RESPONSE);
    if(auto* be=tx.get_byte_enable_ptr()) {
        const auto len=tx.get_byte_enable_length();
        if(!len)return fail(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
        for(unsigned i=0;i<n;++i)if(be[i%len]!=0 && be[i%len]!=0xff)return fail(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
    }
    return true;
}
void Watchdog::read_bytes(tlm::tlm_generic_payload& tx) {
    const auto value=read(static_cast<unsigned>(tx.get_address()&~3ull));
    for(unsigned i=0;i<tx.get_data_length();++i)
        if(!tx.get_byte_enable_ptr() || tx.get_byte_enable_ptr()[i%tx.get_byte_enable_length()])
            tx.get_data_ptr()[i]=static_cast<unsigned char>(value>>(8*((tx.get_address()&3)+i)));
}
void Watchdog::transport(tlm::tlm_generic_payload& tx,sc_core::sc_time& delay) {
    auto incoming=delay;delay=sc_core::SC_ZERO_TIME;
    wait(incoming+latency_); // Yield even for errors; writes are atomic at completion.
    synchronize();
    if(!validate(tx))return;
    if(!preset_n.read() || !wdogres_n.read()) {tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);return;}
    if(tx.is_read())read_bytes(tx);
    else {
        std::uint32_t value=0,mask=0;
        for(unsigned i=0;i<tx.get_data_length();++i)
            if(!tx.get_byte_enable_ptr() || tx.get_byte_enable_ptr()[i%tx.get_byte_enable_length()]) {
                const unsigned shift=8*((tx.get_address()&3)+i);
                mask|=0xffu<<shift;value|=std::uint32_t(tx.get_data_ptr()[i])<<shift;
            }
        write(static_cast<unsigned>(tx.get_address()&~3ull),value,mask);
    }
    synchronize();changed_.notify(sc_core::SC_ZERO_TIME);
    tx.set_response_status(tlm::TLM_OK_RESPONSE);
}
unsigned Watchdog::debug(tlm::tlm_generic_payload& tx) {
    if(!validate(tx))return 0;
    if(!tx.is_read()){tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);return 0;}
    synchronize();read_bytes(tx);tx.set_response_status(tlm::TLM_OK_RESPONSE);
    return tx.get_data_length();
}
} // namespace fu3_wdt
