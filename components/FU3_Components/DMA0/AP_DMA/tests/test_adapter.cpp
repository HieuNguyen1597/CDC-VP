#include "regression_bench.h"
#include "fu3/dma/peripheral_adapter.h"

struct AdapterTest : Regression {
    sc_core::sc_clock clock{"clock",sc_core::sc_time(10,sc_core::SC_NS)};
    fu3::dma::PeripheralAdapter adapter{"adapter"};
    sc_core::sc_signal<bool> valid,ready,last,ack_valid,ack_ready,ack_last,dma_request,dma_clear;
    sc_core::sc_signal<sc_dt::sc_uint<2>> kind,ack_kind;
    bool read;
    SC_HAS_PROCESS(AdapterTest);
    AdapterTest(sc_core::sc_module_name name,bool read_)
        : Regression(name,"adapter",61443),read(read_) {
        adapter.clock(clock);adapter.reset_n(reset_n);adapter.request_valid(valid);adapter.request_ready(ready);
        adapter.request_kind(kind);adapter.drlast(last);adapter.ack_valid(ack_valid);adapter.ack_ready(ack_ready);
        adapter.ack_kind(ack_kind);adapter.ack_last(ack_last);adapter.dma_request(dma_request);adapter.dma_clear(dma_clear);
        SC_METHOD(bridge);sensitive<<dma_request<<rc<<tc<<reset_n;
    }
    void bridge() {
        rx.write(read && reset_n.read() && dma_request.read()?4:0);
        tx.write(!read && reset_n.read() && dma_request.read()?2:0);
        dma_clear.write(reset_n.read() && ((read?rc.read():tc.read()).to_uint()&(read?4:2)));
    }
    void reset_adapter() {
        valid.write(false);ack_ready.write(false);reset_n.write(false);wait(20,sc_core::SC_NS);
        reset_n.write(true);wait(11,sc_core::SC_NS);
    }
    void submit(fu3::dma::RequestKind next,bool last_value) {
        kind.write(static_cast<unsigned>(next));settle();
        unsigned i=0;while(!ready.read() && i++<100) wait(1,sc_core::SC_NS);
        check(i<100,"adapter becomes request-ready");
        wait(clock.negedge_event());valid.write(true);last.write(last_value);
        wait(clock.posedge_event());settle();valid.write(false);
    }
    void ack() {
        unsigned i=0;while(!ack_valid.read() && i++<200)wait(1,sc_core::SC_NS);
        check(i<200,"adapter acknowledgement arrives");
    }
    unsigned bytes() { return read?received.size():sent.size(); }
    void setup(unsigned size) {
        configure(0,read?0x1e000:0x1000,read?0x8000:0x1f000,size);
        reg(true,16,read?0x01010001:0x81010008);reg(true,20,read?0x81010008:0x01010001);
        reg(true,32,read?2:(1u<<16));reg(true,68,1);
    }
    void run() override {
        reset_adapter();latency=1;memory[0x1000]=0xa1;memory[0x1001]=0xb2;
        setup(2);
        submit(fu3::dma::RequestKind::Single,true);ack();
        check(bytes()==1 && !dma_request.read(),"single accepted and released once");
        check(ack_kind.read()==0 && ack_last.read(),"latched SINGLE/DRLAST metadata");
        check(reg(false,72)!=0,"DRLAST does not terminate DMA command");
        valid.write(true);kind.write(1);wait(40,sc_core::SC_NS);
        check(!ready.read() && ack_valid.read() && bytes()==1,"ack backpressure holds one credit and metadata");
        check(ack_kind.read()==0,"pending request cannot overwrite acknowledged metadata");
        valid.write(false);ack_ready.write(true);wait(20,sc_core::SC_NS);ack_ready.write(false);
        submit(fu3::dma::RequestKind::Burst,false);ack();done(0);
        check(bytes()==2 && ack_kind.read()==1 && !ack_last.read(),"BURST request type preserved");
        if(read) check(memory[0x8000]==1 && memory[0x8001]==2,"adapter RX data");
        else check(sent==std::vector<unsigned char>({0xa1,0xb2}),"adapter TX data");
        ack_ready.write(true);wait(20,sc_core::SC_NS);ack_ready.write(false);
        received.clear();sent.clear();setup(1);wait(20,sc_core::SC_NS);
        const auto before=calls;
        submit(fu3::dma::RequestKind::Flush,true);ack();
        check(!dma_request.read() && bytes()==0 && calls==before,"FLUSH acknowledged without AXI data service");
        check(reg(false,72)!=0 && ack_kind.read()==2,"FLUSH does not complete payload command");
        // Reset while Ack is held and while Service is waiting.
        reset_adapter();check(!ack_valid.read() && !dma_request.read() && ready.read(),"reset clears held ack");
        submit(fu3::dma::RequestKind::Single,false);wait(20,sc_core::SC_NS);
        check(dma_request.read(),"adapter holds request until service clear");
        reset_adapter();check(!ack_valid.read() && !dma_request.read(),"reset cancels adapter service");
        kind.write(3);valid.write(true);wait(20,sc_core::SC_NS);
        check(!ready.read() && !dma_request.read() && !ack_valid.read(),"reserved request kind not accepted");
        std::cout<<(read?"adapter_rx":"adapter_tx")<<": checks="<<checks<<" errors="<<errors<<'\n';
        sc_core::sc_stop();
    }
};
int sc_main(int argc,char** argv) {
    if(argc!=2)return 2;
    AdapterTest bench{"bench",std::string(argv[1])=="rx"};sc_core::sc_start();return bench.errors?1:0;
}
