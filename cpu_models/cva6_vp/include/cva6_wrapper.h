#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <cdc/cpu/cpu_base.h>

namespace cdc::cpu {


class cva6_wrapper : public cpu_base {
public:
    // FU3 (Bảng 8-1): truyền AFNSU để misa = RV32IMC (chỉ M-mode)
    // Mặc định rỗng = giữ misa IMACF + NUS 
    explicit cva6_wrapper(sc_core::sc_module_name name, const cpu_config& config = cpu_config{},
                          const std::string& disable_extensions = "");
    ~cva6_wrapper() override;

    // 2 cổng AXI riêng biệt cho lệnh và dữ liệu
    tlm::tlm_initiator_socket<>& instr_bus() override;
    tlm::tlm_initiator_socket<>& data_bus() override;

    // Hệ thống không xài bus gộp vì 2 cổng AXI riêng biệt
    bool has_unified_bus() const override { return false; }

    // Nhận ngắt từ bộ điều khiển ngắt (CLINT/PLIC - Soft, Timer, External)
    // Đẩy vào MIP register của CVA6
    /* FU3 (Bảng 11-2): Case 3 = ipi_i (SYSCSR), 
                        Case 7 = time_irq_i (TIMER0), 
                        Case 11 = irq_i[0] (PLIC target 0) */

    void set_irq(unsigned cause, bool level) override;


    void load_elf(const std::string& path) override;
    void reset_cpu() override;

    // Chân reset của CVA6 (rst_ni): true = giữ reset, false = nhả và chạy từ reset PC.
    // FU3: gọi set_reset(true) ngay sau khi tạo và Ibex handoff gọi set_reset(false).
    void set_reset(bool asserted);
    bool in_reset() const noexcept { return in_reset_; }
    
    std::uint64_t get_pc() const override;
    std::string backend_name() const override;
    
    // Số lệnh đã thực hiện => 0 nếu Backend không thể báo cáo.
    std::uint64_t get_instret() const override;

private:
    
    // Hàm này tự động chạy ngay trước khi thời gian mô phỏng bắt đầu (t = 0).
    // Chỉ dùng để nạp file ELF và reset PC sau khi các dây nối Bus đã liên kết xong.
    void start_of_simulation() override;

    // Gom ISS + bộ nhớ + runner để cho phép triển khai riêng tư, tránh đưa các header của CVA6 vào giao diện công khai.
    struct impl;               
    std::unique_ptr<impl> impl_;
    std::string elf_path_;
    // std::uint64_t entry_pc_ = 0;   // Chưa từng được đọc/ghi ở bản gốc do entry của ELF lưu ở elf_entry_pc_
    std::uint64_t elf_entry_pc_ = 0;  // Lưu entry của ELF và xem C7/C8
    bool elf_loaded_ = false;
    bool initialised_ = false;
    bool in_reset_ = false;
    sc_core::sc_event reset_released_;

    void run(); 
};

}  // namespace cdc::cpu