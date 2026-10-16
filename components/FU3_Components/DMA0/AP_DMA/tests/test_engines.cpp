#include "regression_bench.h"

struct Advanced : Regression {
    struct Observation { unsigned channel; bool read, descriptor; std::uint64_t address; sc_core::sc_time at; };
    std::vector<Observation> observations;
    unsigned live_read=0,live_write=0,max_read=0,max_write=0;
    bool overlap=false, full_overlap=false, variable=false, slow_read=false, slow_write=false;
    bool fail_descriptor=false, drain_fault=false, drain_read=true;
    sc_core::sc_time target_delay=sc_core::sc_time(20,sc_core::SC_NS);
    Advanced(sc_core::sc_module_name name,std::string mode,const fu3::dma::EngineConfig& config)
        : Regression(name,std::move(mode),61443,config) {}
    void transport(tlm::tlm_generic_payload& t,sc_core::sc_time& delay) override {
        const auto* e=t.get_extension<fu3::dma::TransferExtension>();
        check(e!=nullptr,"engine metadata");
        observations.push_back({e?e->channel:0,t.is_read(),e&&e->command_fetch,t.get_address(),sc_core::sc_time_stamp()});
        auto& live=t.is_read()?live_read:live_write;
        ++live; max_read=std::max(max_read,live_read); max_write=std::max(max_write,live_write);
        overlap=overlap || (live_read && live_write);
        full_overlap=full_overlap || (live_read==4 && live_write==4);
        unsigned total_r=0,total_w=0;
        for(unsigned n=0;n<8;++n) { auto p=reg(false,n*256+0x3c); total_r+=p&15; total_w+=(p>>8)&15; }
        check(total_r<=4 && total_w<=4,"global pending depths");
        auto duration=target_delay;
        if(variable && t.is_read()) duration=sc_core::sc_time(t.get_address()==0x1000?50:5,sc_core::SC_NS);
        if((slow_read&&t.is_read()) || (slow_write&&t.is_write())) duration=sc_core::sc_time(100,sc_core::SC_NS);
        const bool selected=drain_fault && t.is_read()==drain_read;
        const bool first=selected && t.get_address()==(drain_read?0x1000u:0x8000u);
        if(selected) duration=sc_core::sc_time(first?5:60,sc_core::SC_NS);
        // Validate architecture at acceptance, before a possible reset during wait.
        if(first) t.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        else if(fail_descriptor && e && e->command_fetch) t.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        else Regression::transport(t,delay);
        if(duration!=sc_core::SC_ZERO_TIME) wait(duration);
        --live;
    }
    void pending_config(unsigned n,unsigned rlimit,unsigned wlimit,bool renable=true,bool wenable=true) {
        reg(true,n*256+16,0x80010004|(rlimit<<24)|(renable?0x40000000:0));
        reg(true,n*256+20,0x80010004|(wlimit<<24)|(wenable?0x40000000:0));
    }
    void fill(unsigned src,unsigned count) { for(unsigned i=0;i<count;++i) memory[src+i]=(i*37+9)&255; }
    void check_copy(unsigned src,unsigned dst,unsigned count) {
        check(std::equal(memory.begin()+src,memory.begin()+src+count,memory.begin()+dst),"advanced copy bytes");
    }
    void pending() {
        fill(0x1000,256); configure(0,0x1000,0x8000,256); pending_config(0,4,4);
        reg(true,0x44,1); done(0);
        check(max_read==4 && max_write==4,"achieved four read AND four write contexts");
        check(overlap,"read and write overlap"); check_copy(0x1000,0x8000,256);
        check(reg(false,0x3c)==0 && reg(false,0x38)==32,"all reservations drained");
        // Global 4-context budget shared by eight channels, never 4 per channel globally.
        reset(); max_read=max_write=0;
        for(unsigned n=0;n<8;++n) { fill(0x1000+n*256,64); configure(n,0x1000+n*256,0x8000+n*256,64); pending_config(n,4,4); }
        reg(true,0x1048,255); for(unsigned n=0;n<8;++n) done(n);
        check(max_read==4 && max_write==4 && full_overlap,"four read and four write contexts overlap across channels");
        for(unsigned n=0;n<8;++n) check_copy(0x1000+n*256,0x8000+n*256,64);
    }
    void limits() {
        for(unsigned r:{0u,1u,2u,3u,4u,15u}) for(unsigned w:{0u,1u,2u,3u,4u,15u}) {
            reset(); max_read=max_write=0; fill(0x1000,128); configure(0,0x1000,0x8000,128); pending_config(0,r,w);
            reg(true,0x44,1); done(0);
            check(max_read<=std::clamp(r,1u,4u) && max_write<=std::clamp(w,1u,4u),"configured clamps");
            check_copy(0x1000,0x8000,128);
        }
        for(bool read_enable:{false,true}) for(bool write_enable:{false,true}) {
            reset(); max_read=max_write=0; configure(0,0x1000,0x8000,128); pending_config(0,4,4,read_enable,write_enable);
            reg(true,0x44,1); done(0);
            check(max_read<=(read_enable?4u:1u) && max_write<=(write_enable?4u:1u),"independent endpoint enable");
        }
        // Peripheral read remains single-credit even when OUTS_MAX=4 enabled.
        reset(); max_read=0; received.clear(); configure(0,0x1e000,0x8000,8);
        reg(true,16,0x44010004); reg(true,20,0xc4010004); reg(true,32,2); reg(true,68,1);
        for(unsigned i=0;i<2;++i) {
            reg(true,0x1050,4); unsigned k=0;
            while(!(rc.read().to_uint()&4) && k++<100) wait(1,sc_core::SC_NS);
            check(k<100,"peripheral read credit returns"); wait(11,sc_core::SC_NS);
        }
        done(0); check(max_read==1,"peripheral cannot multi-pend");
    }
    void ordered() {
        variable=true; fill(0x1000,64); configure(0,0x1000,0x8000,64); pending_config(0,4,4);
        reg(true,68,1); done(0); check_copy(0x1000,0x8000,64);
        auto first=std::find_if(observations.begin(),observations.end(),[](const auto& o){return !o.read;});
        check(first!=observations.end() && first->at>=sc_core::sc_time(71,sc_core::SC_NS),"same-ID in-order retirement holds younger read returns");
    }
    void arbitration() {
        for(unsigned n=0;n<2;++n) { fill(0x1000+n*256,128); configure(n,0x1000+n*256,0x8000+n*256,128); pending_config(n,4,4); }
        reg(true,16,0xc4030004); // Channel0 read token budget 3.
        reg(true,0x1048,3); done(0); done(1);
        std::vector<unsigned> grants;
        for(const auto& o:observations) if(o.read) grants.push_back(o.channel);
        check(grants.size()>=4 && grants[0]==0 && grants[1]==0 && grants[2]==0 && grants[3]==1,"read WRR 3:1 first grant group");
        check_copy(0x1000,0x8000,128); check_copy(0x1100,0x8100,128);
        // Both read bursts fill a FIFO simultaneously, so both write contenders
        // are eligible when the independent write arbiter grants its first slots.
        reset(); observations.clear();
        for(unsigned n=0;n<2;++n) { configure(n,0x1000+n*256,0x8000+n*256,128); pending_config(n,4,4); }
        reg(true,16,0x81010020); reg(true,0x110,0x81010020);
        reg(true,20,0xc4030004);
        reg(true,0x1048,3); done(0); done(1);
        grants.clear();
        for(const auto& o:observations) if(!o.read) grants.push_back(o.channel);
        check(grants.size()>=4 && grants[0]==0 && grants[1]==0 && grants[2]==0 && grants[3]==1,"write WRR 3:1 independent grant group");
        // Idle waiting must be event-driven rather than cycle polling.
        reset(); configure(0,0x1e000,0x8000,1); reg(true,32,2); reg(true,16,0x01010001); reg(true,68,1);
        wait(1,sc_core::SC_US); auto before=dma.scheduler_wakeups(); wait(100,sc_core::SC_US);
        check(dma.scheduler_wakeups()-before<4,"scheduler sleeps while no request");
    }
    void timeout() {
        for(bool read:{true,false}) {
            reset(); slow_read=read; slow_write=!read; fill(0x1000,64);
            configure(0,0x1000,0x8000,64); pending_config(0,4,4); reg(true,68,1);
            // Wait for acceptance: a positive scheduler quantum can defer writes.
            unsigned accepted=0;
            while(!(reg(false,60)&(read?15u:0xf00u)) && accepted++<500) wait(1,sc_core::SC_NS);
            check(accepted<500,"timeout phase reaches an accepted transaction");
            const auto until=sc_core::sc_time_stamp()+sc_core::sc_time(40,sc_core::SC_NS);
            while(sc_core::sc_time_stamp()<until && !reg(false,160)) wait(1,sc_core::SC_NS);
            check(reg(false,160)==(1u<<(read?7:9)),"read/write timeout IRQ before target returns");
            check(reg(false,60)!=0,"timeout does not free accepted contexts prematurely");
            const auto count=observations.size(); wait(10,sc_core::SC_NS);
            check(observations.size()==count,"fault blocks new grants");
            done(0); check(reg(false,80)==0,"timeout does not complete command");
            slow_read=slow_write=false; configure(0,0x1000,0x8000,16); reg(true,68,1); done(0);
            check(reg(false,160)==1,"retry after timeout drain");
        }
    }
    void timer_boundary() {
        // Completion exactly at threshold must win regardless of process order.
        target_delay=sc_core::sc_time(30,sc_core::SC_NS); fill(0x1000,64);
        configure(0,0x1000,0x8000,64); pending_config(0,4,4); reg(true,68,1); done(0);
        check(reg(false,160)==1,"threshold equality is successful completion");
        check_copy(0x1000,0x8000,64);
    }
    void watchdog() {
        configure(0,0x1e000,0x8000,1); reg(true,16,0x01010001); reg(true,32,2); reg(true,68,1);
        wait(35,sc_core::SC_NS); settle();
        check(reg(false,160)==(1u<<12) && reg(false,72)==0,"watchdog waiting on request");
        reset(); configure(0,0x1e000,0x8000,1); reg(true,16,0x01010001); reg(true,32,2); reg(true,68,1);
        reg(true,64,0); wait(100,sc_core::SC_NS);
        check(reg(false,160)==0,"watchdog paused while disabled");
        reg(true,64,1); wait(20,sc_core::SC_NS); check(reg(false,160)==0,"watchdog fresh interval on resume");
        wait(15,sc_core::SC_NS); check(reg(false,160)==(1u<<12),"watchdog expires after resume");
        reset(); target_delay=sc_core::sc_time(5,sc_core::SC_NS);
        fill(0x1000,256); configure(0,0x1000,0x8000,256); pending_config(0,4,4); reg(true,68,1); done(0);
        check(reg(false,160)==1,"regular progress resets watchdog");
    }
    void word(unsigned a,unsigned value) { for(unsigned i=0;i<4;++i) memory[a+i]=(value>>(8*i))&255; }
    void descriptor(unsigned at,unsigned src,unsigned dst,unsigned size,unsigned control) {
        word(at,src); word(at+4,dst); word(at+8,size); word(at+12,control);
    }
    void chain() {
        fill(0x1000,128); fill(0x1100,128); fill(0x1200,128);
        descriptor(0x3ff8,0x1100,0x8100,64,0x5000|1);
        descriptor(0x5000,0x1200,0x8200,32,3);
        configure(0,0x1000,0x8000,128,0,4,4,0x3ff8|1); pending_config(0,4,4);
        reg(true,68,1); done(0);
        check_copy(0x1000,0x8000,128); check_copy(0x1100,0x8100,64); check_copy(0x1200,0x8200,32);
        check(reg(false,80)==0x30003,"three commands and queued events");
        unsigned fetches=0;
        for(const auto& o:observations) if(o.descriptor) ++fetches;
        check(fetches==3,"descriptor crossing 4 KiB splits into two chunks");
        for(unsigned remaining=2;;--remaining) {
            reg(true,164,1); settle(); check(((reg(false,80)>>16)&15)==remaining,"one W1C dequeues one event");
            check(irq.read()==(remaining!=0),"IRQ held until completion queue empty"); if(!remaining) break;
        }
        // Fetch error and malformed command terminate without fake completion.
        reset(); configure(0,0x1000,0x8000,8,0,4,4,0x5000|1); fail_descriptor=true;
        reg(true,68,1); done(0); fail_descriptor=false;
        check((reg(false,160)&8)!=0 && (reg(false,80)&0xfff)==1,"descriptor DECERR preserves first completion");
        reset(); descriptor(0x5000,0x1100,0x8100,0,3);
        configure(0,0x1000,0x8000,8,0,4,4,0x5000|1); reg(true,68,1); done(0);
        check((reg(false,160)&8)!=0,"malformed zero-length descriptor rejected");
        // Queue saturation is explicit: completed counter continues, event count saturates at 15.
        reset(); target_delay=sc_core::SC_ZERO_TIME;
        for(unsigned i=0;i<20;++i) descriptor(0x6000+i*16,0x1000,0x9000+i*4,4,i==19?3:((0x6000+(i+1)*16)|1));
        configure(0,0x1000,0x8ffc,4,0,4,4,0x6000|1); reg(true,68,1); done(0);
        check(reg(false,80)==(21|(15u<<16)),"chain counters and event saturation");
    }
    void fault_drain() {
        for(bool read:{true,false}) {
            reset(); fill(0x1000,256); configure(0,0x1000,0x8000,256); pending_config(0,4,4);
            drain_fault=true; drain_read=read; reg(true,68,1);
            unsigned i=0;
            while(!reg(false,160) && i++<200) wait(1,sc_core::SC_NS);
            check(i<200 && reg(false,160)==(read?8u:16u),"multi-pending error IRQ");
            check(reg(false,60)!=0,"accepted contexts retained while fault drains");
            const auto issued=observations.size(); wait(10,sc_core::SC_NS);
            check(observations.size()==issued,"fault stops both dispatch paths");
            done(0); check(reg(false,60)==0 && reg(false,80)==0,"fault drain completes without success event");
            drain_fault=false;
            configure(0,0x1000,0x8000,64); reg(true,68,1); done(0);
            check_copy(0x1000,0x8000,64);
        }
    }
    void reset_pending() {
        slow_read=true; configure(0,0x1000,0x8000,128); pending_config(0,4,4); reg(true,68,1);
        wait(5,sc_core::SC_NS); reset();
        check(reg(false,60)==0 && !irq.read(),"reset clears architectural pending");
        slow_read=false; configure(0,0x1100,0x8100,64); pending_config(0,4,4); reg(true,68,1); done(0);
        check(reg(false,160)==1,"new epoch waits for physical slots and completes");
        check_copy(0x1100,0x8100,64);
    }
    void run() override {
        latency=0; reset();
        if(scenario=="pending") pending();
        else if(scenario=="limits") limits();
        else if(scenario=="ordered") ordered();
        else if(scenario=="arbitration") arbitration();
        else if(scenario=="timeout") timeout();
        else if(scenario=="watchdog") watchdog();
        else if(scenario=="timer_boundary") timer_boundary();
        else if(scenario=="chain") chain();
        else if(scenario=="reset_pending") reset_pending();
        else if(scenario=="fault_drain") fault_drain();
        else check(false,"unknown advanced scenario");
        std::cout << scenario << ": checks=" << checks << " max_read=" << max_read << " max_write=" << max_write
                  << " overlap=" << overlap << " errors=" << errors << '\n';
        sc_core::sc_stop();
    }
};
int sc_main(int argc,char** argv) {
    if(argc!=2) return 2;
    std::string mode=argv[1]; fu3::dma::EngineConfig config;
    if(mode=="timeout" || mode=="timer_boundary") config.transaction_timeout=sc_core::sc_time(30,sc_core::SC_NS);
    if(mode=="watchdog" || mode=="timer_boundary") config.watchdog_timeout=sc_core::sc_time(30,sc_core::SC_NS);
    if(mode=="pending") config.scheduler_quantum=sc_core::sc_time(1,sc_core::SC_NS);
    if(mode=="timeout") config.scheduler_quantum=sc_core::sc_time(100,sc_core::SC_NS);
    Advanced bench{"bench",mode,config}; sc_core::sc_start(); return bench.errors?1:0;
}
