/*
 * FX1 SoC linker script for BootROM images (DDR3 VP subtask 5, firmware boot).
 * Preprocessed with -DFX1_LINKER_SCRIPT. Code, read-only data, the initial
 * .data image and an optional embedded application (.app_image) are in
 * BootROM; .data, .bss and the per-hart stacks are in the boot SRAM. Nothing
 * is placed in DDR: the DDR is not usable until firmware initialized it, and
 * the ECC startup scrub zeroes it. The MEMORY lengths make an image that does
 * not fit BootROM (2 MiB) or SRAM (1 MiB) a link error.
 */
#include "fx1/fx1_memory_map.h"

#define FX1_ROM_STACK_SIZE 0x4000 /* 16 KiB per hart */

OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    ROM  (rx)  : ORIGIN = FX1_BOOTROM_BASE, LENGTH = FX1_BOOTROM_SIZE
    SRAM (rw)  : ORIGIN = FX1_SRAM_BASE,    LENGTH = FX1_SRAM_SIZE
}

__rom_stack_size = FX1_ROM_STACK_SIZE;

SECTIONS
{
    .text : {
        KEEP(*(.text.init))
        *(.text .text.*)
    } > ROM

    .rodata : ALIGN(8) {
        *(.rodata .rodata.* .srodata .srodata.*)
    } > ROM

    .app_image : ALIGN(8) {
        KEEP(*(.app_image))
    } > ROM

    .data : ALIGN(8) {
        __data_start = .;
        *(.data .data.*)
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
    } > SRAM AT > ROM
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SRAM

    .stack (NOLOAD) : ALIGN(16) {
        __stack_base = .;
        . += FX1_ROM_STACK_SIZE * FX1_NUM_HARTS;
        __stack_end = .;
    } > SRAM

    /DISCARD/ : { *(.note* .comment .riscv.attributes) }
}

ASSERT(__stack_end <= FX1_SRAM_BASE + FX1_SRAM_SIZE, "fx1 rom: SRAM overflow")
