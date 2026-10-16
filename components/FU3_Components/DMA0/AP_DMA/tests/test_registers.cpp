#include "fu3/dma/ap_dma.h"
#include <iostream>
#include <cstring>

struct Bench : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<Bench> cpu{"cpu"};
    tlm_utils::simple_target_socket<Bench> memory{"memory"};
    sc_core::sc_out<bool> reset_n{"reset_n"};
    sc_core::sc_in<bool> irq{"irq"};
    sc_core::sc_in<sc_dt::sc_uint<32>> rx_clear{"rx_clear"}, tx_clear{"tx_clear"};
    unsigned master_transactions = 0;
    unsigned errors = 0;
    SC_CTOR(Bench) {
        memory.register_b_transport(this, &Bench::memory_access);
        SC_THREAD(run);
    }
    void check(bool condition, const char* message) {
        if (!condition) { ++errors; std::cerr << "FAIL: " << message << '\n'; }
    }
    void memory_access(tlm::tlm_generic_payload& tx, sc_core::sc_time&) {
        ++master_transactions;
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
    }
    std::uint32_t access(bool write, std::uint64_t address, std::uint32_t value = 0,
                         tlm::tlm_response_status expected = tlm::TLM_OK_RESPONSE) {
        unsigned char data[4];
        for (unsigned i = 0; i < 4; ++i) data[i] = (value >> (8*i)) & 0xff;
        tlm::tlm_generic_payload tx;
        tx.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        tx.set_address(address); tx.set_data_ptr(data);
        tx.set_data_length(4); tx.set_streaming_width(4);
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        cpu->b_transport(tx, delay);
        if (tx.get_response_status() != expected) {
            ++errors; std::cerr << "Unexpected response at " << std::hex << address << std::dec << '\n';
        }
        std::uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i) result |= std::uint32_t(data[i]) << (8*i);
        return result;
    }
    void register_checks() {
        struct Field { unsigned offset; std::uint32_t reset, mask; };
        const Field fields[] = {
            {0x00,0,0xffffffff}, {0x04,0,0xffffffff}, {0x08,0,0x3ff},
            {0x0c,0,0xffffffff}, {0x10,0x84010000,0xcf3f007f},
            {0x14,0x84010000,0xcf3f007f}, {0x18,0,0x30000000},
            {0x1c,0,0}, {0x20,0,0x071f071f}, {0x24,0,0xbfbf},
            {0x40,1,1}, {0xa8,0x1fff,0x1fff}
        };
        for (unsigned ch = 0; ch < 8; ++ch) {
            const auto base = ch * 0x100;
            for (const auto& f : fields) {
                check(access(false, base+f.offset) == f.reset, "channel reset value");
                access(true, base+f.offset, 0xffffffff);
                check(access(false, base+f.offset) == f.mask, "channel write mask");
            }
            // A channel write must not leak into another channel context.
            access(true, base, 0x12000000 + ch);
            for (auto off : {0x30u,0x34u,0x3cu,0x48u,0x50u,0xa0u,0xacu})
                check(access(false, base+off) == 0, "inactive status");
            for (auto off : {0x28u,0x4cu,0x54u,0x9cu,0xb0u,0xfcu})
                access(false, base+off, 0, tlm::TLM_ADDRESS_ERROR_RESPONSE);
            for (auto off : {0x44u})
                access(false, base+off, 0, tlm::TLM_COMMAND_ERROR_RESPONSE);
            access(true, base+0xa0, 1);
            access(true, base+0xa4, 0x1fff);
            access(true, base+0x48, 1, tlm::TLM_COMMAND_ERROR_RESPONSE);
        }
        for (unsigned ch=0; ch<8; ++ch)
            check(access(false, ch*0x100) == 0x12000000+ch, "channel isolation");
        check(access(false,0x10f0)==0x0a602258, "capability0");
        check(access(false,0x10d0)==1, "idle");
        check(access(false,0x1000)==0, "core IRQ bitmap");
        for (auto off : {0x1030u,0x1040u}) {
            access(true,off,0xffffffff); check(access(false,off)==0,"compatibility register");
        }
        access(true,0x1038,0xffffffff);
        check(access(false,0x1038)==0xffff,"priority storage mask");
        for (auto off : {0x1050u,0x1054u}) {
            access(true,off,0xffffffff);
            check(access(false,off)==0xfffffffe,"software request excludes memory bit");
        }
        for (auto off : {0x800ull,0xffcull,0x1004ull,0x10fcull,0x1100ull,0x1000010f0ull,1ull})
            access(false,off,0,tlm::TLM_ADDRESS_ERROR_RESPONSE);
        access(true,0x1048,1,tlm::TLM_COMMAND_ERROR_RESPONSE);
        check(access(false,0x10f4)==0xf17,"implemented capability flags");
        reset_n.write(false); wait(1,sc_core::SC_NS);
        access(true,0,1,tlm::TLM_COMMAND_ERROR_RESPONSE);
        reset_n.write(true); wait(1,sc_core::SC_NS);
        for (unsigned ch=0; ch<8; ++ch)
            for (const auto& f : fields)
                check(access(false,ch*0x100+f.offset)==f.reset,"reset restores modified state");
        check(access(false,0x1050)==0 && access(false,0x1054)==0,"reset software requests");
        check(access(false,0x1038)==0,"reset priority storage");
        // A peripheral timebase must be supplied explicitly by the platform.
        access(true,0,0x1000); access(true,4,0xa000); access(true,8,1); access(true,12,3);
        access(true,16,0x81010008); access(true,20,0x01010001); access(true,0x20,1u<<16);
        access(true,0x44,1,tlm::TLM_COMMAND_ERROR_RESPONSE);
    }
    void run() {
        reset_n.write(false);
        wait(1, sc_core::SC_NS);
        check(!irq.read() && rx_clear.read() == 0 && tx_clear.read() == 0,
              "outputs inactive during reset");
        reset_n.write(true);
        wait(1, sc_core::SC_NS);
        unsigned char bytes[4] = {0x55, 0xaa, 0x55, 0xaa};
        tlm::tlm_generic_payload tx;
        tx.set_command(tlm::TLM_READ_COMMAND);
        tx.set_address(0x1004);
        tx.set_data_ptr(bytes);
        tx.set_data_length(4);
        tx.set_streaming_width(4);
        tx.set_dmi_allowed(true);
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        cpu->b_transport(tx, delay);
        check(tx.get_response_status() == tlm::TLM_ADDRESS_ERROR_RESPONSE,
              "reserved register access explicitly rejected");
        check(!tx.is_dmi_allowed(), "DMI disabled");
        check(bytes[0] == 0x55 && bytes[1] == 0xaa && bytes[2] == 0x55 && bytes[3] == 0xaa,
              "unimplemented read must not fabricate register data");
        tx.set_address(0);
        tx.set_data_length(2);
        cpu->b_transport(tx, delay);
        check(tx.get_response_status() == tlm::TLM_BURST_ERROR_RESPONSE, "reject short register access");
        tx.set_data_length(4);
        unsigned char enables[4] = {0xff,0xff,0xff,0xff};
        tx.set_byte_enable_ptr(enables);
        cpu->b_transport(tx, delay);
        check(tx.get_response_status() == tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE, "reject byte enables");
        tx.set_byte_enable_ptr(nullptr);
        tx.set_command(tlm::TLM_IGNORE_COMMAND);
        cpu->b_transport(tx, delay);
        check(tx.get_response_status() == tlm::TLM_COMMAND_ERROR_RESPONSE, "reject unsupported command");
        register_checks();
        check(master_transactions == 0, "register model must not issue DMA traffic");
        check(!irq.read() && rx_clear.read() == 0 && tx_clear.read() == 0,
              "outputs inactive after reset release");
        sc_core::sc_stop();
    }
};
int sc_main(int, char**) {
    if (!std::strstr(sc_core::sc_version(), "2.3.4")) {
        std::cerr << "SystemC runtime must be 2.3.4\n";
        return 1;
    }
    fu3::dma::ApDma dma{"dma"};
    Bench bench{"bench"};
    sc_core::sc_signal<bool> reset_n, irq;
    sc_core::sc_signal<sc_dt::sc_uint<32>> rx_request, tx_request, rx_clear, tx_clear;
    bench.cpu.bind(dma.target_socket);
    dma.master_socket.bind(bench.memory);
    dma.reset_n(reset_n); bench.reset_n(reset_n);
    dma.irq(irq); bench.irq(irq);
    dma.rx_request(rx_request); dma.tx_request(tx_request);
    dma.rx_clear(rx_clear); bench.rx_clear(rx_clear);
    dma.tx_clear(tx_clear); bench.tx_clear(tx_clear);
    sc_core::sc_start();
    std::cout << "Register/reset errors: " << bench.errors << '\n';
    return bench.errors ? 1 : 0;
}
