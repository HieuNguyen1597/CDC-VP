#pragma once
#include "fu3/dma/ap_dma.h"
#include "fu3/dma/transfer_extension.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

struct Regression : sc_core::sc_module {
    fu3::dma::ApDma dma{"dma", sc_core::sc_time(10, sc_core::SC_NS)};
    tlm_utils::simple_initiator_socket<Regression> cpu{"cpu"};
    tlm_utils::simple_target_socket<Regression> ram{"ram"};
    sc_core::sc_signal<bool> reset_n{"reset_n"}, irq{"irq"};
    sc_core::sc_signal<sc_dt::sc_uint<32>> rx{"rx"}, tx{"tx"}, rc{"rc"}, tc{"tc"};
    std::vector<unsigned char> memory = std::vector<unsigned char>(0x20000);
    std::string scenario;
    std::mt19937 random;
    unsigned errors = 0, checks = 0, transfers = 0, calls = 0;
    unsigned latency = 1;
    bool inject = false, inject_read = false;
    tlm::tlm_response_status injected = tlm::TLM_OK_RESPONSE;
    unsigned rx_calls = 0, tx_calls = 0;
    std::vector<unsigned char> received, sent;
    SC_HAS_PROCESS(Regression);
    Regression(sc_core::sc_module_name name, std::string scenario_, unsigned seed,
               const fu3::dma::EngineConfig& config = fu3::dma::EngineConfig{})
        : sc_module(name), dma("dma", sc_core::sc_time(10,sc_core::SC_NS),config), scenario(std::move(scenario_)), random(seed) {
        cpu.bind(dma.target_socket); dma.master_socket.bind(ram);
        dma.reset_n(reset_n); dma.irq(irq); dma.rx_request(rx); dma.tx_request(tx);
        dma.rx_clear(rc); dma.tx_clear(tc);
        ram.register_b_transport(this, &Regression::transport);
        SC_THREAD(run);
    }
    void check(bool ok, const char* reason) {
        ++checks;
        if (!ok) { ++errors; if (errors < 30) std::cerr << "FAIL: " << reason << " at " << sc_core::sc_time_stamp() << '\n'; }
    }
    unsigned reg(bool write, std::uint64_t address, unsigned value = 0,
                 tlm::tlm_response_status expected = tlm::TLM_OK_RESPONSE) {
        unsigned char data[4];
        for (unsigned i=0;i<4;++i) data[i]=(value>>(8*i))&255;
        tlm::tlm_generic_payload t;
        t.set_command(write?tlm::TLM_WRITE_COMMAND:tlm::TLM_READ_COMMAND);
        t.set_address(address); t.set_data_ptr(data); t.set_data_length(4); t.set_streaming_width(4);
        sc_core::sc_time delay=sc_core::SC_ZERO_TIME; cpu->b_transport(t,delay);
        check(t.get_response_status()==expected,"register response");
        value=0; for(unsigned i=0;i<4;++i) value|=unsigned(data[i])<<(8*i);
        return value;
    }
    void settle() { for(unsigned i=0;i<5;++i) wait(sc_core::SC_ZERO_TIME); }
    void reset() {
        rx.write(0); tx.write(0); reset_n.write(false); wait(20,sc_core::SC_NS);
        reset_n.write(true); wait(1,sc_core::SC_NS);
    }
    void configure(unsigned n,unsigned src,unsigned dst,unsigned size,unsigned mode=0,
                   unsigned rb=8,unsigned wb=8,unsigned control=3) {
        unsigned b=n*256;
        reg(true,b+0x40,0); reg(true,b+0xa4,0x1fff);
        reg(true,b,src); reg(true,b+4,dst); reg(true,b+8,size); reg(true,b+12,control);
        reg(true,b+16,0x81010000|rb); reg(true,b+20,0x81010000|wb);
        reg(true,b+24,mode<<28); reg(true,b+0x20,0); reg(true,b+0x40,1);
    }
    void done(unsigned n) {
        unsigned k=0;
        while(reg(false,n*256+0x48) && k++<10000) wait(1,sc_core::SC_NS);
        check(k<10000,"bounded completion"); settle(); ++transfers;
    }
    virtual void transport(tlm::tlm_generic_payload& t,sc_core::sc_time& delay) {
        ++calls;
        const auto* e=t.get_extension<fu3::dma::TransferExtension>();
        check(e!=nullptr,"data metadata present");
        if(e) {
            check(e->channel<8 && e->axi_id==4,"channel/id metadata");
            check(e->unit_bytes==1 || e->unit_bytes==2 || e->unit_bytes==4,"legal unit");
            check(t.get_address()%e->unit_bytes==0,"aligned unit");
            check(e->beats && e->beats<=16 && e->beats*e->unit_bytes==t.get_data_length(),"beat budget");
            check((t.get_address()&4095)+t.get_data_length()<=4096,"page boundary");
            check(t.get_data_length()<=32,"buffer budget");
            const auto pending=reg(false,e->channel*256+0x3c);
            check((t.is_read()?(pending&15):((pending>>8)&15))>=1,"in-flight pending status");
            const auto limit=[&](bool read) {
                const auto cfg=reg(false,e->channel*256+(read?16:20));
                const auto pc=reg(false,e->channel*256+32);
                if (((pc>>(read?0:16))&31) || !(cfg&0x40000000)) return 1u;
                return std::clamp((cfg>>24)&15u,1u,4u);
            };
            check((pending&15)<=limit(true) && ((pending>>8)&15)<=limit(false),"endpoint pending limits");
            const auto fifo=reg(false,e->channel*256+0x38);
            check((fifo&63)<=32 && ((fifo>>16)&63)<=32,"FIFO status range");
            if(!e->command_fetch) check((fifo&63)+((fifo>>16)&63)+t.get_data_length()<=32,"in-flight reservation included in FIFO budget");
            const auto config=reg(false,e->channel*256+(t.is_read()?16:20));
            if(!e->command_fetch) check(t.get_data_length()<=(config&127),"configured burst cap");
        }
        if(latency&2) wait(2,sc_core::SC_NS);
        if(latency&1) delay+=sc_core::sc_time(3,sc_core::SC_NS);
        if(inject && t.is_read()==inject_read) { t.set_response_status(injected); return; }
        if(t.get_address()==0x1e000 && t.is_read()) {
            ++rx_calls; const unsigned n=t.get_data_length();
            const auto i=received.size(); for(unsigned j=0;j<n;++j) { t.get_data_ptr()[j]=(i+j+1)&255; received.push_back(t.get_data_ptr()[j]); }
        } else if(t.get_address()==0x1f000 && t.is_write()) {
            ++tx_calls; sent.insert(sent.end(),t.get_data_ptr(),t.get_data_ptr()+t.get_data_length());
        } else {
            const auto a=t.get_address(); const auto len=t.get_data_length();
            if(a>=memory.size() || len>memory.size()-a) { t.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE); return; }
            if(t.is_read()) std::copy_n(memory.data()+a,len,t.get_data_ptr());
            else std::copy_n(t.get_data_ptr(),len,memory.data()+a);
        }
        t.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void copy(unsigned n,unsigned size,unsigned sa,unsigned da,unsigned rb,unsigned wb,unsigned mode,unsigned timing) {
        latency=timing;
        unsigned src=0x1ff0+sa,dst=0x8000+da;
        for(unsigned i=0;i<size;++i) memory[src+i]=random()&255;
        std::vector<unsigned char> expected(size);
        const unsigned group=1u<<mode;
        for(unsigned i=0;i<size;++i) expected[i]=memory[src+(i^(group-1))];
        std::fill(memory.begin()+dst-1,memory.begin()+dst+size+1,0xcc);
        configure(n,src,dst,size,mode,rb,wb);
        reg(true,n*256+0x44,1); done(n);
        check(std::equal(expected.begin(),expected.end(),memory.begin()+dst),"copy oracle");
        check(memory[dst-1]==0xcc && memory[dst+size]==0xcc,"destination guards");
        check(reg(false,n*256+0xa0)==1 && reg(false,n*256+0x50)==0x10001,"completion status");
        reg(true,n*256+0xa4,1); settle(); check(!irq.read(),"IRQ cleared");
    }
    void matrix() {
        for(unsigned mode=0;mode<3;++mode)
            for(unsigned len : {1u,2u,3u,4u,7u,8u,15u,16u,31u,32u,33u,63u,64u,65u,255u,256u,257u,1020u,1023u})
                if(len%(1u<<mode)==0)
                    for(unsigned sa=0;sa<4;++sa) for(unsigned da=0;da<4;++da)
                        copy((sa+da+mode)%8,len,sa,da,1+((sa*31+len)%127),1+((da*17+len)%127),mode,(sa+da)%4);
    }
    void randomized() {
        for(unsigned i=0;i<600;++i) {
            unsigned mode=random()%3, group=1u<<mode;
            unsigned size=group*(1+random()%(1023/group));
            const unsigned n=random()%8, sa=random()%17, da=random()%8;
            const unsigned rb=1+random()%127,wb=1+random()%127,timing=random()%4;
            copy(n,size,sa,da,rb,wb,mode,timing);
        }
    }
    void errors_and_control() {
        for(bool read : {false,true}) for(bool decode : {false,true}) {
            reset(); configure(0,0x1000,0x8000,64);
            inject=true; inject_read=read; injected=decode?tlm::TLM_ADDRESS_ERROR_RESPONSE:tlm::TLM_GENERIC_ERROR_RESPONSE;
            reg(true,0x44,1); done(0); inject=false;
            const unsigned bit=read?(decode?3:1):(decode?4:2);
            check(reg(false,0xa0)==(1u<<bit) && reg(false,0x50)==0,"exact bus fault/no completion");
            check(reg(false,0x3c)==0 && irq.read(),"fault drains pending and asserts IRQ");
            reg(true,0xa4,1); settle(); check(irq.read(),"clearing wrong source preserves IRQ");
            reg(true,0xa8,0); settle(); check(!irq.read(),"error IRQ mask");
            reg(true,0xa8,0x1fff); settle(); check(irq.read(),"error IRQ unmask");
            reg(true,0xa4,1u<<bit); settle(); check(!irq.read(),"error IRQ acknowledge");
            configure(0,0x1000,0x8000,64); reg(true,0x44,1); done(0);
            check(reg(false,0xa0)==1,"retry after bus error");
        }
        reset(); latency=3;
        configure(0,0x1000,0x8000,128); reg(true,0x44,1);
        reg(true,0x44,1,tlm::TLM_COMMAND_ERROR_RESPONSE);
        reg(true,0,0x2000,tlm::TLM_COMMAND_ERROR_RESPONSE);
        wait(1,sc_core::SC_NS); reg(true,0x40,0); wait(10,sc_core::SC_NS);
        const auto paused=calls; wait(10,sc_core::SC_NS); check(calls==paused,"disable no grants");
        reg(true,0x40,1); done(0);
        configure(0,0x1000,0x8000,16,0,8,8,2); reg(true,0x44,1); done(0);
        check(reg(false,0xa0)==0 && reg(false,0x50)==1 && !irq.read(),"SET_INT=0");
        // Atomic group validation; one disabled channel must prevent partial start.
        configure(0,0x1000,0x8000,16); configure(1,0x1100,0x8100,16); reg(true,0x140,0);
        reg(true,0x1048,3,tlm::TLM_COMMAND_ERROR_RESPONSE); check(reg(false,0x48)==0,"failed group start is atomic");
        reg(true,0x140,1); reg(true,0x1048,0xffff0003); done(0); done(1);
        check(reg(false,0x1000)==3,"group IRQ bitmap");
        reg(true,0xa4,1); settle(); check(irq.read() && reg(false,0x1000)==2,"one channel cannot clear another IRQ");
        reg(true,0x1a4,1); settle(); check(!irq.read(),"last group IRQ clears");
        // Reset during an accepted read, and during an accepted write.
        for(bool write : {false,true}) {
            configure(0,0x1000,0x8000,128);
            reg(true,0x44,1);
            unsigned i=0; while(reg(false,0x3c)!=(write?0x100u:1u) && i++<100) wait(1,sc_core::SC_NS);
            check(i<100,"reach pending phase"); reset();
            check(reg(false,0x48)==0 && reg(false,0x3c)==0 && reg(false,0xa0)==0 && !irq.read(),"reset discards late result");
            copy(0,16,0,0,8,8,0,3);
        }
    }
    void decode() {
        const std::set<unsigned> ch_ok={0,4,8,12,16,20,24,28,32,36,48,52,56,60,44,64,72,80,160,168,172};
        const std::set<unsigned> ch_unsupported={68,164};
        const std::set<unsigned> gl_ok={0x1000,0x1030,0x1038,0x1040,0x1050,0x1054,0x10d0,0x10e0,0x10f0,0x10f4};
        const std::set<unsigned> gl_unsupported={0x1048};
        for(unsigned a=0;a<0x1100;a+=4) {
            auto expected=tlm::TLM_ADDRESS_ERROR_RESPONSE;
            if(a<0x800) {
                if(ch_ok.count(a%256)) expected=tlm::TLM_OK_RESPONSE;
                if(ch_unsupported.count(a%256)) expected=tlm::TLM_COMMAND_ERROR_RESPONSE;
            } else {
                if(gl_ok.count(a)) expected=tlm::TLM_OK_RESPONSE;
                if(gl_unsupported.count(a)) expected=tlm::TLM_COMMAND_ERROR_RESPONSE;
            }
            reg(false,a,0,expected);
        }
        for(unsigned a=0;a<0x1100;++a) if(a%4) reg(false,a,0,tlm::TLM_ADDRESS_ERROR_RESPONSE);
        unsigned char bytes[8]{}; unsigned char be[4]={255,255,255,255};
        for(unsigned kind=0;kind<6;++kind) {
            tlm::tlm_generic_payload t;
            t.set_command(tlm::TLM_READ_COMMAND); t.set_address(0);
            t.set_data_ptr(kind==0?nullptr:bytes); t.set_data_length(kind==1?8:4);
            t.set_streaming_width(kind==2?2:4);
            if(kind==3) t.set_byte_enable_ptr(be);
            if(kind==4) t.set_command(tlm::TLM_IGNORE_COMMAND);
            if(kind==5) reset_n.write(false);
            if(kind==5) wait(1,sc_core::SC_NS);
            sc_core::sc_time delay=sc_core::SC_ZERO_TIME; cpu->b_transport(t,delay);
            auto expected=kind<3?tlm::TLM_BURST_ERROR_RESPONSE:kind==3?tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE:tlm::TLM_COMMAND_ERROR_RESPONSE;
            check(t.get_response_status()==expected,"malformed/reset payload policy");
        }
    }
    void peripheral_ids() {
        latency=3;
        for(unsigned id=1;id<32;++id) for(bool read : {false,true}) {
            reset(); const unsigned n=id%8,b=n*256;
            received.clear(); sent.clear();
            configure(n,read?0x1e000:0x1000,read?0x8000:0x1f000,2);
            reg(true,b+16,read?0x01010001:0x81010008); reg(true,b+20,read?0x81010008:0x01010001);
            reg(true,b+32,read?id:(id<<16)); reg(true,b+68,1);
            wait(10,sc_core::SC_NS); check(received.empty()&&sent.empty(),"request gating");
            for(unsigned k=0;k<2;++k) {
                const auto address=read?0x1050:0x1054;
                reg(true,address,1u<<id);
                unsigned i=0; while(!((read?rc.read():tc.read()).to_uint()&(1u<<id)) && i++<100) wait(1,sc_core::SC_NS);
                check(i<100,"all IDs acknowledge");
                check(reg(false,address)==0,"all IDs software auto-clear");
                wait(11,sc_core::SC_NS);
            }
            done(n); check(reg(false,b+160)==1,"peripheral completes");
            if(read) check(received.size()==2 && memory[0x8000]==1 && memory[0x8001]==2,"RX bytes");
            else check(sent.size()==2 && sent[0]==memory[0x1000] && sent[1]==memory[0x1001],"TX bytes");
            received.clear(); sent.clear();
            configure(n,read?0x1e000:0x1000,read?0x8000:0x1f000,2);
            reg(true,b+16,read?0x01010001:0x81010008); reg(true,b+20,read?0x81010008:0x01010001);
            reg(true,b+32,read?id:(id<<16)); reg(true,b+68,1);
            for(unsigned k=0;k<2;++k) {
                if(read) rx.write(1u<<id); else tx.write(1u<<id);
                unsigned i=0; while(!((read?rc.read():tc.read()).to_uint()&(1u<<id)) && i++<100) wait(1,sc_core::SC_NS);
                check(i<100,"all external IDs acknowledge");
                if(read) rx.write(0); else tx.write(0);
                wait(11,sc_core::SC_NS);
            }
            done(n);
            if(read) check(received.size()==2 && memory[0x8000]==1 && memory[0x8001]==2,"external RX data");
            else check(sent.size()==2 && sent[0]==memory[0x1000] && sent[1]==memory[0x1001],"external TX data");
        }
        reset(); configure(0,0x1e000,0x1f000,1); reg(true,32,0x10001);
        reg(true,68,1,tlm::TLM_COMMAND_ERROR_RESPONSE); // P2P not advertised.
    }
    void group_channels() {
        latency=3;
        for(unsigned n=0;n<8;++n) {
            for(unsigned i=0;i<128;++i) memory[0x1000+n*256+i]=(n*31+i)&255;
            configure(n,0x1000+n*256,0x8000+n*256,128,n%3,1+n,3+n);
        }
        reg(true,0x1048,255);
        check(reg(false,0x10d0)==0,"core busy after group start");
        for(unsigned n=0;n<8;++n) done(n);
        check(reg(false,0x1000)==255 && irq.read(),"all eight IRQ sources visible");
        for(unsigned n=0;n<8;++n) {
            unsigned group=1u<<(n%3);
            for(unsigned i=0;i<128;++i)
                check(memory[0x8000+n*256+i]==memory[0x1000+n*256+(i^(group-1))],"all eight data contexts isolated");
            reg(true,n*256+0xa4,1); settle();
            check(reg(false,0x1000)==(255u & ~((1u<<(n+1))-1)),"selective per-channel acknowledge");
            check(irq.read()==(n!=7),"IRQ stays until last channel acknowledged");
        }
        check(reg(false,0x10d0)==1,"core idle after all channels");
    }
    void handshake() {
        latency=3;
        for(bool read : {false,true}) for(unsigned delay=0;delay<8;++delay) {
            reset(); received.clear(); sent.clear();
            configure(0,read?0x1e000:0x1000,read?0x8000:0x1f000,2);
            reg(true,16,read?0x01010001:0x81010008);
            reg(true,20,read?0x81010008:0x01010001);
            reg(true,32,read?(2u|(delay<<8)):((1u<<16)|(delay<<24)));
            reg(true,68,1);
            auto request=[&](bool high) { if(read) rx.write(high?4:0); else tx.write(high?2:0); };
            const unsigned bit=read?4:2;
            auto clear=[&]() { return ((read?rc.read():tc.read()).to_uint()&bit)!=0; };
            auto await=[&]() {
                unsigned i=0; while(!clear() && i++<200) wait(1,sc_core::SC_NS);
                check(i<200,"external clear arrives");
            };
            request(true); await(); const auto first=sc_core::sc_time_stamp();
            // Hold request after service longer than pulse and recovery period.
            wait(90,sc_core::SC_NS);
            check((read?received.size():sent.size())==1,"held external credit consumed once");
            request(false); wait(1,sc_core::SC_NS); request(true); await();
            check(sc_core::sc_time_stamp()-first>=sc_core::sc_time(10*delay,sc_core::SC_NS),"recovery respected");
            request(false); done(0);
            check((read?received.size():sent.size())==2,"rearmed request services next byte");
            wait(11,sc_core::SC_NS);
            // Exercise recovery while another software credit is ready early.
            reset(); received.clear(); sent.clear();
            configure(0,read?0x1e000:0x1000,read?0x8000:0x1f000,2);
            reg(true,16,read?0x01010001:0x81010008); reg(true,20,read?0x81010008:0x01010001);
            reg(true,32,read?(2u|(delay<<8)):((1u<<16)|(delay<<24))); reg(true,68,1);
            const unsigned sw=read?0x1050:0x1054;
            reg(true,sw,bit); await(); const auto service=sc_core::sc_time_stamp();
            reg(true,sw,bit);
            if(delay>1) {
                wait(10*delay-1,sc_core::SC_NS);
                check((read?received.size():sent.size())==1,"early request cannot bypass delay");
            }
            done(0);
            check(sc_core::sc_time_stamp()-service>=sc_core::sc_time(10*delay,sc_core::SC_NS),"delayed second credit");
            check((read?received.size():sent.size())==2,"second software credit retained");
        }
        // Two channels share one request ID. A held request must never be reused.
        reset(); received.clear(); sent.clear();
        for(unsigned n=0;n<2;++n) {
            configure(n,0x1000+n,0x1f000,1); reg(true,n*256+20,0x01010001); reg(true,n*256+32,1u<<16);
        }
        reg(true,0x1048,3); tx.write(2); wait(50,sc_core::SC_NS);
        check(sent.size()==1,"shared ID grants one external credit globally");
        tx.write(0); wait(1,sc_core::SC_NS); tx.write(2); done(0); done(1);
        check(sent.size()==2,"second channel progresses after new credit");
        tx.write(0); wait(11,sc_core::SC_NS);
    }
    virtual void run() {
        reset();
        if(scenario=="matrix") matrix();
        else if(scenario=="random") randomized();
        else if(scenario=="errors") errors_and_control();
        else if(scenario=="decode") decode();
        else if(scenario=="ids") peripheral_ids();
        else if(scenario=="channels") group_channels();
        else if(scenario=="handshake") handshake();
        else check(false,"unknown scenario");
        std::cout << scenario << ": checks=" << checks << " transfers=" << transfers << " bus_calls=" << calls << " errors=" << errors << '\n';
        sc_core::sc_stop();
    }
};
