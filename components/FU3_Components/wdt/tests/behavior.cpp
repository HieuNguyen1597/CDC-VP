#include <fu3_wdt/wdt.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <iostream>

struct Bench:sc_core::sc_module {
    fu3_wdt::Watchdog dut{"dut",sc_core::sc_time(10,sc_core::SC_NS)};
    tlm_utils::simple_initiator_socket<Bench> cpu{"cpu"};
    sc_core::sc_signal<bool> pre{"pre"},rst{"rst"},gate{"gate"},halt{"halt"},irq{"irq"},req{"req"},pause{"pause"};
    unsigned checks=0,failures=0;bool finished=false;
    SC_HAS_PROCESS(Bench);
    explicit Bench(sc_core::sc_module_name n):sc_module(n) {
        cpu.bind(dut.socket);dut.preset_n(pre);dut.wdogres_n(rst);dut.clock_enabled(gate);dut.debug_halt(halt);
        dut.irq(irq);dut.reset_request(req);dut.paused(pause);
        SC_THREAD(run);SC_THREAD(watchdog);
    }
    void expect(bool ok,const char* message){++checks;if(!ok){++failures;std::cerr<<"FAIL @ "<<sc_core::sc_time_stamp()<<": "<<message<<'\n';}}
    void settle(){for(unsigned i=0;i<5;++i)wait(sc_core::SC_ZERO_TIME);}
    void at(unsigned ns){auto t=sc_core::sc_time(ns,sc_core::SC_NS);if(t>sc_core::sc_time_stamp())wait(t-sc_core::sc_time_stamp());settle();}
    unsigned access(bool wr,unsigned a,unsigned v=0,unsigned len=4,unsigned char* be=nullptr,unsigned bel=0,
        tlm::tlm_response_status expected=tlm::TLM_OK_RESPONSE) {
        unsigned char b[4];for(unsigned i=0;i<4;++i)b[i]=v>>(8*i);
        tlm::tlm_generic_payload tx;tx.set_address(a);tx.set_command(wr?tlm::TLM_WRITE_COMMAND:tlm::TLM_READ_COMMAND);
        tx.set_data_ptr(b);tx.set_data_length(len);tx.set_streaming_width(len);tx.set_byte_enable_ptr(be);tx.set_byte_enable_length(bel);
        auto delay=sc_core::SC_ZERO_TIME;cpu->b_transport(tx,delay);settle();
        expect(tx.get_response_status()==expected,"response status");expect(!tx.is_dmi_allowed()&&delay==sc_core::SC_ZERO_TIME,"delay consumed, no DMI");
        unsigned result=0;for(unsigned i=0;i<4;++i)result|=unsigned(b[i])<<(8*i);return result;
    }
    unsigned peek(unsigned a) {
        unsigned char b[4]={};tlm::tlm_generic_payload tx;tx.set_command(tlm::TLM_READ_COMMAND);tx.set_address(a);
        tx.set_data_ptr(b);tx.set_data_length(4);tx.set_streaming_width(4);
        const auto before=sc_core::sc_time_stamp();expect(cpu->transport_dbg(tx)==4&&tx.is_response_ok(),"debug inspection");
        expect(sc_core::sc_time_stamp()==before,"debug is untimed");
        unsigned value=0;for(unsigned i=0;i<4;++i)value|=unsigned(b[i])<<(8*i);return value;
    }
    void cold(){pre=false;rst=false;settle();pre=true;rst=true;gate=true;halt=false;settle();}
    void run(){
        cold();
        expect(peek(0x1000)==0&&peek(0x1004)==0xffffffff&&peek(0x1008)==0xffffffff&&peek(0x100c)==1,"reset defaults and unlocked readback");
        access(true,0x1004,3);access(true,0x100c,3);
        at(29);expect(!irq&&!req&&peek(0x1008)==1,"no premature first timeout");
        at(30);expect(irq&&!req&&peek(0x1010)==3&&peek(0x1008)==3,"first timeout at third tick, automatic reload");
        at(59);expect(irq&&!req,"no premature reset");
        at(60);expect(req&&peek(0x1010)==1,"second timeout requests reset and clears seen");
        at(69);expect(req,"default reset pulse lasts one tick");at(70);expect(!req,"reset pulse ends");
        access(true,0x1018,0);expect(!irq&&peek(0x1010)==0,"any CLEAR value acknowledges and rearms");
        access(true,0x100c,1); // stop and mask
        access(true,0x1004,0);expect(!irq&&peek(0x1010)==3&&peek(0x1014)==0,"zero load raises raw while disabled");
        access(true,0x1004,5);expect(peek(0x1010)==1,"LOAD cancels seen but retains raw");
        access(true,0x100c,3);expect(irq,"reenabling exposes retained raw");
        access(true,0x1018,123);expect(!irq,"CLEAR removes raw");
        access(true,0x100c,1);access(true,0x1000,0);
        access(true,0x1004,123);access(true,0x100c,0);access(true,0x1018,0);
        expect(peek(0x1000)==1&&peek(0x1004)==5&&peek(0x100c)==1,"lock blocks configuration writes");
        access(true,0x1000,0x1acce551);expect(peek(0x1000)==0,"key unlocks");
        access(true,0x1004,0x11223344);
        unsigned char sparse[2]={0xff,0};access(true,0x1004,0xaabbccdd,4,sparse,2);
        expect(peek(0x1004)==0x11bb33dd,"repeating sparse byte enables merge LOAD");
        access(true,0x1005,0x66,1);access(true,0x1006,0x7788,2);
        expect(peek(0x1004)==0x778866dd,"byte and halfword lane writes");
        unsigned char off=0;access(true,0x1004,0,4,&off,1);
        expect(peek(0x1004)==0x778866dd,"all disabled lanes have no side effect");
        access(true,0x1000,0x51,1);expect(peek(0x1000)==1,"partial key cannot unlock");
        access(true,0x1000,0x1acce551);access(true,0x1008,0);
        expect(peek(0x1008)==0x778866dd,"RO write ignored");expect(access(false,0x1018)==0,"WO reads zero");
        unsigned char bad=1;access(true,0x1004,0,4,&bad,1,tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
        access(false,0x1020,0,4,nullptr,0,tlm::TLM_ADDRESS_ERROR_RESPONSE);
        access(false,0,0,4,nullptr,0,tlm::TLM_ADDRESS_ERROR_RESPONSE);
        access(false,0x1001,0,2,nullptr,0,tlm::TLM_ADDRESS_ERROR_RESPONSE);
        access(false,0x1004,0,3,nullptr,0,tlm::TLM_BURST_ERROR_RESPONSE);

        // Debug gating preserves count; DBGEN alone does not pause.
        access(true,0x1004,100);access(true,0x100c,7);halt=true;settle();
        const auto held=peek(0x1008);wait(100,sc_core::SC_NS);settle();
        expect(pause&&peek(0x1008)==held&&peek(0x101c)==1,"debug halt freezes count");
        halt=false;settle();wait(20,sc_core::SC_NS);settle();
        expect(!pause&&peek(0x1008)==held-2,"debug resume and DBGEN alone permit counting");
        gate=false;settle();const auto gated=peek(0x1008);wait(50,sc_core::SC_NS);
        expect(peek(0x1008)==gated,"watchdog clock gate freezes counter");gate=true;settle();
        // Watchdog-only reset preserves programmed settings and lock.
        access(true,0x1000,0);rst=false;settle();
        expect(peek(0x1008)==0xffffffff&&peek(0x1010)==0&&!req&&!irq,"watchdog reset clears runtime state");
        expect(peek(0x1004)==100&&peek(0x100c)==7&&peek(0x1000)==1,"watchdog reset retains configuration and lock");
        access(false,0x1004,0,4,nullptr,0,tlm::TLM_GENERIC_ERROR_RESPONSE);rst=true;settle();
        pre=false;settle();expect(peek(0x1004)==0xffffffff&&peek(0x100c)==1&&peek(0x1000)==0,"programming reset defaults");pre=true;settle();

        // A known global tick boundary gives an independently calculated deadline.
        auto start=((sc_core::sc_time_stamp().value()/sc_core::sc_time(10,sc_core::SC_NS).value())+1)*10;
        at(static_cast<unsigned>(start));cold();
        access(true,0x1004,2);access(true,0x100c,3|(2u<<3));
        at(static_cast<unsigned>(start+20));expect(irq&&!req,"first short timeout");
        at(static_cast<unsigned>(start+40));expect(req,"RESDIV second timeout");
        access(true,0x100c,1|(2u<<3)); // stop counting without masking reset
        at(static_cast<unsigned>(start+79));expect(req,"four-tick reset pulse persists while counter disabled");
        at(static_cast<unsigned>(start+80));expect(!req,"RESDIV=2 pulse ends after four ticks");

        // No escalation with RESEN masked, even across repeated timeouts.
        cold();access(true,0x1004,1);access(true,0x100c,2);
        wait(40,sc_core::SC_NS);settle();expect(irq&&!req,"RESEN masks repeated second timeouts");
        access(true,0x100c,0);const auto stopped=peek(0x1008);
        wait(30,sc_core::SC_NS);settle();expect(!irq&&peek(0x1008)==stopped,"INTEN freezes counter and masks retained IRQ");

        // LOAD=0 generates one immediate warning and escalates on a later tick.
        start=((sc_core::sc_time_stamp().value()/sc_core::sc_time(10,sc_core::SC_NS).value())+1)*10;
        at(static_cast<unsigned>(start));cold();access(true,0x1004,0);access(true,0x100c,3);
        expect(irq&&!req,"zero load does not reset in the same delta");
        at(static_cast<unsigned>(start+9));expect(!req,"zero load waits for next tick before escalation");
        at(static_cast<unsigned>(start+10));expect(req,"zero load second timeout occurs on next tick");
        rst=false;settle();expect(!req&&!irq,"asserted watchdog reset cancels pulse asynchronously");rst=true;settle();

        // CLEAR completing exactly on a second-timeout tick cannot retract reset.
        start=((sc_core::sc_time_stamp().value()/sc_core::sc_time(10,sc_core::SC_NS).value())+1)*10;
        at(static_cast<unsigned>(start));cold();access(true,0x1004,2);access(true,0x100c,3);
        at(static_cast<unsigned>(start+39));access(true,0x1018,0);
        expect(req&&!irq&&peek(0x1010)==0,"expiry precedes simultaneous service, pulse survives CLEAR");
        cold();access(true,0x1004,0);access(true,0x1000,0);access(true,0x1018,1);
        expect(peek(0x1010)==3,"locked CLEAR cannot clear pending raw/seen");

        // Malformed transfers and debug writes never mutate the register bank.
        {
            unsigned char bytes[4]={};tlm::tlm_generic_payload tx;
            tx.set_address(0x1004);tx.set_data_ptr(bytes);tx.set_data_length(4);tx.set_streaming_width(4);
            tx.set_command(tlm::TLM_WRITE_COMMAND);
            expect(cpu->transport_dbg(tx)==0&&tx.get_response_status()==tlm::TLM_COMMAND_ERROR_RESPONSE,"debug writes rejected");
            tx.set_command(tlm::TLM_IGNORE_COMMAND);auto delay=sc_core::sc_time(7,sc_core::SC_NS);auto before=sc_core::sc_time_stamp();
            cpu->b_transport(tx,delay);
            expect(tx.get_response_status()==tlm::TLM_COMMAND_ERROR_RESPONSE&&delay==sc_core::SC_ZERO_TIME&&
                sc_core::sc_time_stamp()-before==sc_core::sc_time(8,sc_core::SC_NS),"invalid command yields and consumes annotation once");
            tx.set_command(tlm::TLM_READ_COMMAND);tx.set_data_ptr(nullptr);cpu->b_transport(tx,delay);
            expect(tx.get_response_status()==tlm::TLM_GENERIC_ERROR_RESPONSE,"null data rejected");
            tx.set_data_ptr(bytes);tx.set_streaming_width(1);cpu->b_transport(tx,delay);
            expect(tx.get_response_status()==tlm::TLM_BURST_ERROR_RESPONSE,"streaming wrap rejected");
            tx.set_streaming_width(4);tx.set_byte_enable_ptr(bytes);tx.set_byte_enable_length(0);cpu->b_transport(tx,delay);
            expect(tx.get_response_status()==tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE,"zero enable length rejected");
        }

        // Event-driven long timeout: billions of ticks must not require a tick loop.
        cold();access(true,0x1004,0xffffffff);access(true,0x100c,2);
        const auto long_start=sc_core::sc_time_stamp();
        wait(1,sc_core::SC_SEC);settle();
        expect(!irq&&!req&&peek(0x1008)==0xffffffff-100000000u,"large timeout advances arithmetically");
        expect(sc_core::sc_time_stamp()-long_start==sc_core::sc_time(1,sc_core::SC_SEC),"long simulation interval completed");
        finished=true;sc_core::sc_stop();
    }
    void watchdog(){wait(2,sc_core::SC_SEC);expect(false,"test watchdog");sc_core::sc_stop();}
};
int sc_main(int,char**){Bench b{"bench"};sc_core::sc_start();std::cout<<"WDT behavior: "<<b.checks<<" checks, "<<b.failures<<" failures\n";return b.failures||!b.finished?1:0;}
