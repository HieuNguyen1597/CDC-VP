#include "fu3/qspi/qspi_controller.h"
#include "fu3/qspi/qspi_ahb_adapter.h"
#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sc_core;
using namespace fu3::qspi;
namespace {
struct Record {
    FlashTransaction phase;
    tlm::tlm_command command;
    std::uint64_t address;
    std::vector<unsigned char> bytes;
    sc_time time;
};
// Virtual 4-GiB address space, so DLR/FSIZE boundaries never allocate gigabytes.
// Expected packing is checked against host byte vectors, not FIFO internals.
struct Target : sc_module {
    tlm_utils::simple_target_socket<Target> socket{"socket"};
    std::vector<Record> records;
    std::map<std::uint64_t,unsigned char> written;
    std::vector<std::uint32_t> statuses{0};
    unsigned polls=0;
    tlm::tlm_response_status response=tlm::TLM_OK_RESPONSE;
    bool throws=false;
    sc_time latency=SC_ZERO_TIME;
    explicit Target(sc_module_name n):sc_module(n) { socket.register_b_transport(this,&Target::transfer); }
    static unsigned char pattern(std::uint64_t a) { return static_cast<unsigned char>((a*29)^(a>>8)^0x96); }
    unsigned char byte(std::uint64_t a) const {
        const auto found=written.find(a); return found==written.end()?pattern(a):found->second;
    }
    void transfer(tlm::tlm_generic_payload& tx,sc_time& delay) {
        auto* m=tx.get_extension<FlashTransaction>();
        if (!m) throw std::runtime_error("missing flash extension");
        const auto n=tx.get_data_length();
        if (n>4) throw std::runtime_error("unbounded backend chunk");
        if (!m->cancel && throws) throw std::runtime_error("injected backend exception");
        if (!m->cancel && response!=tlm::TLM_OK_RESPONSE) { tx.set_response_status(response); return; }
        if (!m->cancel) {
            if (tx.is_read() && m->instruction==5) {
                const auto status=statuses[std::min<std::size_t>(polls++,statuses.size()-1)];
                for (unsigned i=0;i<n;++i) tx.get_data_ptr()[i]=static_cast<unsigned char>(status>>(8*i));
            } else for (unsigned i=0;i<n;++i) {
                if (tx.is_read()) tx.get_data_ptr()[i]=byte(tx.get_address()+i);
                if (tx.is_write()) written[tx.get_address()+i]=tx.get_data_ptr()[i];
            }
            delay+=latency;
        }
        std::vector<unsigned char> bytes;
        if (n) bytes.assign(tx.get_data_ptr(),tx.get_data_ptr()+n);
        records.push_back({*m,tx.get_command(),tx.get_address(),bytes,sc_time_stamp()});
        tx.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};
struct Reply { tlm::tlm_response_status status; std::uint32_t value; sc_time delay; };
struct Job {
    bool mapped=true,write=false,done=false;
    std::uint64_t address=0;
    std::uint32_t value=0;
    sc_time annotation=SC_ZERO_TIME;
    Reply reply{tlm::TLM_INCOMPLETE_RESPONSE,0,SC_ZERO_TIME};
    sc_time ended=SC_ZERO_TIME;
};
struct Bench : sc_module {
    Config config;
    sc_time core_period,p_period,h_period;
    sc_signal<bool> ck{"ck"},pc{"pc"},hc{"hc"};
    sc_signal<bool> rn{"rn"},prn{"prn"},hrn{"hrn"};
    sc_signal<bool> rxclr{"rxclr"},txclr{"txclr"},irq{"irq"},rxreq{"rxreq"},txreq{"txreq"};
    QspiController dut;
    QspiAhbAdapter bridge{"bridge"};
    Target target{"target"};
    tlm_utils::simple_initiator_socket<Bench> ap{"ap"},mm{"mm"};
    bool core_on=true,p_on=true,h_on=true,passed=false;
    std::string scenario;
    unsigned assertions=0;
    std::array<Job,2> jobs;
    sc_event go0,go1;
    SC_HAS_PROCESS(Bench);
    Bench(sc_module_name n,std::string test,unsigned depth,unsigned rx_depth):sc_module(n),
        config{depth,rx_depth,200,ThresholdUnit::Bytes},
        core_period(depth==32?10:6+(depth%5)*2,SC_NS),
        p_period(depth==32?14:11+(depth%4)*4,SC_NS),
        h_period(depth==32?22:7+(depth%7)*4,SC_NS),
        dut("dut",config),scenario(std::move(test)) {
        dut.clk(ck); dut.pclk(pc); dut.hclk(hc);
        dut.reset_n(rn); dut.preset_n(prn); dut.hreset_n(hrn);
        dut.dma_rx_clr(rxclr); dut.dma_tx_clr(txclr);
        dut.irq(irq); dut.dma_rx_req(rxreq); dut.dma_tx_req(txreq);
        ap.bind(dut.ap_qspic); mm.bind(bridge.socket); bridge.ahb.bind(dut.xip); dut.flash.bind(target.socket);
        SC_THREAD(core_clock); SC_THREAD(p_clock); SC_THREAD(h_clock);
        SC_THREAD(worker0); SC_THREAD(worker1); SC_THREAD(run);
    }
    void core_clock() { for (;;) { ck=false; wait(core_period/2); ck=core_on; wait(core_period/2); } }
    void p_clock() { for (;;) { pc=false; wait(p_period/2); pc=p_on; wait(p_period/2); } }
    void h_clock() { for (;;) { hc=false; wait(h_period/2); hc=h_on; wait(h_period/2); } }
    void check(bool ok,const std::string& what) {
        ++assertions; if (!ok) throw std::runtime_error(what+" at "+sc_time_stamp().to_string());
    }
    void settle() { wait(SC_ZERO_TIME); wait(SC_ZERO_TIME); }
    void tick(unsigned n=1) { for (unsigned i=0;i<n;++i) wait(ck.posedge_event()); settle(); }
    void until(const std::function<bool()>& predicate,const char* message,unsigned limit=20000) {
        for (unsigned i=0;i<limit;++i) { if (predicate()) return; tick(); }
        check(false,message);
    }
    Reply access(bool mapped,bool write,std::uint64_t address,std::uint32_t value=0,
                 unsigned n=4,unsigned width=4,unsigned char* enables=nullptr,unsigned enable_length=0,
                 bool null=false,int command=-1,sc_time annotation=SC_ZERO_TIME) {
        std::array<unsigned char,8> data{};
        for (unsigned i=0;i<4;++i) data[i]=static_cast<unsigned char>(value>>(8*i));
        tlm::tlm_generic_payload tx;
        const auto cmd=command<0?(write?tlm::TLM_WRITE_COMMAND:tlm::TLM_READ_COMMAND):static_cast<tlm::tlm_command>(command);
        tx.set_command(cmd); tx.set_address(address); tx.set_data_ptr(null?nullptr:data.data());
        tx.set_data_length(n); tx.set_streaming_width(width);
        tx.set_byte_enable_ptr(enables); tx.set_byte_enable_length(enable_length); tx.set_dmi_allowed(true);
        if (mapped) mm->b_transport(tx,annotation); else ap->b_transport(tx,annotation);
        check(!tx.is_dmi_allowed(),"DMI disabled on every response");
        check(tx.get_address()==address && tx.get_command()==cmd && tx.get_data_length()==n &&
              tx.get_streaming_width()==width && tx.get_data_ptr()==(null?nullptr:data.data()) &&
              tx.get_byte_enable_ptr()==enables && tx.get_byte_enable_length()==enable_length,"payload fields preserved");
        std::uint32_t result=0;
        for (unsigned i=0;i<4;++i) result|=std::uint32_t(data[i])<<(8*i);
        return {tx.get_response_status(),result,annotation};
    }
    void wr(unsigned offset,std::uint32_t value) { check(access(false,true,offset,value).status==tlm::TLM_OK_RESPONSE,"AP write"); }
    std::uint32_t rd(unsigned offset) {
        const auto result=access(false,false,offset); check(result.status==tlm::TLM_OK_RESPONSE,"AP read"); return result.value;
    }
    std::uint32_t peek(unsigned offset) {
        unsigned char data[4]{}; tlm::tlm_generic_payload tx;
        tx.set_command(tlm::TLM_READ_COMMAND); tx.set_address(offset); tx.set_data_ptr(data);
        tx.set_data_length(4); tx.set_streaming_width(4);
        check(ap->transport_dbg(tx)==4,"debug snapshot");
        std::uint32_t value=0; for (unsigned i=0;i<4;++i) value|=std::uint32_t(data[i])<<(8*i); return value;
    }
    void reset() {
        core_on=p_on=h_on=true; rn=false; prn=false; hrn=false; rxclr=false; txclr=false;
        wait(31,SC_NS); rn=true; prn=true; hrn=true; tick(2);
        target.latency=SC_ZERO_TIME; target.response=tlm::TLM_OK_RESPONSE; target.throws=false;
        target.records.clear(); target.written.clear(); target.statuses={0}; target.polls=0;
    }
    void setup(std::uint32_t bits=0,unsigned fsize=16,unsigned prescaler=1) {
        wr(reg::DCR,fsize<<16); wr(reg::CR,(prescaler<<24)|reg::EN|bits);
    }
    static std::uint32_t command(unsigned mode,unsigned ad=1,unsigned data=1,unsigned instruction=3) {
        return (mode<<26)|(data<<24)|(2u<<12)|(ad<<10)|(1u<<8)|instruction;
    }
    void read_start(std::uint32_t address,std::uint32_t dlr) {
        wr(reg::FCR,63); wr(reg::DLR,dlr); wr(reg::CCR,command(1)); wr(reg::AR,address);
    }
    void done() { until([&]{ return (peek(reg::SR)&reg::TCF)!=0; },"TCF timeout"); check(!(peek(reg::SR)&reg::TEF),"no transfer error"); }
    void abort() { wr(reg::CR,peek(reg::CR)|reg::ABORT); check(!(peek(reg::CR)&reg::ABORT) && !(peek(reg::SR)&reg::BUSY),"abort self clears and ends BUSY"); }
    void job(unsigned index) {
        auto& j=jobs[index]; j.reply=access(j.mapped,j.write,j.address,j.value,4,4,nullptr,0,false,-1,j.annotation);
        j.ended=sc_time_stamp(); j.done=true;
    }
    void worker0() { for (;;) { wait(go0); job(0); } }
    void worker1() { for (;;) { wait(go1); job(1); } }
    void launch(unsigned index,bool mapped,bool write,std::uint64_t address,std::uint32_t value=0,sc_time delay=SC_ZERO_TIME) {
        jobs[index]=Job{}; jobs[index].mapped=mapped; jobs[index].write=write;
        jobs[index].address=address; jobs[index].value=value; jobs[index].annotation=delay;
        (index?go1:go0).notify(SC_ZERO_TIME); settle();
    }
    void join(unsigned index) { until([&]{return jobs[index].done;},"worker timeout"); }
    static std::uint32_t word(const std::vector<unsigned char>& bytes,unsigned offset,unsigned n) {
        std::uint32_t value=0; for (unsigned i=0;i<n;++i) value|=std::uint32_t(bytes[offset+i])<<(8*i); return value;
    }
    void drain(const std::vector<unsigned char>& expected) {
        unsigned offset=0;
        while (offset<expected.size()) {
            until([&]{ return (peek(reg::SR)>>16)!=0; },"RX data timeout");
            const unsigned available=(peek(reg::SR)>>16)&63;
            const unsigned n=std::min(4u,available);
            check(offset+n<=expected.size(),"RX never supplies extra bytes");
            check(rd(reg::DR)==word(expected,offset,n),"independent RX packing oracle"); offset+=n;
        }
        done(); check(!(peek(reg::SR)&reg::BUSY),"RX drained clears BUSY");
    }
    void audit(std::uint64_t length,std::uint32_t address,tlm::tlm_command cmd) {
        std::uint64_t offset=0,id=0; unsigned begins=0,ends=0;
        for (const auto& r:target.records) {
            check(!r.phase.cancel && r.phase.offset==offset && r.phase.length==length,"frame offset/length integrity");
            if (!id) id=r.phase.frame;
            check(r.phase.frame==id && r.command==cmd && r.address==std::uint64_t(address)+offset,"frame identity/direction/address");
            check(r.bytes.size()<=4,"bounded chunk size");
            begins+=r.phase.begin; ends+=r.phase.end;
            check(r.phase.begin==(offset==0) && r.phase.end==(offset+r.bytes.size()==length),"one begin/end at exact boundaries");
            if (offset) check(r.phase.instruction_lines==0,"instruction not repeated between chunks");
            offset+=r.bytes.size();
        }
        check(offset==length && begins==1 && ends==1,"exact frame byte count");
    }
    void trigger_combinations();
    void dlr_boundaries();
    void fifo_packing();
    void irq_w1c();
    void active_reset();
    void bus_errors();
    void polling_edges();
    void dma_edges();
    void domains();
    void parameters();
    void run();
};

void Bench::trigger_combinations() {
    // R 1.6 p.7 / 1.7 p.9 / 1.9 p.10, including EN and absent instruction.
    for (unsigned enabled=0;enabled<2;++enabled) for (unsigned mode=0;mode<4;++mode)
        for (unsigned ad=0;ad<4;++ad) for (unsigned data=0;data<4;++data) for (unsigned im=0;im<2;++im) {
            reset(); wr(reg::DCR,16u<<16); wr(reg::DLR,0); wr(reg::PSMKR,1); wr(reg::PSMAR,0);
            wr(reg::CR,0x01400000|enabled);
            // Alternate phase keeps IMODE=0/no-address/no-data a nonempty frame.
            const auto c=(mode<<26)|(data<<24)|(2u<<12)|(ad<<10)|(im<<8)|(im?0:1u<<14)|5;
            wr(reg::CCR,c);
            const bool invalid_poll=enabled && mode==2 && !data;
            auto verify=[&](bool starts) {
                check(bool(peek(reg::SR)&reg::BUSY)==(starts&&!invalid_poll),"trigger-stage BUSY truth table");
                check(bool(peek(reg::SR)&reg::TEF)==(starts&&invalid_poll),"poll-without-data legality at trigger");
            };
            bool started=enabled && ad==0 && ((mode==0 && data==0) || mode==1 || mode==2);
            verify(started);
            if (!started) {
                wr(reg::AR,16);
                started=enabled && ad!=0 && ((mode==0 && data==0) || mode==1 || mode==2);
                verify(started);
            }
            if (!started) {
                wr(reg::DR,0x12345678);
                started=enabled && mode==0 && data!=0; verify(started);
            }
            if (started && !invalid_poll) {
                until([&]{return target.records.size()==1;},"trigger did not issue a frame");
                check(target.records.front().phase.instruction_lines==im,"instruction phase independent of trigger");
                check(target.records.front().phase.address_lines==(ad?(1u<<(ad-1)):0),"address mode reaches backend");
                if (mode==1 && data) rd(reg::DR);
                tick(3); check(target.records.size()==1,"one trigger issues exactly one frame");
            } else {
                tick(100); check(target.records.empty(),"disabled/mapped/nontrigger writes never issue commands");
            }
        }
}

void Bench::dlr_boundaries() {
    // R 1.5 and 1.2: exact finite/undefined lengths, no 32-bit arithmetic wrap.
    for (unsigned n:{1u,2u,3u,4u,5u,31u,32u,33u}) {
        reset(); setup(); read_start(128,n-1);
        std::vector<unsigned char> expected(n); for (unsigned i=0;i<n;++i) expected[i]=Target::pattern(128+i);
        drain(expected); audit(n,128,tlm::TLM_READ_COMMAND);
    }
    for (auto dlr:{0xfffffffeu,0xffffffffu}) {
        reset(); setup(); read_start(0,dlr);
        if (dlr==0xfffffffeu) check((peek(reg::SR)&reg::TEF) && target.records.empty(),"finite huge DLR rejected without allocation");
        else {
            until([&]{return !target.records.empty();},"undefined DLR started");
            check(target.records[0].phase.length==0x20000,"undefined length follows FSIZE"); abort();
        }
    }
    reset(); setup(0,0); read_start(0,1);
    drain({Target::pattern(0),Target::pattern(1)}); // Minimum FSIZE is two bytes.
    wr(reg::FCR,63); wr(reg::DLR,2); wr(reg::AR,0);
    check(peek(reg::SR)&reg::TEF,"one byte beyond smallest FSIZE");
    for (auto dlr:{3u,4u}) {
        reset(); setup(0,31); wr(reg::DLR,dlr); wr(reg::CCR,command(1)|(3u<<12)); wr(reg::AR,0xfffffffc);
        if (dlr==4) check((peek(reg::SR)&reg::TEF) && target.records.empty(),"4-GiB FSIZE boundary rejects overflow");
        else {
            std::vector<unsigned char> expected(4); for(unsigned i=0;i<4;++i) expected[i]=Target::pattern(0xfffffffcULL+i);
            drain(expected); audit(4,0xfffffffc,tlm::TLM_READ_COMMAND);
        }
    }
    for (auto dlr:{0xfffffffeu,0xffffffffu}) {
        reset(); setup(0,31); wr(reg::DLR,dlr); wr(reg::CCR,command(1)|(3u<<12)); wr(reg::AR,0);
        until([&]{return !target.records.empty();},"large valid length starts bounded");
        check(target.records[0].phase.length==(dlr==0xffffffffu?0x100000000ULL:0xffffffffULL),"DLR length retained in 64 bits");
        abort(); check(target.records.size()==2 && target.records.back().phase.cancel,"large streaming frame cancels cleanly");
    }
}

void Bench::fifo_packing() {
    for (unsigned n:{1u,2u,3u,4u,5u,7u,31u,32u,33u,63u,64u,65u}) {
        reset(); setup(); std::vector<unsigned char> bytes(n);
        for(unsigned i=0;i<n;++i) bytes[i]=static_cast<unsigned char>(i*13+7);
        wr(reg::DLR,n-1); wr(reg::CCR,command(0,1,1,2)); wr(reg::AR,256);
        for(unsigned i=0;i<n;i+=4) {
            const auto count=std::min(4u,n-i);
            until([&]{return ((peek(reg::SR)>>8)&63)<=32-count;},"TX space");
            wr(reg::DR,word(bytes,i,count));
        }
        done(); audit(n,256,tlm::TLM_WRITE_COMMAND);
        for(unsigned i=0;i<n;++i) check(target.written.at(256+i)==bytes[i],"TX bytes independently match host buffer");
        check(target.written.count(256+n)==0,"no high-lane bytes beyond DLR");
        target.records.clear(); read_start(256,n-1); drain(bytes); audit(n,256,tlm::TLM_READ_COMMAND);
    }
    reset(); setup(); read_start(0,32); until([&]{return ((peek(reg::SR)>>16)&63)==32;},"RX full");
    check(target.records.size()==8,"exact 32-byte RX capacity"); tick(200);
    check(target.records.size()==8 && !(peek(reg::SR)&reg::TCF),"33rd byte stalls without drop or early TCF");
    std::vector<unsigned char> expected(33); for(unsigned i=0;i<33;++i) expected[i]=Target::pattern(i);
    drain(expected); audit(33,0,tlm::TLM_READ_COMMAND);
    reset(); setup(0,16,63); wr(reg::DLR,39); wr(reg::CCR,command(0,1,1,2)); wr(reg::AR,0);
    for(unsigned i=0;i<9;++i) wr(reg::DR,0x01010101u*i);
    check(((peek(reg::SR)>>8)&63)==32,"TX full with first word in flight");
    wr(reg::DR,0xdeadbeef); check(peek(reg::DMASR)&reg::TXOVRF,"full TX write flags overrun");
    until([&]{return ((peek(reg::SR)>>8)&63)<=28;},"TX resumes",100000);
    wr(reg::DR,0xfeedface); done();
    std::vector<unsigned char> sent;
    for(const auto& r:target.records) sent.insert(sent.end(),r.bytes.begin(),r.bytes.end());
    check(sent.size()==40 && word(sent,36,4)==0xfeedface,"overrun word discarded; retry keeps packing");
    for(unsigned i=0;i<9;++i) check(word(sent,4*i,4)==0x01010101u*i,"full FIFO ordering preserved");
}

void Bench::irq_w1c() {
    // R 1.3 threshold flags reassert if the condition remains after W1C.
    setup(reg::RXFTIE|reg::TCIE); read_start(0,3); done(); tick();
    check(irq.read() && (peek(reg::SR)&10)==10,"simultaneous RXFTF and TCF");
    wr(reg::FCR,0); check((peek(reg::SR)&10)==10 && irq.read(),"writing zero preserves flags");
    wr(reg::FCR,reg::TCF); check(!(peek(reg::SR)&reg::TCF) && irq.read(),"clearing TCF leaves threshold IRQ");
    wr(reg::FCR,reg::RXFTF); settle();
    check(!(peek(reg::SR)&reg::RXFTF) && !irq.read(),"RX threshold W1C observed before next edge");
    tick(); check((peek(reg::SR)&reg::RXFTF) && irq.read(),"RX condition reasserts on subsequent edge");
    wr(reg::CR,0x01000001); check(!irq.read() && (peek(reg::SR)&reg::RXFTF),"masking IRQ preserves flag");
    wr(reg::CR,0x01000001|reg::RXFTIE); check(irq.read(),"unmask pending threshold immediately");
    rd(reg::DR); wr(reg::FCR,reg::RXFTF); tick(3);
    check(!(peek(reg::SR)&reg::RXFTF) && !irq.read(),"draining condition prevents reassertion");
    wr(reg::DMACR,8); rd(reg::DR); check(irq.read() && peek(reg::DMASR)==8,"empty DR read sets underrun");
    wr(reg::FCR,63); check(peek(reg::DMASR)==8 && irq.read(),"FCR does not clear DMA flags");
    wr(reg::DMAFCR,0); check(peek(reg::DMASR)==8,"DMA W1C zero ignored");
    wr(reg::DMAFCR,8); tick(5); check(!irq.read() && peek(reg::DMASR)==0,"underrun does not recur without another DR read");
    rd(reg::DR); check(irq.read(),"new empty read produces a new underrun");
    reset(); setup(reg::TXFTIE|(31u<<8)); tick();
    check(irq.read() && (peek(reg::SR)&reg::TXFTF),"empty TX threshold 32 free bytes");
    wr(reg::FCR,reg::TXFTF); settle(); check(!irq.read(),"TX W1C lowers IRQ for cleared flag");
    tick(); check(irq.read(),"unchanged TX free-space condition reasserts");
    reset(); setup(reg::SMIE); target.statuses={0}; wr(reg::DLR,0); wr(reg::PSMKR,1); wr(reg::PSMAR,0); wr(reg::PIR,1000);
    wr(reg::CCR,command(2,0,1,5)); until([&]{return target.polls==1;},"first matching poll"); tick();
    check(irq.read(),"status match IRQ while APMS disabled");
    wr(reg::FCR,reg::SMF); tick(20);
    check(!irq.read() && !(peek(reg::SR)&reg::SMF) && target.polls==1,"SMF is an event, not an RX-level condition");
    until([&]{return target.polls==2;},"next matching poll"); tick(); check(irq.read(),"new poll can set SMF again"); abort();
}

void Bench::active_reset() {
    auto pulse_core=[&] {
        core_on=false; wait(core_period*2); rn=false; wait(1,SC_NS);
        for(unsigned off=0;off<64;off+=4) check(peek(off)==0,"asynchronous full register reset with stopped core");
        check(!irq.read() && !rxreq.read() && !txreq.read(),"reset clears outputs");
        rn=true; core_on=true; tick(3);
    };
    setup(); read_start(0,7);
    check((peek(reg::SR)&reg::BUSY) && target.records.empty(),"prefix phase active before first backend callback");
    pulse_core(); tick(200); check(target.records.empty(),"reset during serial prefix prevents any backend frame");
    // Mid-frame TX starved after its first chunk; no fabricated completion.
    setup(reg::TCIE); wr(reg::DLR,15); wr(reg::CCR,command(0,1,1,2)); wr(reg::AR,0); wr(reg::DR,0x12345678);
    until([&]{return target.records.size()==1;},"first TX chunk");
    check(peek(reg::SR)&reg::BUSY,"TX waiting for remaining data"); pulse_core();
    check(target.records.size()==2 && target.records.back().phase.cancel,"core reset closes open TX frame once");
    tick(300); check(target.records.size()==2 && peek(reg::SR)==0,"no late TX completion after reset");
    setup(); read_start(0,63); until([&]{return ((peek(reg::SR)>>16)&63)==32;},"RX-full reset stage");
    pulse_core(); check(target.records.back().phase.cancel,"RX-full frame cancelled");
    setup(); read_start(0,3); done(); check(peek(reg::SR)&reg::BUSY,"unread completed RX stage");
    pulse_core(); check(peek(reg::DR)==0 && peek(reg::SR)==0,"reset discards unread data and TCF");
    setup(reg::SMIE); wr(reg::DLR,0); wr(reg::PSMKR,1); wr(reg::PSMAR,1); wr(reg::PIR,1000);
    wr(reg::CCR,command(2,0,1,5)); until([&]{return target.polls==1;},"poll reset stage");
    pulse_core(); const auto old=target.polls; tick(1500); check(target.polls==old,"reset prevents next scheduled poll");
    // HRESET while a backend has returned annotated latency: caller must abort.
    reset(); setup(); wr(reg::CCR,command(3)); target.latency=sc_time(600,SC_NS);
    launch(0,true,false,0); until([&]{return target.records.size()==1;},"XIP backend delay stage");
    hrn=false; wait(1,SC_NS); hrn=true; join(0);
    check(jobs[0].reply.status==tlm::TLM_GENERIC_ERROR_RESPONSE,"HRESET releases active XIP caller");
    wait(1,SC_US); check(peek(reg::CR)==0x01000001 && !(peek(reg::SR)&(reg::BUSY|reg::TCF)),"isolated HRESET retains configuration without stale success (VP policy)");
    target.latency=SC_ZERO_TIME; check(access(true,false,4).status==tlm::TLM_OK_RESPONSE,"XIP recovery after HRESET");
    target.records.clear(); target.latency=sc_time(600,SC_NS);
    launch(0,true,false,8); until([&]{return target.records.size()==1;},"core reset during XIP backend latency");
    rn=false; wait(1,SC_NS); rn=true; join(0); wait(1,SC_US);
    check(jobs[0].reply.status==tlm::TLM_GENERIC_ERROR_RESPONSE && peek(reg::CR)==0 && peek(reg::SR)==0,
        "core reset cancels delayed XIP and prevents stale flags");
    target.latency=SC_ZERO_TIME; setup(); wr(reg::CCR,command(3));
    // PRESET pulse must invalidate both admitted and mutex-queued AP writes.
    core_on=false; wait(core_period*2);
    launch(0,false,true,reg::ABR,0x11223344); launch(1,false,true,reg::ABR,0xaabbccdd); wait(p_period*3);
    prn=false; wait(1,SC_NS); prn=true; core_on=true; join(0); join(1);
    check(jobs[0].reply.status==tlm::TLM_GENERIC_ERROR_RESPONSE && jobs[1].reply.status==tlm::TLM_GENERIC_ERROR_RESPONSE,"PRESET cancels active and queued AP callers");
    check(peek(reg::ABR)==0,"cancelled writes do not leak after reset pulse");
    launch(0,true,false,0,0,sc_time(100,SC_NS)); wait(20,SC_NS); rn=false; wait(1,SC_NS); rn=true; join(0);
    check(jobs[0].reply.status==tlm::TLM_GENERIC_ERROR_RESPONSE,"core reset invalidates caller still consuming incoming delay");
}

void Bench::bus_errors() {
    const auto before=peek(reg::DCR);
    for(bool mapped:{false,true}) {
        for(auto address:{1ULL,2ULL,3ULL,0x100000000ULL})
            check(access(mapped,false,address,0x5a5a5a5a).status==tlm::TLM_ADDRESS_ERROR_RESPONSE,"unaligned/high address error");
        for(unsigned length:{0u,1u,2u,3u,5u,8u})
            check(access(mapped,false,0,0,length,8).status==tlm::TLM_BURST_ERROR_RESPONSE,"non-word length error");
        for(unsigned width:{0u,1u,2u,3u})
            check(access(mapped,false,0,0,4,width).status==tlm::TLM_BURST_ERROR_RESPONSE,"streaming wrap rejected");
        unsigned char be[]={255,0,255,255};
        check(access(mapped,true,0,0xffffffff,4,4,be,4).status==tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE,"sparse lanes rejected");
        check(access(mapped,false,0,0,4,4,be,0).status==tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE,"zero-length byte enable rejected");
        check(access(mapped,false,0,0,4,4,nullptr,0,true).status==tlm::TLM_GENERIC_ERROR_RESPONSE,"null data response");
        check(access(mapped,false,0,0,4,4,nullptr,0,false,tlm::TLM_IGNORE_COMMAND).status==tlm::TLM_COMMAND_ERROR_RESPONSE,"invalid bus command response");
    }
    check(peek(reg::DCR)==before && target.records.empty(),"rejected traffic is side-effect free");
    check(access(false,true,0x40,0xffffffff).status==tlm::TLM_ADDRESS_ERROR_RESPONSE,"unmapped AP word");
    check(access(true,false,0x1ffff).status==tlm::TLM_ADDRESS_ERROR_RESPONSE,"aperture crossing rejected whole");
    setup(0,7); wr(reg::CCR,command(3));
    check(access(true,false,252).status==tlm::TLM_OK_RESPONSE,"last mapped word succeeds");
    const auto count=target.records.size();
    check(access(true,false,256).status==tlm::TLM_ADDRESS_ERROR_RESPONSE && target.records.size()==count,"FSIZE failure does not reach flash");
    for(auto response:{tlm::TLM_ADDRESS_ERROR_RESPONSE,tlm::TLM_COMMAND_ERROR_RESPONSE,tlm::TLM_BURST_ERROR_RESPONSE,tlm::TLM_GENERIC_ERROR_RESPONSE}) {
        target.response=response;
        check(access(true,false,0).status==tlm::TLM_GENERIC_ERROR_RESPONSE,"backend failure maps to functional AHB error");
        check(!(peek(reg::SR)&reg::TEF),"mapped failures do not set indirect TEF");
        target.response=tlm::TLM_OK_RESPONSE;
        check(access(true,false,4).status==tlm::TLM_OK_RESPONSE,"error releases admission for retry");
    }
    target.throws=true; check(access(true,false,0).status==tlm::TLM_GENERIC_ERROR_RESPONSE,"backend exception maps to AHB error");
    target.throws=false; target.response=tlm::TLM_ADDRESS_ERROR_RESPONSE; read_start(0,3);
    until([&]{return (peek(reg::SR)&reg::TEF)!=0;},"indirect backend error");
    check(!(peek(reg::SR)&(reg::BUSY|reg::TCF)),"failed indirect transfer has no false completion");
    target.response=tlm::TLM_OK_RESPONSE;
    read_start(0,3); drain({Target::pattern(0),Target::pattern(1),Target::pattern(2),Target::pattern(3)});
    // Illegal peripheral configuration is not an AP protocol error.
    wr(reg::FCR,63); wr(reg::CR,0); wr(reg::CR,1);
    check(access(false,true,reg::CCR,command(0,0,0,6)).status==tlm::TLM_OK_RESPONSE && (peek(reg::SR)&reg::TEF),"legal AP write with illegal divider sets TEF");
}

void Bench::polling_edges() {
    for(unsigned bytes=1;bytes<=4;++bytes) for(bool use_or:{false,true}) for(bool stop:{false,true}) {
        reset(); setup((use_or?reg::PMM:0)|(stop?reg::APMS:0));
        const std::uint32_t value=0x96814253;
        target.statuses={value}; wr(reg::DLR,bytes-1);
        const auto high_bit=1u<<(8*(bytes-1));
        wr(reg::PSMKR,high_bit); wr(reg::PSMAR,value&high_bit); wr(reg::PIR,1000);
        wr(reg::DMACR,3); wr(reg::CCR,command(2,0,1,5)); until([&]{return target.polls==1;},"poll-width completion"); tick();
        check((peek(reg::SR)&reg::SMF) && bool(peek(reg::SR)&reg::BUSY)==!stop,"AND/OR APMS matrix");
        const auto expected=bytes==4?value:value&((1u<<(8*bytes))-1);
        check(rd(reg::DR)==expected && rd(reg::DR)==expected,"last status zero-extends without consuming data");
        check(peek(reg::DMASR)==0 && !rxreq.read() && !txreq.read(),"poll DR bypasses RX FIFO and DMA");
        if(!stop) abort();
    }
    // Empty effective masks and match-vs-timeout priority are documented VP
    // policies, separately checked from explicit nonempty-mask FU3 clauses.
    for(bool use_or:{false,true}) for(bool beyond:{false,true}) {
        reset(); setup(reg::APMS|(use_or?reg::PMM:0));
        wr(reg::DLR,0); wr(reg::PSMKR,beyond?0xffffff00:0); wr(reg::PSMAR,0xffffffff); wr(reg::PIR,1000);
        wr(reg::CCR,command(2,0,1,5)); until([&]{return target.polls==1;},"empty effective mask"); tick();
        check(bool(peek(reg::SR)&reg::SMF)==!use_or && bool(peek(reg::SR)&reg::BUSY)==use_or,"VP empty AND/OR convention ignores bytes beyond DLR");
        if(use_or) abort();
    }
    for(unsigned timeout:{0u,1u,31u,32u}) for(bool stop:{false,true}) {
        reset(); setup(reg::PTCEN|(stop?reg::APMS:0)); target.statuses={1};
        wr(reg::DLR,0); wr(reg::PSMKR,1); wr(reg::PSMAR,1); wr(reg::PTR,timeout);
        wr(reg::CCR,command(2,0,1,5));
        until([&]{return !(peek(reg::SR)&reg::BUSY);},"simultaneous poll stop");
        check(target.polls==1 && (peek(reg::SR)&reg::SMF),"timeout does not truncate in-progress status read");
        check(bool(peek(reg::SR)&reg::STOF)==!stop,"VP APMS match priority over simultaneous timeout");
    }
    reset(); setup(); wr(reg::DLR,0); wr(reg::PSMKR,1); wr(reg::PSMAR,1); wr(reg::PIR,65535);
    wr(reg::CCR,command(2,0,1,5)); until([&]{return target.polls==1;},"maximum PIR first frame");
    wait(100,SC_US); check(target.polls==1 && (peek(reg::SR)&reg::BUSY),"maximum PIR gap retained without spurious reads"); abort();
}

void Bench::dma_edges() {
    for(unsigned threshold:{0u,1u,7u,8u,31u}) {
        reset(); setup(threshold<<8); wr(reg::DMACR,1); wr(reg::DLR,63); wr(reg::CCR,command(0,1,1,2)); wr(reg::AR,0); tick();
        check(txreq.read()==(threshold<=7),"TX DMA word threshold at/beyond 32-byte capacity");
        txclr=true; tick(3); check(!txreq.read(),"held TX clear suppresses request");
        txclr=false; tick(); check(txreq.read()==(threshold<=7),"TX request rechecks on next low clear");
        wr(reg::DMACR,0); tick(); check(!txreq.read(),"TX DMA disable without FIFO activity");
        reset(); setup(threshold<<3); wr(reg::DMACR,2); read_start(0,31); done(); tick();
        check(rxreq.read()==(threshold<=7),"RX DMA full-word threshold at/beyond capacity");
        rxclr=true; tick(4); check(!rxreq.read(),"held RX clear suppresses request");
        rxclr=false; tick(); check(rxreq.read()==(threshold<=7),"RX request can reassert without draining eligible FIFO");
        wr(reg::DMACR,0); tick(); check(!rxreq.read(),"RX DMA live disable while BUSY");
        wr(reg::DMACR,2); tick(); check(rxreq.read()==(threshold<=7),"RX DMA live reenable");
        abort(); tick(3); check(!rxreq.read() && !txreq.read(),"abort flushes and suppresses requests");
    }
    reset(); setup(1u<<3); wr(reg::DMACR,2); read_start(0,6); done(); tick();
    check(rxreq.read(),"VP seven-byte tail provides two readable DR words");
    rd(reg::DR); rxclr=true; tick(); rxclr=false; tick();
    check(!rxreq.read(),"three-byte remainder below two-word DMA threshold");
    const auto tail=rd(reg::DR); check((tail&0xff000000u)==0,"partial tail has zero high lane");
    reset(); setup(7u<<8); wr(reg::DMACR,1); wr(reg::DLR,0); wr(reg::CCR,command(0,1,1,2)); wr(reg::AR,0); tick();
    check(txreq.read(),"TX request follows explicit not-all-written condition even with one-byte DLR");
    wr(reg::DR,0x55); tick(); check(!txreq.read(),"all DLR bytes accepted drops request before serial completion"); done();
}

void Bench::domains() {
    setup(); wr(reg::CCR,command(3));
    launch(0,true,false,0); tick(3); launch(1,true,false,4);
    wr(reg::CR,0x01000001|reg::TCIE); check(peek(reg::CR)&reg::TCIE,"AP configuration-live field during XIP");
    join(0); join(1);
    check(jobs[0].reply.status==tlm::TLM_OK_RESPONSE && jobs[1].reply.status==tlm::TLM_OK_RESPONSE && jobs[0].ended<=jobs[1].ended,"one XIP outstanding, ordered completion");
    check(target.records.size()==2 && target.records[0].address==0 && target.records[1].address==4,"no duplicated or interleaved frames");
    // A gated unrelated bus domain does not stop the core/other interface.
    p_on=false; wait(p_period*2); launch(0,true,false,8); join(0);
    check(jobs[0].reply.status==tlm::TLM_OK_RESPONSE,"mapped path runs with PCLK stopped");
    p_on=true; wait(p_period*2); check(rd(reg::CCR)==command(3),"AP recovers when PCLK resumes");
    h_on=false; wait(h_period*2); read_start(16,3);
    drain({Target::pattern(16),Target::pattern(17),Target::pattern(18),Target::pattern(19)});
    h_on=true; wait(h_period*2);
    // Two AP callers timeout under a stopped core; neither may write later.
    core_on=false; wait(core_period*2);
    launch(0,false,true,reg::ABR,0x11223344); launch(1,false,true,reg::ABR,0xaabbccdd);
    wait(p_period*(config.bus_timeout_cycles*2+10));
    check(jobs[0].done && jobs[1].done && jobs[0].reply.status==tlm::TLM_GENERIC_ERROR_RESPONSE && jobs[1].reply.status==tlm::TLM_GENERIC_ERROR_RESPONSE,"stopped-core AP admission/timeouts release both callers");
    core_on=true; tick(5); check(peek(reg::ABR)==0,"timed-out queued writes never apply");
    wr(reg::ABR,0x55667788); check(rd(reg::ABR)==0x55667788,"AP path recovers after clock timeout");
}

void Bench::parameters() {
    const unsigned depth=config.rx_fifo_bytes;
    setup();
    const unsigned length=depth*2+3;
    std::vector<unsigned char> expected(length);
    for(unsigned i=0;i<length;++i) expected[i]=Target::pattern(64+i);
    read_start(64,length-1); drain(expected); audit(length,64,tlm::TLM_READ_COMMAND);
    // Poll at/beyond the capacity boundary with an independently chosen length.
    wr(reg::FCR,63); wr(reg::CR,0x01000001|((depth-1)<<3)); read_start(0,depth-1); done(); tick();
    check(((peek(reg::SR)>>16)&63)==depth && (peek(reg::SR)&reg::RXFTF),"configured RX capacity/byte threshold");
    abort(); wr(reg::FCR,63); wr(reg::DLR,std::min(3u,config.tx_fifo_bytes-1)); wr(reg::CCR,command(0,1,1,2)); wr(reg::AR,512);
    wr(reg::DR,0x78563412); done();
    const auto valid=std::min(4u,config.tx_fifo_bytes);
    for(unsigned i=0;i<valid;++i) check(target.written.at(512+i)==((0x78563412u>>(8*i))&255),"configured TX tail fits capacity");
    if(config.tx_fifo_bytes<4) {
        wr(reg::FCR,63); wr(reg::DLR,3); wr(reg::AR,768); wr(reg::DR,0xffffffff);
        check((peek(reg::DMASR)&reg::TXOVRF) && ((peek(reg::SR)>>8)&63)==0,"capacity below word rejects atomic TX write without partial enqueue"); abort();
    }
    wr(reg::CCR,command(3)); const auto mapped=access(true,false,0);
    check(mapped.status==tlm::TLM_OK_RESPONSE,"XIP remains word-wide at all FIFO depths and clock ratios");
    core_on=false; wait(core_period*2); const auto start=sc_time_stamp();
    check(access(false,true,reg::ABR,123).status==tlm::TLM_GENERIC_ERROR_RESPONSE,"parameterized response timeout");
    check(sc_time_stamp()-start>=p_period*config.bus_timeout_cycles,"timeout uses actual selected AP clock");
    core_on=true; tick(2); check(peek(reg::ABR)==0,"parameterized timeout removes pending write");
    if (depth==32 && config.tx_fifo_bytes==32) {
        check(Config{}.bus_timeout_cycles==5000,"W table 2-1 default bus timeout");
        for(unsigned invalid:{0u,33u}) {
            bool tx_rejected=false,rx_rejected=false;
            try { QspiTxRxFifo invalid_fifo(invalid,32); } catch(const std::invalid_argument&) { tx_rejected=true; }
            try { QspiTxRxFifo invalid_fifo(32,invalid); } catch(const std::invalid_argument&) { rx_rejected=true; }
            check(tx_rejected && rx_rejected,"FIFO capacity guard outside supported 1..32 range");
        }
    }
}

void Bench::run() {
    try {
        reset();
        if (scenario=="p0_triggers") trigger_combinations();
        else if (scenario=="p0_dlr") dlr_boundaries();
        else if (scenario=="p0_fifo") fifo_packing();
        else if (scenario=="p0_irq_w1c") irq_w1c();
        else if (scenario=="p0_active_reset") active_reset();
        else if (scenario=="p0_bus_errors") bus_errors();
        else if (scenario=="p1_polling") polling_edges();
        else if (scenario=="p1_dma") dma_edges();
        else if (scenario=="p1_domains") domains();
        else if (scenario.rfind("p1_parameters_",0)==0) parameters();
        else check(false,"unknown scenario");
        passed=true; std::cout<<"PASS "<<scenario<<" assertions="<<assertions<<" time="<<sc_time_stamp()<<'\n';
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<scenario<<": "<<e.what()<<'\n'; }
    sc_stop();
}
}
int sc_main(int argc,char** argv) {
    const std::string scenario=argc>1?argv[1]:"p0_triggers";
    const auto depth=argc>2?static_cast<unsigned>(std::stoul(argv[2])):32u;
    const auto rx_depth=argc>3?static_cast<unsigned>(std::stoul(argv[3])):depth;
    Bench bench{"bench",scenario,depth,rx_depth}; sc_start(sc_time(100,SC_MS)); return bench.passed?0:1;
}
