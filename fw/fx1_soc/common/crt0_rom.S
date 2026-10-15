/*
 * FX1 SoC start-up for images that run from BootROM before the DDR is usable
 * (DDR3 VP subtask 5, --ddr-boot firmware). Linked with fx1_rom.ld: code and
 * read-only data in BootROM, .data/.bss/stacks in the boot SRAM. Nothing here
 * touches DDR.
 *
 * All harts reset at _start (BootROM base). Hart 0 copies .data from its
 * BootROM load address to SRAM, zeroes .bss and runs main(); its return value
 * goes to fx1_exit(). Every other hart parks in WFI with only MSIE enabled. It
 * leaves the loop only after its MSIP is pending AND fx1_secondary_release is
 * set: SRAM holds no defined value before hart 0 copies .data, so the flag
 * alone is not trusted (fx1_release_secondaries sets the flag before the IPI).
 * The released hart fences (acquire), clears its own MSIP and calls
 * secondary_main(hartid). Each hart has its own FX1_ROM_STACK_SIZE stack.
 */
#include "fx1/fx1_memory_map.h"

#define MIE_MSIE   (1 << 3)
#define MIP_MSIP   (1 << 3)
#define MSTATUS_FS_INITIAL (1 << 13)

    .section .text.init, "ax"
    .globl  _start
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    csrw    mie, zero
    csrr    t0, mhartid
    li      t1, FX1_NUM_HARTS
    bgeu    t0, t1, .Lpark

    /* sp = __stack_base + (hartid + 1) * __rom_stack_size */
    la      sp, __stack_base
    addi    t2, t0, 1
    la      t1, __rom_stack_size
    mul     t2, t2, t1
    add     sp, sp, t2

    la      t1, fx1_trap_entry
    csrw    mtvec, t1
    li      t1, MSTATUS_FS_INITIAL
    csrs    mstatus, t1
    csrw    fcsr, zero
    bnez    t0, .Lsecondary

    /* .data: BootROM -> SRAM */
    la      a0, __data_start
    la      a1, __data_end
    la      a2, __data_load
1:  bgeu    a0, a1, 2f
    lw      t1, 0(a2)
    sw      t1, 0(a0)
    addi    a0, a0, 4
    addi    a2, a2, 4
    j       1b
2:  la      a0, __bss_start
    la      a1, __bss_end
3:  bgeu    a0, a1, 4f
    sw      zero, 0(a0)
    addi    a0, a0, 4
    j       3b
4:  call    main
    call    fx1_exit

.Lsecondary:
    li      t1, MIE_MSIE
    csrs    mie, t1
5:  wfi
    csrr    t1, mip
    andi    t1, t1, MIP_MSIP
    beqz    t1, 5b
    la      t2, fx1_secondary_release
    lw      t1, 0(t2)
    beqz    t1, 5b
    /* acquire: nothing hart 0 published moves above the flag read */
    fence   iorw, iorw
    /* acknowledge the release IPI: msip[hartid] = 0 */
    li      t1, FX1_CLINT_BASE + FX1_CLINT_MSIP_OFF
    slli    t2, t0, 2
    add     t1, t1, t2
    sw      zero, 0(t1)
    li      t1, MIE_MSIE
    csrc    mie, t1
    mv      a0, t0
    call    secondary_main
.Lpark:
    wfi
    j       .Lpark

/*
 * Machine trap entry (same contract as crt0.S): saves the caller-saved
 * integer registers, calls fx1_trap_dispatch(mcause, mepc, mtval) and resumes
 * at the mepc it returns. FP registers are not saved.
 */
    .text
    .align  2
    .globl  fx1_trap_entry
fx1_trap_entry:
    addi    sp, sp, -64
    sw      ra,  0(sp)
    sw      t0,  4(sp)
    sw      t1,  8(sp)
    sw      t2, 12(sp)
    sw      a0, 16(sp)
    sw      a1, 20(sp)
    sw      a2, 24(sp)
    sw      a3, 28(sp)
    sw      a4, 32(sp)
    sw      a5, 36(sp)
    sw      a6, 40(sp)
    sw      a7, 44(sp)
    sw      t3, 48(sp)
    sw      t4, 52(sp)
    sw      t5, 56(sp)
    sw      t6, 60(sp)
    csrr    a0, mcause
    csrr    a1, mepc
    csrr    a2, mtval
    call    fx1_trap_dispatch
    csrw    mepc, a0
    lw      ra,  0(sp)
    lw      t0,  4(sp)
    lw      t1,  8(sp)
    lw      t2, 12(sp)
    lw      a0, 16(sp)
    lw      a1, 20(sp)
    lw      a2, 24(sp)
    lw      a3, 28(sp)
    lw      a4, 32(sp)
    lw      a5, 36(sp)
    lw      a6, 40(sp)
    lw      a7, 44(sp)
    lw      t3, 48(sp)
    lw      t4, 52(sp)
    lw      t5, 56(sp)
    lw      t6, 60(sp)
    addi    sp, sp, 64
    mret
