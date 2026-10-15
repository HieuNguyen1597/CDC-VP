/*
 * Fault-safe 32-bit accesses for the DDR3 FW helpers (ddr3_fw.h).
 *
 *   uint32_t ddr3_probe_read32(uintptr_t addr, uint32_t* value)
 *   uint32_t ddr3_probe_write32(uintptr_t addr, uint32_t value)
 *
 * Return 0, or the mcause of the access fault. The access is the single 4-byte
 * instruction at ddr3_probe_load_insn / ddr3_probe_store_insn (no RVC). When it
 * faults, ddr3_probe_trap() recognises its mepc, stores mcause in this hart's
 * ddr3_probe_cause slot and resumes at mepc + 4. t0..t2 survive the trap
 * (fx1_trap_entry saves the caller-saved registers).
 */
    .section .text.ddr3_probe, "ax"
    .option push
    .option norvc

    .global ddr3_probe_read32
    .type ddr3_probe_read32, @function
ddr3_probe_read32:
    csrr t2, mhartid
    slli t2, t2, 2
    la   t0, ddr3_probe_cause
    add  t0, t0, t2
    sw   zero, 0(t0)
    .global ddr3_probe_load_insn
ddr3_probe_load_insn:
    lw   t1, 0(a0)
    lw   a0, 0(t0)
    bnez a0, 1f
    sw   t1, 0(a1)
1:  ret
    .size ddr3_probe_read32, . - ddr3_probe_read32

    .global ddr3_probe_write32
    .type ddr3_probe_write32, @function
ddr3_probe_write32:
    csrr t2, mhartid
    slli t2, t2, 2
    la   t0, ddr3_probe_cause
    add  t0, t0, t2
    sw   zero, 0(t0)
    .global ddr3_probe_store_insn
ddr3_probe_store_insn:
    sw   a1, 0(a0)
    lw   a0, 0(t0)
    ret
    .size ddr3_probe_write32, . - ddr3_probe_write32

    .option pop
