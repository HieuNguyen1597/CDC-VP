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
    bool fault_read=false, fault_write=false;
    bool saw_unit[5]{};
    SC_CTOR(Bench) { ram.register_b_transport(this,&Bench::transport); SC_THREAD(run); }
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
    void run() {
        reset_n.write(false); wait(2,sc_core::SC_NS); reset_n.write(true); wait(1,sc_core::SC_NS);
        for(unsigned i=0;i<memory.size();++i) memory[i]=(i*37+11)&255;
        // Different read/write alignment and tails; also cross source/destination pages.
        for (unsigned n=0;n<8;++n) {
            unsigned src=0xffc+(n%4), dst=0x5000+(n%3), size= n==7?1023:257+n;
            std::vector<unsigned char> expected(memory.begin()+src,memory.begin()+src+size);
            configure(n,src,dst,size,0x8101007f,0x8101001f);
            const auto before=transactions;
            wait(1,sc_core::SC_NS); check(before==transactions,"enable does not start");
            reg(true,n*256+0x44,1);
            check(reg(false,n*256+0x48)==3,"active immediately after start");
            reg(true,n*256,0,tlm::TLM_COMMAND_ERROR_RESPONSE);
            done(n);
            check(std::equal(expected.begin(),expected.end(),memory.begin()+dst),"byte-for-byte copy");
            check(reg(false,n*256+0x50)==0x10001,"command and event counters");
            check(reg(false,n*256+0xa0)==1,"raw completion");
            check(irq.read(),"completion IRQ");
            reg(true,n*256+0xa8,0); settle(); check(!irq.read(),"IRQ masked");
            reg(true,n*256+0xa8,0x1fff); settle(); check(irq.read(),"IRQ unmasked");
            reg(true,n*256+0xa4,1); settle(); check(!irq.read(),"IRQ acknowledged");
            check(reg(false,n*256+0x50)==1,"event count decremented");
        }
        check(saw_unit[1]&&saw_unit[2]&&saw_unit[4],"all inferred unit sizes");
        // Fixed source repeats one aligned word, no increment on reads.
        configure(0,0x100,0x6000,16,0x01010004,0x81010008);
        reg(true,0x44,1); done(0);
        for(unsigned i=0;i<16;++i) check(memory[0x6000+i]==memory[0x100+i%4],"fixed source");
        reg(true,0xa4,1);
        configure(0,0x200,0x7000,16,0x81010008,0x01010004);
        std::vector<unsigned char> last(memory.begin()+0x20c,memory.begin()+0x210);
        reg(true,0x44,1); done(0);
        check(std::equal(last.begin(),last.end(),memory.begin()+0x7000),"fixed destination");
        reg(true,0xa4,1);
        // Group start and independent channel results.
        configure(0,0x200,0x7100,64); configure(1,0x300,0x7200,64);
        reg(true,0x1048,3); done(0); done(1);
        check(reg(false,0x1000)==3,"group completion bitmap");
        check(std::equal(memory.begin()+0x200,memory.begin()+0x240,memory.begin()+0x7100),"group channel0");
        check(std::equal(memory.begin()+0x300,memory.begin()+0x340,memory.begin()+0x7200),"group channel1");
        reg(true,0xa4,1); reg(true,0x1a4,1);
        // Response errors must not generate successful completion.
        configure(0,0x200,0x7400,16); fault_read=true; reg(true,0x44,1); done(0); fault_read=false;
        check(reg(false,0xa0)==8 && reg(false,0x50)==0,"read DECERR");
        configure(0,0x200,0x7400,16); fault_write=true; reg(true,0x44,1); done(0); fault_write=false;
        check(reg(false,0xa0)==4 && reg(false,0x50)==0,"write SLVERR");
        // Disable pauses new work, reenable resumes; accepted transaction drains.
        configure(0,0x200,0x7500,128); reg(true,0x44,1); wait(1,sc_core::SC_NS);
        reg(true,0x40,0); wait(3,sc_core::SC_NS);
        unsigned paused=transactions; wait(5,sc_core::SC_NS);
        check(transactions==paused,"disabled channel issues no new work");
        reg(true,0x40,1); done(0);
        // Reset while an annotated response is pending must discard stale completion.
        configure(0,0x200,0x7600,128); reg(true,0x44,1); settle();
        reset_n.write(false); wait(2,sc_core::SC_NS); reset_n.write(true); wait(2,sc_core::SC_NS);
        check(reg(false,0x48)==0 && reg(false,0xa0)==0 && reg(false,0x50)==0,"reset drops stale engine completion");
        check(!irq.read(),"reset clears IRQ");
        configure(0,0x200,0x7800,8); reg(true,0x44,1); done(0);
        check(reg(false,0xa0)==1,"restart after reset");
        configure(0,0x200,0x7800,0); reg(true,0x44,1,tlm::TLM_COMMAND_ERROR_RESPONSE);
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
    sc_core::sc_start(); std::cout << "M2M errors: " << bench.errors << '\n';
    return bench.errors?1:0;
}
