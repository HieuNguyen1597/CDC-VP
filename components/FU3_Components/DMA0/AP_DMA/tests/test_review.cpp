#include "regression_bench.h"
#include <memory>

struct Review : Regression {
    struct Seen { unsigned channel,cache,prot; bool read,descriptor; unsigned address; };
    std::vector<Seen> seen;
    bool inspect_attributes=false, wrong_id=false, slow_channel_zero=false;
    bool reverse_writes=false, simulate_phases=false, stall_phase=false;
    fu3::dma::BusPhase selected_phase=fu3::dma::BusPhase::ReadAddress;
    std::uint32_t fault=0;
    bool fault_read=true;
    unsigned per_channel_reads[8]{}, per_channel_writes[8]{};
    unsigned tx_live=0,tx_peak=0,slow_duration_ns=60;
    std::vector<unsigned> read_grants,write_grants;
    Review(sc_core::sc_module_name name,std::string scenario,const fu3::dma::EngineConfig& config)
        : Regression(name,std::move(scenario),0xF003,config) {}
    void transport(tlm::tlm_generic_payload& t,sc_core::sc_time& delay) override {
        auto* e=t.get_extension<fu3::dma::TransferExtension>();
        check(e!=nullptr,"metadata exists"); if(!e) return;
        seen.push_back({e->channel,e->cache,e->prot,t.is_read(),e->command_fetch,unsigned(t.get_address())});
        (t.is_read()?read_grants:write_grants).push_back(e->channel);
        ++(t.is_read()?per_channel_reads[e->channel]:per_channel_writes[e->channel]);
        if(t.is_write()) {
            check(e->write_strobes.size()==e->beats,"one strobe per beat");
            for(unsigned beat=0;beat<e->beats;++beat) {
                const auto lane=(t.get_address()+(e->increment?beat*e->unit_bytes:0))&3;
                static constexpr unsigned masks[3][4]={{1,2,4,8},{3,0,12,0},{15,0,0,0}};
                const unsigned row=e->unit_bytes==1?0:e->unit_bytes==2?1:2;
                check(e->write_strobes[beat]==masks[row][lane],"WSTRB matches independent byte-lane truth table");
            }
        }
        if(e->command_fetch && inspect_attributes) check(e->cache==3 && e->prot==5,"fetch uses memory defaults, not payload override");
        if(wrong_id && t.is_read()==fault_read) { e->response_id_valid=true; e->response_id=7; }
        if(t.is_read()==fault_read) e->faults=fault;
        if(simulate_phases) {
            const std::vector<fu3::dma::BusPhase> phases=t.is_read()?
                std::vector<fu3::dma::BusPhase>{fu3::dma::BusPhase::ReadAddress,fu3::dma::BusPhase::ReadData}:
                std::vector<fu3::dma::BusPhase>{fu3::dma::BusPhase::WriteAddress,fu3::dma::BusPhase::WriteData,fu3::dma::BusPhase::WriteResponse};
            for(auto phase:phases) {
                e->enter_phase(phase);
                wait(stall_phase && phase==selected_phase?sc_core::sc_time(50,sc_core::SC_NS):sc_core::sc_time(2,sc_core::SC_NS));
            }
        }
        bool peripheral_write=t.is_write() && t.get_address()==0x1f000;
        if(peripheral_write) { ++tx_live; tx_peak=std::max(tx_peak,tx_live); }
        Regression::transport(t,delay);
        if(reverse_writes && t.is_write()) wait((t.get_address()&31)?3:30,sc_core::SC_NS);
        if(slow_channel_zero && e->channel==0 && t.is_read()) wait(slow_duration_ns,sc_core::SC_NS);
        if(peripheral_write) --tx_live;
    }
    void word(unsigned at,unsigned value) { for(unsigned i=0;i<4;++i) memory[at+i]=(value>>(8*i))&255; }
    void descriptor(unsigned at,unsigned src,unsigned dst,unsigned size,unsigned ctrl) {
        word(at,src);word(at+4,dst);word(at+8,size);word(at+12,ctrl);
    }
    void fill(unsigned a,unsigned size) { for(unsigned i=0;i<size;++i) memory[a+i]=(i*31+17)&255; }
    void pending(unsigned n,unsigned r=4,unsigned w=4,unsigned tokens=1) {
        reg(true,n*256+16,0xc0000004|(tokens<<16)|(r<<24));
        reg(true,n*256+20,0xc0000004|(tokens<<16)|(w<<24));
    }
    void capabilities() {
        check(reg(false,0x10e0)==0x101,"integration design capability");
        check(reg(false,0x10f0)==0x0a602258,"capability0");
        check(reg(false,0x10f4)==0xf17,"all implemented optional feature bits");
        check(reg(false,0x2c)==0,"restriction reset");
        configure(0,0x1000,0x8000,128); pending(0);
        check(reg(false,0x2c)==0x167,"aligned M2M restriction");
        reg(true,0x20,1u<<16);
        check(reg(false,0x2c)==0x27,"M2P restricts write pending independently");
        reg(true,0x20,2); check(reg(false,0x2c)==0x47,"P2M restricts read pending independently");
        reg(true,0,0x1001); reg(true,4,0x8002); reg(true,0x20,0);
        check(reg(false,0x2c)==0x160,"unaligned addresses do not permit full-FIFO burst");
        for(auto a:{0x10e0u,0x10f0u,0x10f4u,0x2cu}) reg(true,a,0,tlm::TLM_COMMAND_ERROR_RESPONSE);
        // All IRQ sources can be injected and masked without fabricating a command.
        for(unsigned n=0;n<8;++n) for(unsigned bit=0;bit<13;++bit) {
            unsigned b=n*256; reg(true,b+0xa4,0x1fff);reg(true,b+0xa8,1u<<bit);reg(true,b+0xa0,1u<<bit);settle();
            check(reg(false,b+0xac)==(1u<<bit) && irq.read(),"source injection IRQ");
            check(reg(false,0x1000)==(1u<<n),"injected channel IRQ bitmap");
            check(reg(false,b+0x50)==0,"raw injection is not a completed command");
            reg(true,b+0xa4,1u<<bit);settle();check(!irq.read(),"W1C injected IRQ");
        }
        reg(true,0xa0,0xffffffff); check(reg(false,0xa0)==0x1fff,"raw status reserved mask");
        reg(true,0xa0,0);check(reg(false,0xa0)==0x1fff,"raw writes are W1S");
    }
    void attributes() {
        inspect_attributes=true;
        fill(0x1000,64);
        for(unsigned pattern:{0u,0xbfbfu,0x0080u,0x8000u,0x8da5u}) {
            configure(0,0x1000,0x8001,64); reg(true,0x24,pattern); seen.clear();reg(true,68,1);done(0);
            for(auto o:seen) {
                const bool override=(pattern&(o.read?0x80:0x8000))!=0;
                const auto expected_cache=override?((pattern>>(o.read?0:8))&15):(o.read?3u:6u);
                const auto expected_prot=override?((pattern>>(o.read?4:12))&3):(o.read?5u:2u);
                check(o.cache==expected_cache && o.prot==expected_prot,"memory CACHE/PROT default/override");
            }
        }
        // Peripheral endpoint defaults and metadata on both directions.
        for(bool read:{false,true}) {
            configure(0,read?0x1e000:0x1000,read?0x8000:0x1f000,4);
            reg(true,0x24,0); // Static attributes persist across starts; explicitly select defaults.
            reg(true,16,read?0x01010004:0x81010008);reg(true,20,read?0x81010008:0x01010004);
            reg(true,32,read?2:(1u<<16));seen.clear();reg(true,68,1);reg(true,read?0x1050:0x1054,read?4:2);done(0);
            for(auto o:seen) if((read && o.read)||(!read&&!o.read))
                check(o.cache==(read?9u:12u) && o.prot==(read?1u:3u),"peripheral attribute defaults");
            wait(11,sc_core::SC_NS);
        }
        // Payload override must not leak to command fetch metadata.
        descriptor(0x4000,0x1000,0x8100,8,3);
        configure(0,0x1000,0x8000,8,0,8,8,0x4001);reg(true,0x24,0xbfbf);reg(true,68,1);done(0);
        std::unique_ptr<fu3::dma::TransferExtension> clone;
        fu3::dma::TransferExtension meta; sc_core::sc_event event;
        meta.phase_changed=&event;meta.cache=11;clone.reset(static_cast<fu3::dma::TransferExtension*>(meta.clone()));
        auto* copy=static_cast<fu3::dma::TransferExtension*>(clone.get());
        check(copy->cache==11 && !copy->phase_changed,"metadata clone has no dangling engine callback");
    }
    void protocol_faults() {
        for(bool read:{false,true}) {
            for(unsigned variant=0;variant<7;++variant) {
                reset();wrong_id=variant==0;fault_read=read;
                fault=variant==1?fu3::dma::FifoOverflow:variant==2?fu3::dma::FifoUnderflow:
                    variant==3?fu3::dma::ReadLastError:variant==4?fu3::dma::WriteCreditError:
                    variant==5?fu3::dma::WriteStrobeError:variant==6?fu3::dma::MultipleFifoReturns:0;
                fill(0x1000,64);configure(0,0x1000,0x8000,64);pending(0);reg(true,68,1);done(0);
                const auto expected=variant==1?32u:variant==2?64u:read?8u:16u;
                check(reg(false,160)==expected,"exact protocol/FIFO interrupt mapping");
                check(reg(false,80)==0 && reg(false,60)==0,"fault drains without completion");
                wrong_id=false;fault=0;configure(0,0x1000,0x8000,16);reg(true,68,1);done(0);
                check(reg(false,160)==1,"retry after injected adapter fault");
            }
        }
    }
    void phase_timers() {
        simulate_phases=true;
        const unsigned bits[]={8,7,11,10,9};
        for(unsigned phase=0;phase<5;++phase) {
            reset();selected_phase=static_cast<fu3::dma::BusPhase>(phase);stall_phase=true;
            configure(0,0x1000,0x8000,16);reg(true,68,1);
            unsigned k=0;while(!reg(false,160) && k++<100) wait(1,sc_core::SC_NS);
            check(k<100 && reg(false,160)==(1u<<bits[phase]),"phase-specific timeout before target returns");
            check(reg(false,60)!=0,"phase timeout retains physical context");done(0);
            check(reg(false,80)==0,"phase timeout cannot complete successfully");
        }
        reset();stall_phase=false;configure(0,0x1000,0x8000,64);pending(0);reg(true,68,1);done(0);
        check(reg(false,160)==1,"phase transitions reset per-phase interval");
    }
    void mixed_chain() {
        // Endian and alternating SET_INT persist across a chain.
        for(unsigned mode=1;mode<3;++mode) {
            reset();fill(0x1000,48);descriptor(0x4000,0x1010,0x8010,16,0x4010);
            descriptor(0x4010,0x1020,0x8020,16,3);
            configure(0,0x1000,0x8000,16,mode,1,3,0x4001);pending(0);reg(true,68,1);done(0);
            for(unsigned i=0;i<48;++i) check(memory[0x8000+i]==memory[0x1000+(i^((1u<<mode)-1))],"chain endian payload and static config");
            check(reg(false,80)==0x20003,"mixed SET_INT chain counts");
        }
        // P2M chain with swap16, one-byte peripheral credits across commands.
        reset();received.clear();descriptor(0x4000,0x1e000,0x8010,2,3);
        configure(0,0x1e000,0x8000,2,1,1,1,0x4001);
        reg(true,16,0x01010001);reg(true,32,2);reg(true,68,1);
        for(unsigned i=0;i<4;++i) {
            reg(true,0x1050,4);unsigned k=0;
            while(!(rc.read().to_uint()&4) && k++<100) wait(1,sc_core::SC_NS);
            check(k<100,"chain RX clear");wait(11,sc_core::SC_NS);
        }
        done(0);check(memory[0x8000]==2 && memory[0x8001]==1 && memory[0x8010]==4 && memory[0x8011]==3,"P2M endian chain");
        // Disable during fetch drains its response but must not issue next payload.
        reset();latency=2;descriptor(0x4000,0x1100,0x8100,16,3);
        configure(0,0x1000,0x8000,16,0,4,4,0x4001);seen.clear();reg(true,68,1);
        unsigned k=0;while(std::none_of(seen.begin(),seen.end(),[](auto o){return o.descriptor;}) && k++<100) wait(1,sc_core::SC_NS);
        check(k<100,"fetch observed");reg(true,64,0);wait(10,sc_core::SC_NS);auto before=calls;
        wait(10,sc_core::SC_NS);check(calls==before,"disabled fetch cannot launch payload");reg(true,64,1);done(0);
        // Reset discards a descriptor response from the old epoch.
        reset();configure(0,0x1000,0x8000,16,0,4,4,0x4001);seen.clear();reg(true,68,1);k=0;
        while(std::none_of(seen.begin(),seen.end(),[](auto o){return o.descriptor;}) && k++<100) wait(1,sc_core::SC_NS);
        check(k<100,"fetch before reset");reset();
        check(reg(false,72)==0 && reg(false,0)==0 && !irq.read(),"reset drops stale descriptor");
        // Test the actual 12-bit wrap, not only event-counter saturation.
        reset();latency=0;
        for(unsigned i=0;i<4096;++i) descriptor(0x4000+i*16,0x1000,0x18000,4,i==4095?3:((0x4000+(i+1)*16)|1));
        configure(0,0x1000,0x18000,4,0,4,4,0x4001);reg(true,68,1);done(0);
        check(reg(false,80)==0xf0001,"4097 completed commands wrap to one; 15 events saturate");
    }
    void stress() {
        latency=2;reverse_writes=true;
        for(unsigned iteration=0;iteration<30;++iteration) {
            reset();std::array<std::vector<unsigned char>,8> oracle;
            for(unsigned n=0;n<8;++n) {
                const unsigned mode=random()%3,group=1u<<mode,size=group*(1+random()%(256/group));
                const unsigned src=0x1000+n*0x400,dst=0x9000+n*0x400;
                fill(src,size);oracle[n].resize(size);
                for(unsigned i=0;i<size;++i) oracle[n][i]=memory[src+(i^(group-1))];
                configure(n,src,dst,size,mode,1+random()%127,1+random()%127);
                unsigned r=1+random()%4,w=1+random()%4,token=iteration%3==0?0:iteration%3==1?63:1+random()%63;
                reg(true,n*256+16,0xc0000000|(r<<24)|(token<<16)|(1+random()%127));
                reg(true,n*256+20,0xc0000000|(w<<24)|(token<<16)|(1+random()%127));
            }
            reg(true,0x1048,255);for(unsigned n=0;n<8;++n) done(n);
            for(unsigned n=0;n<8;++n) check(std::equal(oracle[n].begin(),oracle[n].end(),memory.begin()+0x9000+n*0x400),"mixed pending/token/write-order stress");
        }
        // Observe global write retirement with a younger channel returning first.
        reset();seen.clear();configure(0,0x1000,0x8000,4);configure(1,0x1100,0x8101,1);
        pending(0);pending(1);reg(true,0x1048,3);
        unsigned observed=0;
        while(std::none_of(seen.begin(),seen.end(),[](auto o){return !o.read && o.channel==1;}) && observed++<100) wait(1,sc_core::SC_NS);
        check(observed<100,"younger write accepted");wait(10,sc_core::SC_NS);
        check(reg(false,0x1a0)==0,"younger B response waits for older same-ID retirement");done(0);done(1);
        // With zero-latency targets both contenders stay ready: inspect full token groups.
        reverse_writes=false;latency=0;
        for(unsigned token:{0u,63u}) {
            reset();read_grants.clear();write_grants.clear();
            for(unsigned n=0;n<2;++n) {
                configure(n,0x1000+n*256,0x8000+n*256,128);
                const unsigned weight=n==0?token:1;
                reg(true,n*256+16,0xc4000001|(weight<<16));reg(true,n*256+20,0xc4000001|(weight<<16));
            }
            reg(true,0x1048,3);done(0);done(1);
            const unsigned budget=token?token:1;
            for(const auto* grants:{&read_grants,&write_grants}) {
                check(grants->size()>budget,"weighted grants observed");
                for(unsigned i=0;i<budget && i<grants->size();++i) check((*grants)[i]==0,"token grant group");
                if(grants->size()>budget) check((*grants)[budget]==1,"WRR releases bus after token budget");
            }
        }
        latency=2;
        // TX endpoint limit stays one even when enabled OUTS_MAX=4.
        reset();tx_peak=0;sent.clear();configure(0,0x1000,0x1f000,8);
        reg(true,16,0xc4010004);reg(true,20,0x44010004);reg(true,32,1u<<16);reg(true,68,1);
        for(unsigned i=0;i<2;++i) {
            reg(true,0x1054,2);unsigned k=0;while(!(tc.read().to_uint()&2) && k++<100) wait(1,sc_core::SC_NS);
            check(k<100,"TX max4 credit returns");wait(11,sc_core::SC_NS);
        }
        done(0);check(tx_peak==1 && sent.size()==8,"TX OUTS_MAX=4 is single-credit");
    }
    void timers_combined() {
        // Deterministic priority: transaction timeout wins over simultaneous watchdog.
        slow_channel_zero=true;configure(0,0x1000,0x8000,16);pending(0);reg(true,68,1);
        wait(35,sc_core::SC_NS);check(reg(false,160)==128,"transaction timeout wins same-time watchdog");
        done(0);slow_channel_zero=false;
        // A stalled request channel must not prevent another channel making progress.
        reset();configure(0,0x1e000,0x8000,1);reg(true,16,0x01010001);reg(true,32,2);
        fill(0x1000,1020);configure(1,0x1000,0x9000,1020);pending(1);reg(true,0x1048,3);
        wait(35,sc_core::SC_NS);check(reg(false,160)==4096,"watchdog isolated to waiting channel");
        check((reg(false,0x1a0)&~1u)==0,"healthy channel has no collateral timer fault");done(1);
        check(reg(false,0x1a0)==1 && std::equal(memory.begin()+0x1000,memory.begin()+0x13fc,memory.begin()+0x9000),"healthy channel completes while another watchdog faults");
        // Old deadlines must not fire on a new epoch. A new channel can still
        // legitimately hit its OWN watchdog while waiting for quarantined slots.
        for(unsigned duration:{40u,60u}) {
            reset();slow_duration_ns=duration;slow_channel_zero=true;
            configure(0,0x1000,0x8000,16);pending(0);reg(true,68,1);wait(5,sc_core::SC_NS);
            reset();slow_channel_zero=false;configure(0,0x1000,0x8000,16);pending(0);reg(true,68,1);done(0);
            wait(70,sc_core::SC_NS);
            check(reg(false,160)==(duration==40?1u:4096u),"old deadlines discarded; new watchdog obeys its own resource-wait interval");
            check(reg(false,0x10d0)==1,"old physical contexts eventually drain");
        }
    }
    void boundaries() {
        // MMIO arrival delay must be consumed once before the write is visible.
        unsigned char bytes[4]={0x78,0x56,0x34,0x12};tlm::tlm_generic_payload t;
        t.set_command(tlm::TLM_WRITE_COMMAND);t.set_address(0);t.set_data_ptr(bytes);t.set_data_length(4);t.set_streaming_width(4);
        auto before=sc_core::sc_time_stamp();auto delay=sc_core::sc_time(17,sc_core::SC_NS);cpu->b_transport(t,delay);
        check(sc_core::sc_time_stamp()-before==sc_core::sc_time(17,sc_core::SC_NS) && delay==sc_core::SC_ZERO_TIME,"MMIO annotated arrival consumed once");
        check(reg(false,0)==0x12345678,"MMIO value after arrival");
        // A cyclic command list must yield simulation time, so reset can recover.
        descriptor(0x4000,0x1000,0x8000,4,0x4001);configure(0,0x1000,0x8000,4,0,4,4,0x4001);
        latency=0;reg(true,68,1);wait(2,sc_core::SC_NS);
        check(reg(false,72)!=0,"cyclic chain allows future-time control thread");reset();check(reg(false,72)==0,"reset recovers cyclic list");
    }
    void run() override {
        reset();latency=1;
        if(scenario=="capabilities") capabilities();
        else if(scenario=="attributes") attributes();
        else if(scenario=="protocol_faults") protocol_faults();
        else if(scenario=="phase_timers") phase_timers();
        else if(scenario=="mixed_chain") mixed_chain();
        else if(scenario=="stress") stress();
        else if(scenario=="boundaries") boundaries();
        else if(scenario=="timers_combined") timers_combined();
        else check(false,"unknown review scenario");
        std::cout<<scenario<<": checks="<<checks<<" transfers="<<transfers<<" errors="<<errors<<'\n';sc_core::sc_stop();
    }
};
int sc_main(int argc,char** argv) {
    if(argc!=2)return 2;
    fu3::dma::EngineConfig config;config.design_capability=0x101;
    config.memory_read_cache=3;config.memory_write_cache=6;config.memory_read_prot=5;config.memory_write_prot=2;
    config.peripheral_read_cache=9;config.peripheral_write_cache=12;config.peripheral_read_prot=1;config.peripheral_write_prot=3;
    if(std::string(argv[1])=="phase_timers") for(auto& threshold:config.phase_timeouts) threshold=sc_core::sc_time(10,sc_core::SC_NS);
    if(std::string(argv[1])=="timers_combined") {
        config.transaction_timeout=sc_core::sc_time(30,sc_core::SC_NS);
        config.watchdog_timeout=sc_core::sc_time(30,sc_core::SC_NS);
    }
    Review bench{"bench",argv[1],config};sc_core::sc_start();return bench.errors?1:0;
}
