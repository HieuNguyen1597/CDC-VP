OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    /* 64 KiB CVA6_ISRAM, 4 KiB cuối (0x0001_F000) dành cho debug code */
    ISRAM (rx) : ORIGIN = 0x00010000, LENGTH = 60K

    /* 64 KiB DSRAM dùng chung với Ibex, non-executable */
    DSRAM (rw) : ORIGIN = 0x01000000, LENGTH = 64K
}

SECTIONS
{
    .text : {
        KEEP(*(.text.init))          /* _start đặt đúng tại reset PC 0x0001_0000 */
        *(.text .text.*)
    } > ISRAM

    .rodata : ALIGN(4) {
        *(.rodata .rodata.* .srodata .srodata.*)
    } > ISRAM

    /* Khối kết quả ở đầu DSRAM (0x0100_0000) để platform/test đọc theo địa chỉ cố định */
    .results (NOLOAD) : {
        KEEP(*(.results))
    } > DSRAM

    /* Image nằm trong ISRAM vì Ibex chỉ nạp CVA6_ISRAM và startup chép sang DSRAM */
    .data : ALIGN(4) {
        _data_start = .;
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(4);
        _data_end = .;
    } > DSRAM AT > ISRAM
    _data_load = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > DSRAM

    _stack_top = ORIGIN(DSRAM) + LENGTH(DSRAM);
    ASSERT(_bss_end + 1K <= _stack_top, "DSRAM: not enough room for the stack")
}
