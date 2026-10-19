#include "fu3/qspi/qspi_flash_model.h"
#include <tlm_utils/simple_initiator_socket.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace sc_core;
using namespace fu3::qspi;
struct FlashBench : sc_module {
    QspiFlashModel flash{"flash"};
    sc_signal<bool> rst{"rst"};
    tlm_utils::simple_initiator_socket<FlashBench> host{"host"};
    std::string scenario;
    bool passed=false;
    unsigned assertions=0;
    std::uint64_t sequence=0;
    SC_HAS_PROCESS(FlashBench);
    FlashBench(sc_module_name name,std::string test):sc_module(name),scenario(std::move(test)) {
        host.bind(flash.socket); flash.reset_n(rst); SC_THREAD(run);
    }
    void check(bool value,const char* message) { ++assertions; if(!value) throw std::runtime_error(message); }
    FlashTransaction descriptor(unsigned opcode,std::uint32_t address,unsigned length) {
        FlashTransaction m;
        m.frame=++sequence; m.instruction=static_cast<unsigned char>(opcode); m.instruction_lines=1;
        m.address=address; m.length=length; m.data_lines=length?1:0;
        if(opcode==3 || opcode==0xb || opcode==0x3b || opcode==0x6b || opcode==0xbb || opcode==0xeb ||
           opcode==2 || opcode==0x32 || opcode==0x20 || opcode==0xd8) { m.address_lines=1; m.address_bytes=3; }
        if(opcode==0xb || opcode==0x3b || opcode==0x6b) m.dummy_cycles=8;
        if(opcode==0x3b || opcode==0xbb) m.data_lines=2;
        if(opcode==0x6b || opcode==0xeb || opcode==0x32) m.data_lines=4;
        if(opcode==0xbb || opcode==0xeb) {
            m.address_lines=m.alternate_lines=opcode==0xbb?2:4; m.alternate_bytes=1;
            m.alternate=0xf0; m.dummy_cycles=opcode==0xeb?4:0;
        }
        return m;
    }
    tlm::tlm_response_status send(FlashTransaction& m,tlm::tlm_command command,unsigned char* p,unsigned n,
                                  bool attach=true,unsigned char* enables=nullptr,bool null_data=false,
                                  sc_time annotation=SC_ZERO_TIME) {
        tlm::tlm_generic_payload tx;
        tx.set_command(command); tx.set_address(std::uint64_t(m.address)+m.offset);
        tx.set_data_ptr(null_data?nullptr:p); tx.set_data_length(n); tx.set_streaming_width(n);
        tx.set_byte_enable_ptr(enables); tx.set_byte_enable_length(enables?1:0);
        if(attach) tx.set_extension(&m);
        host->b_transport(tx,annotation);
        if(attach) tx.clear_extension<FlashTransaction>();
        return tx.get_response_status();
    }
    tlm::tlm_response_status frame(FlashTransaction m,tlm::tlm_command command,std::vector<unsigned char>& data) {
        const auto instruction_lines=m.instruction_lines;
        do {
            unsigned n=static_cast<unsigned>(std::min<std::uint64_t>(4,m.length-m.offset));
            m.begin=m.offset==0; m.end=m.offset+n==m.length;
            m.instruction_lines=m.begin?instruction_lines:0;
            auto result=send(m,command,data.empty()?nullptr:data.data()+m.offset,n);
            if(result!=tlm::TLM_OK_RESPONSE) return result;
            m.offset+=n;
        } while(m.offset<m.length);
        return tlm::TLM_OK_RESPONSE;
    }
    tlm::tlm_response_status command(unsigned opcode,unsigned address=0) {
        std::vector<unsigned char> bytes;
        return frame(descriptor(opcode,address,0),tlm::TLM_IGNORE_COMMAND,bytes);
    }
    std::vector<unsigned char> read(unsigned opcode,unsigned address,unsigned length) {
        std::vector<unsigned char> bytes(length);
        check(frame(descriptor(opcode,address,length),tlm::TLM_READ_COMMAND,bytes)==tlm::TLM_OK_RESPONSE,"read command");
        return bytes;
    }
    tlm::tlm_response_status program(unsigned address,std::vector<unsigned char> bytes,unsigned opcode=2) {
        return frame(descriptor(opcode,address,static_cast<unsigned>(bytes.size())),tlm::TLM_WRITE_COMMAND,bytes);
    }
    void wren() { check(command(6)==tlm::TLM_OK_RESPONSE,"WREN"); }
    void ready() {
        for(unsigned i=0;i<1000;++i) {
            if(!(read(5,0,1)[0]&1)) return;
            wait(10,SC_US);
        }
        check(false,"flash operation watchdog");
    }
    void quad_enable() { wren(); check(program(0,{2},0x31)==tlm::TLM_OK_RESPONSE,"write QE"); ready(); }
    void commands() {
        check(read(0x9f,0,6)==std::vector<unsigned char>({0x7f,0x51,0x11,0x7f,0x51,0x11}),"configured synthetic JEDEC ID");
        check(read(5,0,3)==std::vector<unsigned char>(3,0),"initial status");
        wren(); check(read(5,0,1)[0]==2,"WEL set");
        check(command(4)==tlm::TLM_OK_RESPONSE && read(5,0,1)[0]==0,"WRDI clears WEL");
        flash.load({0x12,0x34,0x56,0x78,0x9a},8);
        for(unsigned op:{3u,0xbu,0x3bu,0xbbu}) check(read(op,8,5)==flash.read_image(8,5),"single/dual read bytes");
        auto m=descriptor(0x6b,8,4); std::vector<unsigned char> b(4);
        check(frame(m,tlm::TLM_READ_COMMAND,b)==tlm::TLM_COMMAND_ERROR_RESPONSE,"quad requires QE");
        quad_enable(); check(read(0x35,0,1)[0]==2,"QE status");
        for(unsigned op:{0x6bu,0xebu}) check(read(op,8,5)==flash.read_image(8,5),"quad read bytes");
        m=descriptor(0x0b,8,4); m.dummy_cycles=0;
        check(frame(m,tlm::TLM_READ_COMMAND,b)==tlm::TLM_COMMAND_ERROR_RESPONSE,"dummy phase checked");
    }
    void programs() {
        check(program(0,{0})==tlm::TLM_COMMAND_ERROR_RESPONSE,"program requires WEL");
        wren(); check(program(250,{1,2,3,4,5,6,7,8,9,10})==tlm::TLM_OK_RESPONSE,"program split across chunks/page wrap");
        check(read(5,0,1)[0]==1 && flash.read_image(250,6)==std::vector<unsigned char>(6,255),"WIP/WEL and delayed commit");
        std::vector<unsigned char> b(4);
        check(frame(descriptor(3,0,4),tlm::TLM_READ_COMMAND,b)==tlm::TLM_GENERIC_ERROR_RESPONSE,"array read rejected busy");
        check(command(6)==tlm::TLM_GENERIC_ERROR_RESPONSE,"WREN rejected busy");
        ready(); check(flash.read_image(250,6)==std::vector<unsigned char>({1,2,3,4,5,6}),"end page bytes");
        check(flash.read_image(0,4)==std::vector<unsigned char>({7,8,9,10}),"wrapped page bytes");
        check(flash.read_image(256,4)==std::vector<unsigned char>(4,255),"next page untouched");
        wren(); check(program(0,{0xff,0xf0,0,0xff})==tlm::TLM_OK_RESPONSE,"reprogram"); ready();
        check(read(3,0,4)==std::vector<unsigned char>({7,0,0,10}),"NOR only clears bits");
        wren(); check(program(0,std::vector<unsigned char>(257,0))==tlm::TLM_COMMAND_ERROR_RESPONSE,"profile page length bound");
        quad_enable(); wren(); check(program(512,{0xaa,0x55,0x81},0x32)==tlm::TLM_OK_RESPONSE,"quad page program"); ready();
        check(read(0x6b,512,3)==std::vector<unsigned char>({0xaa,0x55,0x81}),"quad program readback");
        // A partial frame cancelled before CS close must not program anything.
        wren(); auto m=descriptor(2,768,8); m.begin=true; unsigned char data[4]={0,0,0,0};
        check(send(m,tlm::TLM_WRITE_COMMAND,data,4)==tlm::TLM_OK_RESPONSE,"program first chunk");
        m.begin=false; m.end=true; m.cancel=true; m.offset=4; m.instruction_lines=0;
        check(send(m,tlm::TLM_IGNORE_COMMAND,nullptr,0)==tlm::TLM_OK_RESPONSE,"cancel open program frame");
        wait(60,SC_US); check(flash.read_image(768,8)==std::vector<unsigned char>(8,255) && read(5,0,1)[0]==0,"cancel discards staged data and WEL");
    }
    void erases() {
        flash.load(std::vector<unsigned char>(flash.size(),0));
        for(unsigned op:{0x20u,0xd8u,0xc7u,0x60u}) {
            check(command(op,op==0x20 || op==0xd8?123:0)==tlm::TLM_COMMAND_ERROR_RESPONSE,"erase requires WEL");
            wren(); check(command(op,op==0x20 || op==0xd8?123:0)==tlm::TLM_OK_RESPONSE,"erase accepted");
            check(read(5,0,1)[0]==1,"erase WIP"); ready();
            const unsigned n=op==0x20?4096:(op==0xd8?65536:static_cast<unsigned>(flash.size()));
            check(flash.read_image(0,n)==std::vector<unsigned char>(n,255),"erased aligned region");
            if(n<flash.size()) check(flash.read_image(n,1)[0]==0,"erase neighbor preserved");
            flash.load(std::vector<unsigned char>(flash.size(),0));
        }
    }
    void protocol() {
        unsigned char bytes[8]{};
        auto m=descriptor(3,0,4); m.begin=m.end=true;
        check(send(m,tlm::TLM_READ_COMMAND,bytes,4,false)==tlm::TLM_GENERIC_ERROR_RESPONSE,"metadata required");
        check(send(m,tlm::TLM_READ_COMMAND,bytes,4,true,nullptr,true)==tlm::TLM_GENERIC_ERROR_RESPONSE,"null data");
        unsigned char be=255;
        check(send(m,tlm::TLM_READ_COMMAND,bytes,4,true,&be)==tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE,"flash byte enables rejected");
        check(send(m,tlm::TLM_READ_COMMAND,bytes,8)==tlm::TLM_BURST_ERROR_RESPONSE,"bounded chunks");
        std::vector<unsigned char> b(4);
        check(frame(descriptor(3,static_cast<unsigned>(flash.size()-2),4),tlm::TLM_READ_COMMAND,b)==tlm::TLM_COMMAND_ERROR_RESPONSE,"full frame range validated before data");
        check(command(0xff)==tlm::TLM_COMMAND_ERROR_RESPONSE,"unsupported opcode");
        m=descriptor(3,0,4); m.ddr=true;
        check(frame(m,tlm::TLM_READ_COMMAND,b)==tlm::TLM_COMMAND_ERROR_RESPONSE,"profile rejects DDR");
        m=descriptor(3,0,4); m.address_bytes=4;
        check(frame(m,tlm::TLM_READ_COMMAND,b)==tlm::TLM_COMMAND_ERROR_RESPONSE,"profile uses three address bytes");
        m=descriptor(3,0,8); m.begin=true;
        check(send(m,tlm::TLM_READ_COMMAND,bytes,4)==tlm::TLM_OK_RESPONSE,"first read chunk");
        m.begin=false; m.end=true; m.offset=5; m.instruction_lines=0;
        check(send(m,tlm::TLM_READ_COMMAND,bytes,3)==tlm::TLM_GENERIC_ERROR_RESPONSE,"out-of-order chunk rejected");
        check(read(3,0,4)==std::vector<unsigned char>(4,255),"recovers after malformed frame");
        wren(); check(program(0,{0x80},0x31)==tlm::TLM_COMMAND_ERROR_RESPONSE,"unsupported status bits rejected");
        check(!flash.busy(),"invalid status write has no pending operation");
    }
    void resets() {
        flash.load({0x12,0x34},0); quad_enable();
        check(command(0x99)==tlm::TLM_COMMAND_ERROR_RESPONSE,"reset requires enable reset");
        check(command(0x66)==tlm::TLM_OK_RESPONSE,"enable reset"); read(5,0,1);
        check(command(0x99)==tlm::TLM_COMMAND_ERROR_RESPONSE,"intervening command disarms reset");
        wren(); check(program(256,{0})==tlm::TLM_OK_RESPONSE,"program before reset");
        check(command(0x66)==tlm::TLM_OK_RESPONSE && command(0x99)==tlm::TLM_OK_RESPONSE,"software reset while busy");
        wait(60,SC_US); check(!flash.busy() && read(0x35,0,1)[0]==0,"reset clears busy and volatile QE");
        check(read(3,256,1)[0]==255 && read(3,0,2)==std::vector<unsigned char>({0x12,0x34}),"reset cancels uncommitted program, preserves stored bytes");
        wren(); check(command(0x20)==tlm::TLM_OK_RESPONSE,"erase before hardware reset");
        rst=false; wait(1,SC_NS); check(command(6)==tlm::TLM_GENERIC_ERROR_RESPONSE,"held flash reset rejects access");
        rst=true; wait(250,SC_US);
        check(read(3,0,2)==std::vector<unsigned char>({0x12,0x34}) && read(5,0,1)[0]==0,"reset prevents stale erase completion");
    }
    void images() {
        namespace fs=std::filesystem;
        const fs::path path="qspi-test-image.bin";
        flash.load({1,2,3,4},4); flash.save_image(path.string());
        flash.load({0,0,0,0},4); flash.load_image(path.string());
        check(flash.read_image(4,4)==std::vector<unsigned char>({1,2,3,4}),"image roundtrip");
        bool rejected=false;
        try { flash.load({1,2},flash.size()-1); } catch(const std::out_of_range&) { rejected=true; }
        check(rejected && flash.read_image(flash.size()-1,1)[0]==255,"load range check atomic");
        rejected=false; try { flash.load_image(path.string(),1); } catch(const std::out_of_range&) { rejected=true; }
        check(rejected,"oversized file rejected");
        wren(); check(program(0,{0})==tlm::TLM_OK_RESPONSE,"program for image exclusion");
        rejected=false; try { flash.load({9}); } catch(const std::logic_error&) { rejected=true; }
        check(rejected,"image load rejected busy");
        rejected=false; try { flash.save_image(path.string()); } catch(const std::logic_error&) { rejected=true; }
        check(rejected,"image save rejected busy"); ready();
        fs::remove(path);
    }
    void sioo() {
        quad_enable(); flash.load({1,2,3,4,5,6,7,8});
        auto m=descriptor(0xeb,0,4); std::vector<unsigned char> b(4); m.instruction_lines=0;
        check(frame(m,tlm::TLM_READ_COMMAND,b)==tlm::TLM_COMMAND_ERROR_RESPONSE,"no command before continuous mode");
        m=descriptor(0xeb,0,4); m.alternate=0xa0;
        check(frame(m,tlm::TLM_READ_COMMAND,b)==tlm::TLM_OK_RESPONSE && b==std::vector<unsigned char>({1,2,3,4}),"enter continuous quad IO read");
        m=descriptor(0,4,4); m.instruction_lines=0; m.address_lines=m.alternate_lines=m.data_lines=4;
        m.address_bytes=3; m.alternate_bytes=1; m.alternate=0xa0; m.dummy_cycles=4;
        check(frame(m,tlm::TLM_READ_COMMAND,b)==tlm::TLM_OK_RESPONSE && b==std::vector<unsigned char>({5,6,7,8}),"SIOO uses retained instruction");
        m.frame=++sequence; m.alternate=0xf0;
        check(frame(m,tlm::TLM_READ_COMMAND,b)==tlm::TLM_OK_RESPONSE,"exit continuous mode");
        m.frame=++sequence;
        check(frame(m,tlm::TLM_READ_COMMAND,b)==tlm::TLM_COMMAND_ERROR_RESPONSE,"instruction required after exit");
    }
    void integrity() {
        // FlashTransaction API tests; these are not claims about FU3 pad/CDC RTL.
        std::vector<unsigned char> page(256);
        for(unsigned i=0;i<256;++i) page[i]=static_cast<unsigned char>((i*17)^0xa7);
        wren(); check(program(1024,page)==tlm::TLM_OK_RESPONSE,"64-chunk full page accepted");
        ready(); check(read(3,1024,256)==page,"full page preserves every byte across chunk boundaries");
        check(read(3,1023,1)[0]==255 && read(3,1280,1)[0]==255,"full page preserves adjacent bytes");
        // A foreign begin/cancel must not clobber another open frame.
        wren(); auto m=descriptor(2,2048,8); m.begin=true;
        unsigned char first[4]={0x12,0x34,0x56,0x78},second[4]={0x9a,0xbc,0xde,0xf0};
        check(send(m,tlm::TLM_WRITE_COMMAND,first,4)==tlm::TLM_OK_RESPONSE,"open program frame");
        auto foreign=descriptor(6,0,0); foreign.begin=foreign.end=true;
        check(send(foreign,tlm::TLM_IGNORE_COMMAND,nullptr,0)==tlm::TLM_GENERIC_ERROR_RESPONSE,"foreign frame cannot replace active frame");
        foreign.begin=false; foreign.cancel=true;
        check(send(foreign,tlm::TLM_IGNORE_COMMAND,nullptr,0)==tlm::TLM_GENERIC_ERROR_RESPONSE,"foreign cancel cannot close active frame");
        m.offset=4; m.begin=false; m.end=true; m.instruction_lines=0;
        check(send(m,tlm::TLM_WRITE_COMMAND,second,4)==tlm::TLM_OK_RESPONSE,"original continuation remains valid");
        check(send(m,tlm::TLM_WRITE_COMMAND,second,4)==tlm::TLM_GENERIC_ERROR_RESPONSE,"duplicate final chunk rejected");
        check(flash.busy(),"rejected duplicate does not cancel already accepted program"); ready();
        check(read(3,2048,8)==std::vector<unsigned char>({0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0}),"original frame committed once");
        // Mutate each stable descriptor field in a continuation; staged writes
        // must never reach storage, even after the normal program deadline.
        for(unsigned field=0;field<12;++field) {
            wren(); m=descriptor(2,4096+field*256,8); m.begin=true;
            check(send(m,tlm::TLM_WRITE_COMMAND,first,4)==tlm::TLM_OK_RESPONSE,"staged integrity prefix");
            m.offset=4; m.begin=false; m.end=true; m.instruction_lines=0;
            switch(field) {
            case 0: ++m.length; break;
            case 1: ++m.address; break;
            case 2: m.instruction=0x32; break;
            case 3: m.address_lines=2; break;
            case 4: m.address_bytes=4; break;
            case 5: m.alternate_lines=1; break;
            case 6: m.alternate_bytes=1; break;
            case 7: m.alternate=0xa0; break;
            case 8: m.data_lines=4; break;
            case 9: m.dummy_cycles=1; break;
            case 10: m.ddr=true; break;
            case 11: m.clock_mode3=true; break;
            }
            check(send(m,tlm::TLM_WRITE_COMMAND,second,4)!=tlm::TLM_OK_RESPONSE,"changed continuation descriptor rejected");
            wait(60,SC_US);
            check(flash.read_image(4096+field*256,8)==std::vector<unsigned char>(8,255),"malformed frame never commits partial data");
        }
        // Direction changes, replayed offsets, and an early final marker.
        for(unsigned error=0;error<3;++error) {
            wren(); m=descriptor(2,8192+error*256,8); m.begin=true;
            check(send(m,tlm::TLM_WRITE_COMMAND,first,4)==tlm::TLM_OK_RESPONSE,"ordering prefix");
            m.begin=false; m.instruction_lines=0; m.offset=error==1?0:4; m.end=error!=2;
            check(send(m,error==0?tlm::TLM_READ_COMMAND:tlm::TLM_WRITE_COMMAND,second,4)!=tlm::TLM_OK_RESPONSE,"direction/order/final marker error");
            wait(60,SC_US); check(flash.read_image(8192+error*256,8)==std::vector<unsigned char>(8,255),"invalid order preserves array");
        }
        // New well-formed traffic must still succeed after all error paths.
        wren(); check(program(12288,{0x13,0x05,0x10,0})==tlm::TLM_OK_RESPONSE,"recovery program"); ready();
        check(read(3,12288,4)==std::vector<unsigned char>({0x13,0x05,0x10,0}),"recovery readback");
    }
    void run() {
        try {
            rst=false; wait(1,SC_NS); rst=true; wait(1,SC_NS);
            if(scenario=="flash_commands") commands(); else if(scenario=="flash_program") programs();
            else if(scenario=="flash_erase") erases(); else if(scenario=="flash_protocol") protocol();
            else if(scenario=="flash_reset") resets(); else if(scenario=="flash_image") images();
            else if(scenario=="flash_sioo") sioo(); else if(scenario=="flash_integrity") integrity();
            else check(false,"unknown scenario");
            passed=true; std::cout<<"PASS "<<scenario<<" assertions="<<assertions<<" time="<<sc_time_stamp()<<'\n';
        } catch(const std::exception& e) { std::cerr<<"FAIL "<<scenario<<": "<<e.what()<<'\n'; }
        sc_stop();
    }
};
int sc_main(int argc,char** argv) {
    FlashBench b{"bench",argc>1?argv[1]:"flash_commands"}; sc_start(sc_time(100,SC_MS)); return b.passed?0:1;
}
