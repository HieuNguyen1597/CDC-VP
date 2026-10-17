#include <fu3_wdt/wdt_bank.h>
#include <fu3_bus/bus_system.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <iostream>

struct Bench:sc_core::sc_module {
    static fu3_bus::BusConfig config(){auto c=fu3_bus::BusConfig::fu3();for(auto& t:c.targets)t.enabled=t.name=="WDT0"||t.name=="WDT1";return c;}
    fu3_bus::BusSystem bus{"bus",config()};
    fu3_wdt::WatchdogBank bank{"bank"};
    tlm_utils::simple_initiator_socket<Bench> cpu{"cpu"};
    sc_core::sc_signal<bool> pre{"pre"},gate{"gate"},halt{"halt"};
    sc_core::sc_vector<sc_core::sc_signal<bool>> rst{"rst",2},irq{"irq",2},req{"req",2},pause{"pause",2};
    sc_core::sc_signal<bool> ibex_nmi{"ibex_nmi"},plic18{"plic18"};
    unsigned resets[2]={},checks=0,failures=0;bool finished=false;
    SC_HAS_PROCESS(Bench);
    explicit Bench(sc_core::sc_module_name n):sc_module(n) {
        cpu.bind(bus.initiator("IBEX"));
        for(unsigned i=0;i<2;++i){auto& w=*bank.watchdogs[i];bus.target("WDT"+std::to_string(i)).bind(w.socket);
            w.preset_n(pre);w.wdogres_n(rst[i]);w.clock_enabled(gate);w.debug_halt(halt);w.irq(irq[i]);w.reset_request(req[i]);w.paused(pause[i]);}
        SC_METHOD(route);sensitive<<irq[0]<<irq[1];
        SC_THREAD(run);SC_THREAD(crm0);SC_THREAD(crm1);SC_THREAD(watchdog);
    }
    void route(){ibex_nmi=irq[0];plic18=irq[1];}
    void crm(unsigned i){rst[i]=false;wait(3,sc_core::SC_NS);rst[i]=true;
        while(true){wait(req[i].posedge_event());++resets[i];wait(1,sc_core::SC_NS);rst[i]=false;wait(5,sc_core::SC_NS);rst[i]=true;}}
    void crm0(){crm(0);}void crm1(){crm(1);}
    void expect(bool ok,const char* why){++checks;if(!ok){++failures;std::cerr<<"FAIL: "<<why<<'\n';}}
    void settle(){for(unsigned i=0;i<5;++i)wait(sc_core::SC_ZERO_TIME);}
    unsigned access(unsigned i,bool wr,unsigned offset,unsigned value=0,unsigned len=4) {
        unsigned char b[4];for(unsigned j=0;j<4;++j)b[j]=value>>(8*j);
        tlm::tlm_generic_payload tx;tx.set_command(wr?tlm::TLM_WRITE_COMMAND:tlm::TLM_READ_COMMAND);
        const auto address=fu3_wdt::WatchdogBank::bases[i]+offset;
        tx.set_address(address);tx.set_data_ptr(b);tx.set_data_length(len);tx.set_streaming_width(len);
        fu3_bus::ScopedAccess access(tx,len);auto delay=sc_core::sc_time(2,sc_core::SC_NS);cpu->b_transport(tx,delay);settle();
        expect(tx.is_response_ok(),"real bus transaction succeeds");expect(tx.get_address()==address&&delay==sc_core::SC_ZERO_TIME,"bus restores address and consumes delay");
        unsigned result=0;for(unsigned j=0;j<4;++j)result|=unsigned(b[j])<<(8*j);return result;
    }
    void run(){
        pre=false;gate=true;halt=false;wait(5,sc_core::SC_NS);pre=true;settle();
        access(0,true,0x1004,0x11223344);access(0,true,0x1005,0xaa,1);
        access(1,true,0x1004,0x55667788);
        expect(access(0,false,0x1004)==0x1122aa44&&access(1,false,0x1004)==0x55667788,"independent windows and byte lanes through A2P");
        for(unsigned i=0;i<2;++i){access(i,true,0x1004,100);access(i,true,0x100c,3);access(i,true,0x1000,0);}
        wait(1000,sc_core::SC_NS);settle();
        expect(ibex_nmi&&plic18&&resets[0]==0&&resets[1]==0,"both warning routes assert before reset");
        access(0,true,0x1018,0);expect(ibex_nmi,"locked CLEAR cannot acknowledge");
        access(0,true,0x1000,0x1acce551);access(0,true,0x1018,0);access(0,true,0x100c,1);
        expect(!ibex_nmi&&plic18,"servicing WDT0 does not clear WDT1");
        wait(1100,sc_core::SC_NS);settle();
        expect(resets[0]==0&&resets[1]==1&&!plic18&&!req[1],"fake CRM receives WDT1 reset and returns watchdog reset");
        expect(access(1,false,0x1004)==100&&access(1,false,0x100c)==3&&access(1,false,0x1000)==1,"warm reset retains load, enable and lock");
        expect(access(1,false,0x1008)>0xffff0000u,"watchdog runtime restarts at documented reset value");
        access(1,true,0x1000,0x1acce551);access(1,true,0x100c,1);
        access(0,true,0x1004,10);access(0,true,0x100c,3);
        wait(250,sc_core::SC_NS);settle();expect(resets[0]==1&&resets[1]==1,"WDT0 independently reaches reset path");
        finished=true;sc_core::sc_stop();
    }
    void watchdog(){wait(100,sc_core::SC_US);expect(false,"integration watchdog");sc_core::sc_stop();}
};
int sc_main(int,char**){Bench b{"bench"};sc_core::sc_start();std::cout<<"WDT integration: "<<b.checks<<" checks, "<<b.failures<<" failures\n";return b.failures||!b.finished?1:0;}
