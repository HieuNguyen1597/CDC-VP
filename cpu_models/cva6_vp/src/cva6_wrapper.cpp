#include "cva6_wrapper.h"
#include <stdexcept>
#include <iostream>

#include <tlm.h>
#include <tlm_utils/simple_initiator_socket.h>

#include <cdc/cpu/elf_loader.h>

#include <fu3_bus/access.h>

#include "core/rv32/iss.h"

#include "core/common/clint_if.h"

namespace cdc::cpu {

struct stub_clint : public clint_if {
    std::uint64_t update_and_get_mtime() override {
        return static_cast<std::uint64_t>(sc_core::sc_time_stamp().value());
    }
};

// Đổi chữ cái extension cần bỏ thành mask misa của ISS (mặc định IMACF + NUS).
// Bitmanip (RVB = 1) không có trong ISS Bremen nên không bật được.
static std::uint32_t misa_mask_from_letters(const std::string& letters) {
    std::uint32_t mask = 0;
    for (char c : letters) {
        switch (c) {
            case 'A': mask |= rv32::csr_misa::A; 
                break;
            case 'C': mask |= rv32::csr_misa::C; 
                break;
            case 'D': mask |= rv32::csr_misa::D; 
                break;
            case 'F': mask |= rv32::csr_misa::F; 
                break;
            case 'M': mask |= rv32::csr_misa::M; 
                break;
            case 'N': mask |= rv32::csr_misa::N; 
                break;
            case 'S': mask |= rv32::csr_misa::S; break;
            case 'U': mask |= rv32::csr_misa::U; 
                break;
            default:
                throw std::invalid_argument(std::string("Cannot disable extension '") + c + "'");
        }
    }
    return mask;
}

struct CVA6_InstrAdapter : public rv32::instr_memory_if {
    tlm_utils::simple_initiator_socket<cva6_wrapper, 32>& i_socket;

    // i_socket truyền vào
    CVA6_InstrAdapter(tlm_utils::simple_initiator_socket<cva6_wrapper, 32>& socket) 
        : i_socket(socket) {}

    // Đọc 1 word lệnh căn 4: fault_addr = địa chỉ báo vào mtval khi bus lỗi
    uint32_t fetch_word(uint64_t addr, uint64_t fault_addr)  {
        uint32_t instruction = 0;
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::sc_time(10, sc_core::SC_NS); // Đồng bộ 10ns cho Global Quantum

        trans.set_command(tlm::TLM_READ_COMMAND);
        trans.set_address(addr);
        trans.set_data_ptr(reinterpret_cast<unsigned char*>(&instruction));
        trans.set_data_length(4); // Lệnh RV32 luôn là 4 bytes
        trans.set_streaming_width(4);
        trans.set_byte_enable_ptr(0);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        fu3_bus::ScopedAccess access(trans, 4, fu3_bus::Burst::Increment, /*instruction=*/true);

        i_socket->b_transport(trans, delay);

        if (trans.is_response_error()) {
            // Nếu Bus báo lỗi, ném ngoại lệ để ISS bắt thành trap Access Fault
            // throw std::runtime_error("Bus error luc fetch lenh tai dia chi: " + std::to_string(addr));
            // ISS chỉ bắt SimulationTrap, runtime_error làm dừng cả VP thay vì sinh trap.
            raise_trap(EXC_INSTR_ACCESS_FAULT, fault_addr);   // mcause 1
        }
        return instruction;
    }

    // Bus FU3 chỉ nhận fetch 4 byte căn 4, nhưng có RVC thì PC có thể chỉ căn 2 => đọc word căn hàng rồi ghép, giống fetch thật của CVA6/Ibex.
    uint32_t load_instr(uint64_t addr) override {
        if ((addr & 3) == 0) {
            return fetch_word(addr, addr);
        }
        const uint64_t base = addr & ~uint64_t{3};
        const uint32_t low = fetch_word(base, addr) >> 16;
        if ((low & 3) != 3) {
            return low;   // Lệnh nén 16 bit: không đọc word sau (tránh fault giả ở cuối vùng nhớ)
        }
        return low | (fetch_word(base + 4, base + 4) << 16);
    }
};

struct CVA6_DataAdapter : public rv32::data_memory_if {
    tlm_utils::simple_initiator_socket<cva6_wrapper, 32>& d_socket;

    CVA6_DataAdapter(tlm_utils::simple_initiator_socket<cva6_wrapper, 32>& socket) 
        : d_socket(socket) {}

    void tlm_access(uint64_t addr, uint8_t* data, unsigned int len, tlm::tlm_command cmd) {
        /*
        // Trick: Chặn toàn bộ khu vực UART (Từ 0x10000000 đến 0x10000007)
        if (addr >= 0x10000000 && addr <= 0x10000007) {
            if (cmd == tlm::TLM_WRITE_COMMAND) {
                // Ghi vào thanh ghi TX -> In thẳng ra Terminal
                if (addr == 0x10000000) std::cout << (char)(data[0]) << std::flush;
            } else {
                // Đọc bất kỳ thanh ghi trạng thái nào -> Nhét mã 0x60 vào mọi byte
                for (unsigned int i = 0; i < len; i++) {
                    data[i] = 0x60;
                }
            }
            return; // Phản hồi ngay tắp lự, cách ly hoàn toàn khỏi Bus Router ảo
        } */
        
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME; 

        trans.set_command(cmd);
        trans.set_address(addr);
        trans.set_data_ptr(data);
        trans.set_data_length(len);
        trans.set_streaming_width(len);
        trans.set_byte_enable_ptr(0);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

        // 1 beat = len byte (1/2/4). 
        // Truy cập lệch hàng => bus trả ADDRESS_ERROR => access fault.
        fu3_bus::ScopedAccess access(trans, len, fu3_bus::Burst::Increment);
        d_socket->b_transport(trans, delay);

        if (trans.is_response_error()) {
            // throw std::runtime_error("Data Bus error tai dia chi: " + std::to_string(addr));
            // ISS chỉ bắt SimulationTrap, runtime_error làm dừng cả VP thay vì sinh trap.
            raise_trap(cmd == tlm::TLM_READ_COMMAND ? EXC_LOAD_ACCESS_FAULT        // mcause 5
                                                    : EXC_STORE_AMO_ACCESS_FAULT,  // mcause 7
                       addr);
        }
    }

    int64_t load_double(uint64_t addr) override {
        int64_t val = 0;
        tlm_access(addr, (uint8_t*)&val, 8, tlm::TLM_READ_COMMAND);
        return val;
    }
    int32_t load_word(uint64_t addr) override {
        int32_t val = 0;
        tlm_access(addr, (uint8_t*)&val, 4, tlm::TLM_READ_COMMAND);
        return val;
    }
    int32_t load_half(uint64_t addr) override {
        int16_t val = 0;
        tlm_access(addr, (uint8_t*)&val, 2, tlm::TLM_READ_COMMAND);
        return val; // Tự động Sign-extend lên 32-bit theo chuẩn C++
    }
    int32_t load_byte(uint64_t addr) override {
        int8_t val = 0;
        tlm_access(addr, (uint8_t*)&val, 1, tlm::TLM_READ_COMMAND);
        return val;
    }
    uint32_t load_uhalf(uint64_t addr) override {
        uint16_t val = 0;
        tlm_access(addr, (uint8_t*)&val, 2, tlm::TLM_READ_COMMAND);
        return val; // Tự động Zero-extend
    }
    uint32_t load_ubyte(uint64_t addr) override {
        uint8_t val = 0;
        tlm_access(addr, (uint8_t*)&val, 1, tlm::TLM_READ_COMMAND);
        return val;
    }

    void store_double(uint64_t addr, uint64_t value) override {
        tlm_access(addr, (uint8_t*)&value, 8, tlm::TLM_WRITE_COMMAND);
    }
    void store_word(uint64_t addr, uint32_t value) override {
        tlm_access(addr, (uint8_t*)&value, 4, tlm::TLM_WRITE_COMMAND);
    }
    void store_half(uint64_t addr, uint16_t value) override {
        tlm_access(addr, (uint8_t*)&value, 2, tlm::TLM_WRITE_COMMAND);
    }
    void store_byte(uint64_t addr, uint8_t value) override {
        tlm_access(addr, (uint8_t*)&value, 1, tlm::TLM_WRITE_COMMAND);
    }


    int32_t atomic_load_word(uint64_t addr) override {
        int32_t val = 0;
        tlm_access(addr, (uint8_t*)&val, 4, tlm::TLM_READ_COMMAND);
        return val;
    }
    void atomic_store_word(uint64_t addr, uint32_t value) override {
        tlm_access(addr, (uint8_t*)&value, 4, tlm::TLM_WRITE_COMMAND);
    }
    int32_t atomic_load_reserved_word(uint64_t addr) override {
        int32_t val = 0;
        tlm_access(addr, (uint8_t*)&val, 4, tlm::TLM_READ_COMMAND);
        return val;
    }
    bool atomic_store_conditional_word(uint64_t addr, uint32_t value) override {
        tlm_access(addr, (uint8_t*)&value, 4, tlm::TLM_WRITE_COMMAND);
        return true;    // VP chạy single-core nên SC luôn báo thành công
                        // FU3: RVA = 0 nên LR/SC/AMO báo illegal => hàm này không được gọi.
    }
    void atomic_unlock() override {}
    
    void flush_tlb() override {}
};

struct cva6_wrapper::impl {
    
    struct quantum_init {
        quantum_init() {
            auto& gq = tlm::tlm_global_quantum::instance();
            if (gq.get() < sc_core::sc_time(10, sc_core::SC_NS)) {
                gq.set(sc_core::sc_time(1, sc_core::SC_US));
            }
        }
    } quantum_init_;

    // tlm_utils::simple_initiator_socket<cva6_wrapper, 32> i_socket;
    // tlm_utils::simple_initiator_socket<cva6_wrapper, 32> d_socket;
    // FU3: 1 AXI4 master/CPU => Fetch và Data dùng chung 1 socket.
    tlm_utils::simple_initiator_socket<cva6_wrapper, 32> bus_socket;

    CVA6_InstrAdapter instr_adapter;
    CVA6_DataAdapter data_adapter;

    stub_clint clint;

    // Con trỏ chứa ISS
    std::unique_ptr<rv32::ISS> iss_core;

    /* impl(const cpu_config& config, const std::string& disable_extensions) : i_socket("i_socket"), 
                                                                            d_socket("d_socket"),
                                                                            instr_adapter(i_socket),  // i_socket cho Fetch
                                                                            data_adapter(d_socket)    // d_socket cho Data */
    impl(const cpu_config& config, const std::string& disable_extensions) : bus_socket("bus_socket"),
                                                                            instr_adapter(bus_socket),  // Fetch
                                                                            data_adapter(bus_socket)    // Data
    
    {
        // VP++ đòi hỏi truyền hart_id vào constructor
        // Truyền giá trị hart_id từ cfg vào, tham số thứ 2 (use_E_base_isa) để mặc định là false
        iss_core = std::make_unique<rv32::ISS>(config.hart_id, false);
        // ISS kiểm tra bit misa khi Decode => Extension bị bỏ sẽ báo Illegal Instruction.
        iss_core->csrs.misa.fields.extensions &= ~misa_mask_from_letters(disable_extensions);
    }
};

cva6_wrapper::cva6_wrapper(sc_core::sc_module_name name, const cpu_config& config,
                           const std::string& disable_extensions)
    : cpu_base(name, config), impl_(std::make_unique<impl>(config, disable_extensions)) {

    if (config.xlen != 32) {
        throw std::runtime_error("Invalid XLEN value. Must be 32.");
    } 
    /* if (!config.has_mmu) {
          throw std::runtime_error("Boot Linux requires MMU support.");
        } */ 
    //   FU3 có MmuPresent = 0 nên phải chạy được khi has_mmu = false
    //   Wrapper cũng chưa nối page-table walker => has_mmu = true trước đây chưa có tác dụng.

    SC_HAS_PROCESS(cva6_wrapper);
    SC_THREAD(run);
}

cva6_wrapper::~cva6_wrapper() = default;

// Gộp I/D: cả hai trả về cùng 1 socket (has_unified_bus() = true).
tlm::tlm_initiator_socket<>& cva6_wrapper::instr_bus() {
    // return impl_->i_socket;
    return impl_->bus_socket;
}

tlm::tlm_initiator_socket<>& cva6_wrapper::data_bus() {
    // return impl_->d_socket;
    return impl_->bus_socket;
}



void cva6_wrapper::set_irq(unsigned cause, bool level) {
    if (cause == 7) {
        impl_->iss_core->trigger_timer_interrupt(level);
    } else if (cause == 3) {
        impl_->iss_core->trigger_software_interrupt(level);
    } else if (cause == 11) {
        if (level) {
            impl_->iss_core->trigger_external_interrupt(MachineMode);
        } else {
            impl_->iss_core->clear_external_interrupt(MachineMode);
        }
    }
}

void cva6_wrapper::load_elf(const std::string& path) {
    // Luồng chạy BootROM: Chỉ ghi nhận đường dẫn, không ghi đè cfg.reset_pc bằng entry_pc của file ELF
    elf_path_ = path;
    
}

void cva6_wrapper::reset_cpu() {
    // uint64_t start_pc = cfg.reset_pc_specified() ? cfg.reset_pc : 0x80000000;
    // Dòng code trên chỉ đúng khi start_of_simulation ghi đè cfg.reset_pc (C8). Do nay không ghi đè nữa => Không dùng
    uint64_t start_pc = cfg.reset_pc_specified() ? cfg.reset_pc
                      : (elf_loaded_ ? elf_entry_pc_ : 0x80000000);   

    // std::cout << "[CVA6] reset PC = 0x" << std::hex << start_pc << std::dec << std::endl;
    // Ibex dùng chung wrapper => In tên instance để log 2 CPU không lẫn nhau.
    std::cout << "[" << name() << "] reset PC = 0x" << std::hex << start_pc << std::dec << std::endl;
    
    impl_->iss_core->init(
        &(impl_->instr_adapter),
        &(impl_->data_adapter),
        &(impl_->clint),
        start_pc,
        0x80010000  // Spec FU3 không định nghĩa sp lúc reset (CPU thật không khởi tạo x2).
                    // FW FU3 phải tự đặt sp trong crt0 (fu3_cva6_smoke làm: la sp, _stack_top).
    );
}

std::uint64_t cva6_wrapper::get_pc() const {
    return impl_->iss_core->pc;
}

std::string cva6_wrapper::backend_name() const {
    return "CVA6";
}

// Số lệnh đã thực hiện (để đo hiệu năng). 0 nếu backend không thể báo cáo.
std::uint64_t cva6_wrapper::get_instret() const {
    return impl_->iss_core->total_num_instr; 
}

void cva6_wrapper::start_of_simulation() {
    std::cout << "CVA6 Wrapper: Starting simulation..." << std::endl;
    /* if (elf_path_.empty()) {
        // elf_path_ = "../fw/hello_baremetal_riscv/hello.elf";
        // elf_path_ = "../fw/uart_test_riscv/uart_test.elf";
        // elf_path_ = "..fw/timer_irq_riscv/timer_irq.elf";
        
    } */
    
    if (!elf_path_.empty()) {
        std::cout << "Loading Firmware from: " << elf_path_ << std::endl;
        // Lúc này cáp Bus đã nối xong, ta mới bơm dữ liệu và lấy entry_pc chuẩn
        // cfg.reset_pc = cdc::cpu::load_elf(impl_->d_socket, elf_path_);
        // Dòng trên không dùng vì ghi đè vector reset do platform đặt (FU3 boot_addr_i = 0x0001_0000)
        elf_entry_pc_ = cdc::cpu::load_elf(impl_->bus_socket, elf_path_);
        elf_loaded_ = true;
    }
    // Gọi reset 1 lần duy nhất ở đây
    reset_cpu();
    initialised_ = true;
}

void cva6_wrapper::set_reset(bool asserted) {
    if (asserted) {
        in_reset_ = true;             // CPU dừng ở ranh giới vòng 1000 lệnh kế tiếp
        return;
    }
    if (!in_reset_) {
        return;
    }
    if (initialised_) {               // Trước start_of_simulation thì để nó tự reset sau khi nạp ELF
        reset_cpu();
    }
    in_reset_ = false;
    if (sc_core::sc_is_running()) {
        reset_released_.notify(sc_core::SC_ZERO_TIME);
    }
}


void cva6_wrapper::run() {
    sc_core::wait(sc_core::SC_ZERO_TIME); 
    while (true) {
        if (in_reset_) {                      // FU3: chờ Ibex nhả reset 
            sc_core::wait(reset_released_);
            continue;
        }
        for (int i = 0; i < 1000 && !in_reset_; i++) {
            impl_->iss_core->run_step();
        }
        sc_core::wait(10, sc_core::SC_US); 
    }
}

}  // namespace cdc::cpu
