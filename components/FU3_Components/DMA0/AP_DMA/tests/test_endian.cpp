#include "fu3/dma/ap_dma.h"
#include <algorithm>
#include <iostream>
#include <vector>

struct Bench : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<Bench> cpu{"cpu"};
    tlm_utils::simple_target_socket<Bench> ram{"ram"};
    sc_core::sc_out<bool> reset_n{"reset_n"};
    sc_core::sc_in<bool> irq{"irq"};
    std::vector<unsigned char> memory = std::vector<unsigned char>(65536);
    unsigned errors = 0, transactions = 0;
    SC_CTOR(Bench) { ram.register_b_transport(this, &Bench::transport); SC_THREAD(run); }
    void check(bool ok, const char* message) {
        if (!ok) { ++errors; std::cerr << "FAIL: " << message << '\n'; }
    }
    void transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay) {
        ++transactions;
        const auto a = tx.get_address(); const auto n = tx.get_data_length();
        if (a >= memory.size() || n > memory.size() - a) {
            tx.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE); return;
        }
        if (tx.is_read()) std::copy_n(memory.data()+a,n,tx.get_data_ptr());
        else std::copy_n(tx.get_data_ptr(),n,memory.data()+a);
        delay += sc_core::sc_time(1, sc_core::SC_NS);
        tx.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    unsigned reg(bool write, unsigned a, unsigned value = 0,
                 tlm::tlm_response_status expected = tlm::TLM_OK_RESPONSE) {
        unsigned char data[4];
        for (unsigned i=0; i<4; ++i) data[i] = (value >> (8*i)) & 255;
        tlm::tlm_generic_payload tx;
        tx.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        tx.set_address(a); tx.set_data_ptr(data); tx.set_data_length(4); tx.set_streaming_width(4);
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME; cpu->b_transport(tx,delay);
        check(tx.get_response_status() == expected,"register response");
        value=0; for (unsigned i=0;i<4;++i) value |= unsigned(data[i]) << (8*i);
        return value;
    }
    void configure(unsigned n, unsigned mode, unsigned source, unsigned destination,
                   unsigned size, unsigned read_burst, unsigned write_burst) {
        const auto b = 256*n;
        reg(true,b+0x40,0); reg(true,b+0xa4,0x1fff);
        reg(true,b,source); reg(true,b+4,destination); reg(true,b+8,size); reg(true,b+12,3);
        reg(true,b+16,0x81010000|read_burst); reg(true,b+20,0x81010000|write_burst);
        reg(true,b+24,mode << 28); reg(true,b+0x40,1);
    }
    void done(unsigned n) {
        for (unsigned i=0; i<10000 && reg(false,n*256+0x48); ++i) wait(1,sc_core::SC_NS);
        check(reg(false,n*256+0x48)==0,"transfer finishes");
        for(unsigned i=0;i<4;++i) wait(sc_core::SC_ZERO_TIME);
    }
    void run() {
        reset_n.write(false); wait(2,sc_core::SC_NS); reset_n.write(true); wait(1,sc_core::SC_NS);
        // Known byte vectors, independent of bus endian or integer byte order.
        const std::vector<unsigned char> input={0x10,0x21,0x32,0x43,0x54,0x65,0x76,0x87};
        const std::vector<std::vector<unsigned char>> expected={
            input, {0x21,0x10,0x43,0x32,0x65,0x54,0x87,0x76},
            {0x43,0x32,0x21,0x10,0x87,0x76,0x65,0x54}};
        for (unsigned mode=0;mode<3;++mode) {
            std::copy(input.begin(),input.end(),memory.begin()+0x1000);
            configure(0,mode,0x1000,0x5000,8,1,3);
            reg(true,0x44,1); done(0);
            check(std::equal(expected[mode].begin(),expected[mode].end(),memory.begin()+0x5000),"known endian vector");
        }
        // Grouping must survive different alignments, bursts, FIFO and 4 KiB boundaries.
        for(unsigned mode=0;mode<3;++mode)
            for(unsigned alignment=0;alignment<4;++alignment)
                for(unsigned burst : {1u,3u,5u,31u,127u}) {
                    const unsigned src=0xffc+alignment, dst=0x5000+(3-alignment), size=260;
                    for(unsigned i=0;i<size;++i) memory[src+i]=(i*29+17)&255;
                    std::vector<unsigned char> result(size);
                    const unsigned group=1u<<mode;
                    for(unsigned i=0;i<size;++i) result[i]=memory[src+(i^(group-1))];
                    std::fill(memory.begin()+dst-1,memory.begin()+dst+size+1,0xcc);
                    configure(alignment,mode,src,dst,size,burst,7);
                    reg(true,alignment*256+0x44,1); done(alignment);
                    check(std::equal(result.begin(),result.end(),memory.begin()+dst),"stream swap across bursts");
                    check(memory[dst-1]==0xcc && memory[dst+size]==0xcc,"destination guards");
                    check(reg(false,alignment*256+0x50)==0x10001,"completion counters");
                    reg(true,alignment*256+0xa4,1);
                }
        // Two channels with different modes must never share partial-group state.
        std::copy(input.begin(),input.end(),memory.begin()+0x1000);
        configure(0,1,0x1000,0x6000,8,1,1); configure(1,2,0x1000,0x6100,8,1,3);
        reg(true,0x1048,3); done(0); done(1);
        check(std::equal(expected[1].begin(),expected[1].end(),memory.begin()+0x6000),"channel0 swap16");
        check(std::equal(expected[2].begin(),expected[2].end(),memory.begin()+0x6100),"channel1 swap32");
        // Reject undefined partial groups and reserved mode before side effects.
        for(auto mode : {1u,2u,3u}) {
            configure(0,mode,0x1000,0x6200,mode==3?8:3,1,1);
            const auto before=transactions;
            reg(true,0x44,1,tlm::TLM_COMMAND_ERROR_RESPONSE);
            wait(2,sc_core::SC_NS); check(before==transactions,"unsupported swap has no traffic");
        }
        // Reset discards a partial group; the next start cannot inherit old bytes.
        configure(0,2,0x1000,0x6300,8,1,1); reg(true,0x44,1);
        wait(2,sc_core::SC_NS); reset_n.write(false); wait(2,sc_core::SC_NS);
        reset_n.write(true); wait(2,sc_core::SC_NS);
        check(!irq.read() && reg(false,0x48)==0,"reset discards partial swap");
        configure(0,2,0x1000,0x6300,8,1,1); reg(true,0x44,1); done(0);
        check(std::equal(expected[2].begin(),expected[2].end(),memory.begin()+0x6300),"swap restart after reset");
        sc_core::sc_stop();
    }
};
int sc_main(int,char**) {
    fu3::dma::ApDma dma{"dma"}; Bench bench{"bench"};
    sc_core::sc_signal<bool> reset_n,irq;
    sc_core::sc_signal<sc_dt::sc_uint<32>> rx,tx,rc,tc;
    bench.cpu.bind(dma.target_socket); dma.master_socket.bind(bench.ram);
    dma.reset_n(reset_n); bench.reset_n(reset_n); dma.irq(irq); bench.irq(irq);
    dma.rx_request(rx); dma.tx_request(tx); dma.rx_clear(rc); dma.tx_clear(tc);
    sc_core::sc_start(); std::cout << "Endian errors: " << bench.errors << '\n';
    return bench.errors?1:0;
}
