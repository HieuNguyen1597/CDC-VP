#include "fu3/dma/ap_dma.h"
#include "fu3/dma/transfer_extension.h"
#include <vector>
#include <algorithm>
#include <iostream>
struct Bench : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<Bench> cpu{"cpu"};
    tlm_utils::simple_target_socket<Bench> ram{"ram"};
    sc_core::sc_out<bool> reset_n{"reset_n"};
    sc_core::sc_in<bool> irq{"irq"};
    std::vector<unsigned char> memory = std::vector<unsigned char>(65536, 0);
    unsigned errors=0, transactions=0;
    sc_core::sc_out<sc_dt::sc_uint<32>> rx_request{"rx_request"}, tx_request{"tx_request"};
    sc_core::sc_in<sc_dt::sc_uint<32>> rx_clear{"rx_clear"}, tx_clear{"tx_clear"};
    std::vector<unsigned char> received, transmit;
    unsigned rx_pos=0, rx_pulses=0, tx_pulses=0;
    sc_core::sc_time last_read_response=sc_core::SC_ZERO_TIME, last_write_response=sc_core::SC_ZERO_TIME;
    sc_core::sc_time rx_rise=sc_core::SC_ZERO_TIME, tx_rise=sc_core::SC_ZERO_TIME;
    bool previous_rx=false, previous_tx=false;
    bool error_peripheral=false;
    bool fault_read=false, fault_write=false;
    bool saw_unit[5]{};
    SC_CTOR(Bench) { ram.register_b_transport(this,&Bench::transport); SC_THREAD(run);
        SC_METHOD(monitor_clear); sensitive << rx_clear << tx_clear;
    }
    void monitor_clear() {
        const bool r=rx_clear.read()!=0, t=tx_clear.read()!=0;
        if (r && !previous_rx) {
            ++rx_pulses; rx_rise=sc_core::sc_time_stamp();
            check(rx_rise>=last_read_response,"RX clear after read response");
        }
        if (t && !previous_tx) {
            ++tx_pulses; tx_rise=sc_core::sc_time_stamp();
            check(tx_rise>=last_write_response,"TX clear after write response");
        }
        if (!r && previous_rx && reset_n.read())
            check(sc_core::sc_time_stamp()-rx_rise==sc_core::sc_time(10,sc_core::SC_NS),"RX pulse one cycle");
        if (!t && previous_tx && reset_n.read())
            check(sc_core::sc_time_stamp()-tx_rise==sc_core::sc_time(10,sc_core::SC_NS),"TX pulse one cycle");
        previous_rx=r; previous_tx=t;
    }
    void check(bool ok,const char* message) {
        if (!ok) { ++errors; std::cerr << "FAIL " << message << '\n'; }
    }
    void transport(tlm::tlm_generic_payload& tx,sc_core::sc_time& delay) {
        ++transactions;
        auto* ext=tx.get_extension<fu3::dma::TransferExtension>();
        check(ext!=nullptr,"metadata");
        if (ext) {
            check(ext->unit_bytes==1 || ext->unit_bytes==2 || ext->unit_bytes==4,"unit size");
            check(tx.get_address()%ext->unit_bytes==0,"aligned beat");
            check(ext->beats>=1 && ext->beats<=16,"16 beat limit");
            check(tx.get_data_length()==ext->beats*ext->unit_bytes,"burst length");
            check(tx.get_data_length()<=32,"FIFO capacity");
            check((tx.get_address()&4095)+tx.get_data_length()<=4096,"4 KiB split");
            check(ext->axi_id==4,"AXI ID");
            saw_unit[ext->unit_bytes]=true;
        }
        delay+=sc_core::sc_time(1,sc_core::SC_NS);
        if (tx.get_address()==0x9000 || tx.get_address()==0xa000) {
            if (error_peripheral) { tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE); return; }
            // Exercise a target which waits and also annotates response delay.
            wait(2,sc_core::SC_NS);
            const auto len=tx.get_data_length();
            if (tx.get_address()==0x9000 && tx.is_read()) {
                check(rx_pos+len<=received.size(),"RX queue bounds");
                if (rx_pos+len<=received.size()) std::copy_n(received.begin()+rx_pos,len,tx.get_data_ptr());
                rx_pos+=len; last_read_response=sc_core::sc_time_stamp()+delay;
            } else if (tx.get_address()==0xa000 && tx.is_write()) {
                transmit.insert(transmit.end(),tx.get_data_ptr(),tx.get_data_ptr()+len);
                last_write_response=sc_core::sc_time_stamp()+delay;
            } else check(false,"peripheral direction");
            tx.set_response_status(tlm::TLM_OK_RESPONSE); return;
        }
        if ((fault_read && tx.is_read()) || (fault_write && tx.is_write())) {
            tx.set_response_status(tx.is_read()?tlm::TLM_ADDRESS_ERROR_RESPONSE:tlm::TLM_GENERIC_ERROR_RESPONSE);
            return;
        }
        auto a=tx.get_address(); auto len=tx.get_data_length();
        if (a>=memory.size() || len>memory.size()-a) {
            tx.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE); return;
        }
        if (tx.is_read()) std::copy_n(memory.data()+a,len,tx.get_data_ptr());
        else std::copy_n(tx.get_data_ptr(),len,memory.data()+a);
        tx.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    std::uint32_t reg(bool write,unsigned address,std::uint32_t value=0,
                       tlm::tlm_response_status expected=tlm::TLM_OK_RESPONSE) {
        unsigned char bytes[4];
        for(unsigned i=0;i<4;++i) bytes[i]=(value>>(8*i))&255;
        tlm::tlm_generic_payload tx;
        tx.set_command(write?tlm::TLM_WRITE_COMMAND:tlm::TLM_READ_COMMAND);
        tx.set_address(address); tx.set_data_ptr(bytes); tx.set_data_length(4); tx.set_streaming_width(4);
        sc_core::sc_time delay=sc_core::SC_ZERO_TIME; cpu->b_transport(tx,delay);
        check(tx.get_response_status()==expected,"register response");
        value=0; for(unsigned i=0;i<4;++i) value|=std::uint32_t(bytes[i])<<(8*i);
        return value;
    }
    void settle() { for(unsigned i=0;i<4;++i) wait(sc_core::SC_ZERO_TIME); }
    void configure(unsigned n,unsigned src,unsigned dst,unsigned size,
                   unsigned rc=0x81010008,unsigned wc=0x81010008,unsigned control=3) {
        unsigned b=n*256;
        reg(true,b+0x40,0); reg(true,b+0xa4,0x1fff);
        reg(true,b,src); reg(true,b+4,dst); reg(true,b+8,size); reg(true,b+12,control);
        reg(true,b+16,rc); reg(true,b+20,wc); reg(true,b+0x40,1);
    }
    void done(unsigned n) {
        for(unsigned i=0;i<5000 && reg(false,n*256+0x48);++i) wait(1,sc_core::SC_NS);
        check(reg(false,n*256+0x48)==0,"completion bounded"); settle();
    }
    void wait_clear(bool read,unsigned id) {
        for(unsigned i=0;i<1000;++i) {
            if ((read?rx_clear.read():tx_clear.read()).to_uint() & (1u<<id)) return;
            wait(1,sc_core::SC_NS);
        }
        check(false,"clear pulse timeout");
    }
    void run() {
        reset_n.write(false); wait(2,sc_core::SC_NS); reset_n.write(true); wait(1,sc_core::SC_NS);
        for(unsigned i=0;i<64;++i) memory[0x1000+i]=0x30+i;
        // M2P: memory prefetch allowed, writes gated by TX request. Burst 8 bytes.
        configure(0,0x1000,0xa000,16,0x81010008,0x01010008);
        reg(true,0x40,0); reg(true,0x20,1u<<16); reg(true,0x40,1); reg(true,0x44,1);
        wait(20,sc_core::SC_NS); check(transmit.empty(),"M2P waits for TX request");
        tx_request.write(1u<<1); wait_clear(false,1);
        check(transmit.size()==8 && tx_pulses==1,"one TX service consumes full burst credit");
        wait(30,sc_core::SC_NS); check(transmit.size()==8,"held TX request not reused");
        tx_request.write(0); wait(1,sc_core::SC_NS); tx_request.write(1u<<1);
        wait_clear(false,1); done(0);
        check(transmit.size()==16 && tx_pulses==2,"second TX credit");
        check(std::equal(transmit.begin(),transmit.end(),memory.begin()+0x1000),"M2P byte stream");
        tx_request.write(0); reg(true,0xa4,1); wait(12,sc_core::SC_NS);
        // P2M: fixed byte peripheral, four external credits with 3 cycle recovery.
        received={0x81,0x92,0xa3,0xb4}; rx_pos=0;
        configure(1,0x9000,0x2000,4,0x01010001,0x81010008);
        reg(true,0x140,0); reg(true,0x120,2u|(3u<<8)); reg(true,0x140,1); reg(true,0x144,1);
        const auto before=transactions; wait(20,sc_core::SC_NS);
        check(transactions==before && rx_pos==0,"P2M waits without bus polling");
        rx_request.write(1u<<3); wait(10,sc_core::SC_NS);
        check(rx_pos==0,"wrong peripheral ID does not trigger RX"); rx_request.write(0); settle();
        sc_core::sc_time previous=sc_core::SC_ZERO_TIME;
        for(unsigned i=0;i<4;++i) {
            rx_request.write(1u<<2); wait_clear(true,2);
            check(rx_pos==i+1,"one RX byte per request");
            if (i) check(rx_rise-previous>=sc_core::sc_time(30,sc_core::SC_NS),"RX recovery delay");
            previous=rx_rise;
            rx_request.write(0); wait(11,sc_core::SC_NS);
        }
        done(1); check(std::equal(received.begin(),received.end(),memory.begin()+0x2000),"P2M byte stream");
        reg(true,0x1a4,1);
        // Software requests: ID31, bit0 ignored, auto-clear after each service.
        configure(2,0x1000,0xa000,2,0x81010008,0x01010001);
        reg(true,0x240,0); reg(true,0x220,31u<<16); reg(true,0x240,1); reg(true,0x244,1);
        for(unsigned i=0;i<2;++i) {
            reg(true,0x1054,0x80000001); wait_clear(false,31);
            check(reg(false,0x1054)==0,"software TX request auto-clear");
            wait(11,sc_core::SC_NS);
        }
        done(2); reg(true,0x2a4,1);
        received={0x5a}; rx_pos=0;
        configure(3,0x9000,0x2100,1,0x01010001,0x81010008);
        reg(true,0x340,0); reg(true,0x320,2); reg(true,0x340,1); reg(true,0x344,1);
        reg(true,0x1050,1u<<2); wait_clear(true,2);
        check(reg(false,0x1050)==0,"software RX auto-clear"); done(3);
        check(memory[0x2100]==0x5a,"software RX data"); reg(true,0x3a4,1); wait(11,sc_core::SC_NS);
        // Error must not generate successful clear/complete.
        configure(0,0x1000,0xa000,1,0x81010008,0x01010001);
        reg(true,0x40,0); reg(true,0x20,1u<<16); reg(true,0x40,1); reg(true,0x44,1);
        unsigned pulses=tx_pulses; error_peripheral=true; tx_request.write(1u<<1); done(0);
        error_peripheral=false; check(tx_pulses==pulses && reg(false,0xa0)==4,"failed TX has no clear");
        tx_request.write(0);
        // Disabled channel cannot service a valid pending request.
        configure(0,0x1000,0xa000,1,0x81010008,0x01010001);
        reg(true,0x40,0); reg(true,0x20,1u<<16); reg(true,0x40,1); reg(true,0x44,1);
        settle(); reg(true,0x40,0); tx_request.write(1u<<1);
        const auto size_before=transmit.size(); wait(20,sc_core::SC_NS);
        check(transmit.size()==size_before,"disabled channel waits despite TX request");
        reg(true,0x40,1); wait_clear(false,1); done(0);
        tx_request.write(0); reg(true,0xa4,1); wait(11,sc_core::SC_NS);
        // Reset while waiting for a request must remove the active channel.
        configure(0,0x1000,0xa000,4,0x81010008,0x01010001);
        reg(true,0x40,0); reg(true,0x20,1u<<16); reg(true,0x40,1); reg(true,0x44,1);
        wait(5,sc_core::SC_NS); reset_n.write(false); wait(2,sc_core::SC_NS);
        check(rx_clear.read()==0 && tx_clear.read()==0,"reset removes clear pulses");
        reset_n.write(true); wait(2,sc_core::SC_NS);
        check(reg(false,0x48)==0 && !irq.read(),"reset clears request-wait state");
        // Swap grouping spans separate one-byte peripheral credits.
        transmit.clear();
        memory[0x1000]=0x10; memory[0x1001]=0x21; memory[0x1002]=0x32; memory[0x1003]=0x43;
        configure(0,0x1000,0xa000,4,0x81010001,0x01010001);
        reg(true,0x40,0); reg(true,0x18,1u<<28); reg(true,0x20,1u<<16); reg(true,0x40,1);
        reg(true,0x44,1);
        for(unsigned i=0;i<4;++i) {
            reg(true,0x1054,1u<<1); wait_clear(false,1); wait(11,sc_core::SC_NS);
        }
        done(0);
        check(transmit==std::vector<unsigned char>({0x21,0x10,0x43,0x32}),"M2P swap16 across byte requests");
        reg(true,0xa4,1);
        received={0x10,0x21,0x32,0x43}; rx_pos=0;
        configure(1,0x9000,0x2200,4,0x01010001,0x81010001);
        reg(true,0x140,0); reg(true,0x118,2u<<28); reg(true,0x120,2); reg(true,0x140,1);
        reg(true,0x144,1);
        for(unsigned i=0;i<4;++i) {
            reg(true,0x1050,1u<<2); wait_clear(true,2); wait(11,sc_core::SC_NS);
        }
        done(1);
        const std::vector<unsigned char> swapped={0x43,0x32,0x21,0x10};
        check(std::equal(swapped.begin(),swapped.end(),memory.begin()+0x2200),"P2M swap32 across byte requests");
        sc_core::sc_stop();
    }
};
int sc_main(int,char**) {
    fu3::dma::ApDma dma{"dma", sc_core::sc_time(10,sc_core::SC_NS)}; Bench bench{"bench"};
    sc_core::sc_signal<bool> reset_n,irq;
    sc_core::sc_signal<sc_dt::sc_uint<32>> rx,tx,rc,tc;
    bench.cpu.bind(dma.target_socket); dma.master_socket.bind(bench.ram);
    dma.reset_n(reset_n); bench.reset_n(reset_n); dma.irq(irq); bench.irq(irq);
    dma.rx_request(rx); bench.rx_request(rx); dma.tx_request(tx); bench.tx_request(tx);
    dma.rx_clear(rc); bench.rx_clear(rc); dma.tx_clear(tc); bench.tx_clear(tc);
    sc_core::sc_start(); std::cout << "Peripheral errors: " << bench.errors << '\n';
    return bench.errors?1:0;
}
