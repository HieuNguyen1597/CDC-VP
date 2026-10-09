/*
 * Fault-tolerant 32-bit accesses for the CLI (md, mw, regdump).
 *
 *   int fx1_probe_read32(uint32_t addr, uint32_t* value)   0 ok, 1 bus fault
 *   int fx1_probe_write32(uint32_t addr, uint32_t value)   0 ok, 1 bus fault
 *
 * A mapped region can still hold addresses that the device rejects (CLINT
 * and PLIC register holes answer with an access fault). The access is the
 * one instruction at fx1_probe_load_insn / fx1_probe_store_insn, assembled
 * without RVC: when it faults, the application exception handler
 * (rtos_bsp.c) recognises its mepc, sets fx1_probe_fault and returns. The
 * port's exception entry already resumes at mepc + 4 (portContext.h), which
 * is the next instruction here because this one is 4 bytes long.
 */
    .section .text.fx1_probe, "ax"
    .option push
    .option norvc

    .global fx1_probe_read32
    .type fx1_probe_read32, @function
fx1_probe_read32:
    la t0, fx1_probe_fault
    sw zero, 0(t0)
    .global fx1_probe_load_insn
fx1_probe_load_insn:
    lw t1, 0(a0)
    lw a0, 0(t0)
    bnez a0, 1f
    sw t1, 0(a1)
1:  ret
    .size fx1_probe_read32, . - fx1_probe_read32

    .global fx1_probe_write32
    .type fx1_probe_write32, @function
fx1_probe_write32:
    la t0, fx1_probe_fault
    sw zero, 0(t0)
    .global fx1_probe_store_insn
fx1_probe_store_insn:
    sw a1, 0(a0)
    lw a0, 0(t0)
    ret
    .size fx1_probe_write32, . - fx1_probe_write32

    .option pop

    .section .bss.fx1_probe, "aw", @nobits
    .balign 4
    .global fx1_probe_fault
fx1_probe_fault:
    .word 0
