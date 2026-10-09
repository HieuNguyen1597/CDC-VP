/*
 * FreeRTOS RISC-V chip extension for the FX1 harts (RV32IMAFC, ilp32f).
 *
 * The stock port saves only the integer registers, mepc, mstatus and the
 * critical nesting count. The FX1 CPU has the F extension and the firmware is
 * built with -mabi=ilp32f, so every task (and every ISR) may hold live values
 * in f0..f31 and fcsr: they are saved here on every trap entry and restored
 * on every exit, through the port's portasmSAVE/RESTORE_ADDITIONAL_REGISTERS
 * hooks.
 *
 * Frame layout of the additional area (portContext.h): after the hook the
 * port stores mepc at 0(sp), so word 0 is reserved; the registers go to
 * words 1..33 (f0..f31, fcsr). portasmADDITIONAL_CONTEXT_SIZE must be even on
 * a 32-bit core, hence 34 (word 34 is unused padding; it overlaps the base
 * frame's unused word 0, exactly as pxPortInitialiseStack lays it out, which
 * also zero-initialises these words for a new task: FP registers 0, fcsr 0).
 *
 * mstatus.FS: crt0 sets it to Initial and nothing turns it off, so the
 * fsw/flw here never trap; each task's saved mstatus keeps its FS state.
 */
#ifndef __FREERTOS_RISC_V_EXTENSIONS_H__
#define __FREERTOS_RISC_V_EXTENSIONS_H__

#define portasmHAS_SIFIVE_CLINT        1
#define portasmHAS_MTIME               1
#define portasmADDITIONAL_CONTEXT_SIZE 34

.macro portasmSAVE_ADDITIONAL_REGISTERS
    addi sp, sp, -( portasmADDITIONAL_CONTEXT_SIZE * portWORD_SIZE )
    fsw  f0,   1 * portWORD_SIZE( sp )
    fsw  f1,   2 * portWORD_SIZE( sp )
    fsw  f2,   3 * portWORD_SIZE( sp )
    fsw  f3,   4 * portWORD_SIZE( sp )
    fsw  f4,   5 * portWORD_SIZE( sp )
    fsw  f5,   6 * portWORD_SIZE( sp )
    fsw  f6,   7 * portWORD_SIZE( sp )
    fsw  f7,   8 * portWORD_SIZE( sp )
    fsw  f8,   9 * portWORD_SIZE( sp )
    fsw  f9,  10 * portWORD_SIZE( sp )
    fsw  f10, 11 * portWORD_SIZE( sp )
    fsw  f11, 12 * portWORD_SIZE( sp )
    fsw  f12, 13 * portWORD_SIZE( sp )
    fsw  f13, 14 * portWORD_SIZE( sp )
    fsw  f14, 15 * portWORD_SIZE( sp )
    fsw  f15, 16 * portWORD_SIZE( sp )
    fsw  f16, 17 * portWORD_SIZE( sp )
    fsw  f17, 18 * portWORD_SIZE( sp )
    fsw  f18, 19 * portWORD_SIZE( sp )
    fsw  f19, 20 * portWORD_SIZE( sp )
    fsw  f20, 21 * portWORD_SIZE( sp )
    fsw  f21, 22 * portWORD_SIZE( sp )
    fsw  f22, 23 * portWORD_SIZE( sp )
    fsw  f23, 24 * portWORD_SIZE( sp )
    fsw  f24, 25 * portWORD_SIZE( sp )
    fsw  f25, 26 * portWORD_SIZE( sp )
    fsw  f26, 27 * portWORD_SIZE( sp )
    fsw  f27, 28 * portWORD_SIZE( sp )
    fsw  f28, 29 * portWORD_SIZE( sp )
    fsw  f29, 30 * portWORD_SIZE( sp )
    fsw  f30, 31 * portWORD_SIZE( sp )
    fsw  f31, 32 * portWORD_SIZE( sp )
    frcsr t0
    sw   t0,  33 * portWORD_SIZE( sp )
.endm

.macro portasmRESTORE_ADDITIONAL_REGISTERS
    lw   t0,  33 * portWORD_SIZE( sp )
    fscsr t0
    flw  f0,   1 * portWORD_SIZE( sp )
    flw  f1,   2 * portWORD_SIZE( sp )
    flw  f2,   3 * portWORD_SIZE( sp )
    flw  f3,   4 * portWORD_SIZE( sp )
    flw  f4,   5 * portWORD_SIZE( sp )
    flw  f5,   6 * portWORD_SIZE( sp )
    flw  f6,   7 * portWORD_SIZE( sp )
    flw  f7,   8 * portWORD_SIZE( sp )
    flw  f8,   9 * portWORD_SIZE( sp )
    flw  f9,  10 * portWORD_SIZE( sp )
    flw  f10, 11 * portWORD_SIZE( sp )
    flw  f11, 12 * portWORD_SIZE( sp )
    flw  f12, 13 * portWORD_SIZE( sp )
    flw  f13, 14 * portWORD_SIZE( sp )
    flw  f14, 15 * portWORD_SIZE( sp )
    flw  f15, 16 * portWORD_SIZE( sp )
    flw  f16, 17 * portWORD_SIZE( sp )
    flw  f17, 18 * portWORD_SIZE( sp )
    flw  f18, 19 * portWORD_SIZE( sp )
    flw  f19, 20 * portWORD_SIZE( sp )
    flw  f20, 21 * portWORD_SIZE( sp )
    flw  f21, 22 * portWORD_SIZE( sp )
    flw  f22, 23 * portWORD_SIZE( sp )
    flw  f23, 24 * portWORD_SIZE( sp )
    flw  f24, 25 * portWORD_SIZE( sp )
    flw  f25, 26 * portWORD_SIZE( sp )
    flw  f26, 27 * portWORD_SIZE( sp )
    flw  f27, 28 * portWORD_SIZE( sp )
    flw  f28, 29 * portWORD_SIZE( sp )
    flw  f29, 30 * portWORD_SIZE( sp )
    flw  f30, 31 * portWORD_SIZE( sp )
    flw  f31, 32 * portWORD_SIZE( sp )
    addi sp, sp, ( portasmADDITIONAL_CONTEXT_SIZE * portWORD_SIZE )
.endm

#endif /* __FREERTOS_RISC_V_EXTENSIONS_H__ */
