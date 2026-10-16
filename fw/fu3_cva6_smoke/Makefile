# Cross-compile FU3 CVA6 smoke firmware (CVA6_ISRAM 0x0001_0000, DSRAM 0x0100_0000).
# Mặc định theo convention của repo (riscv-none-elf-). 
# Máy chỉ có riscv64-unknown-elf thì: make CROSS=riscv64-unknown-elf-

CROSS   ?= riscv-none-elf-
CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump

# ISA mà VP mô hình được: RV32IMC (spec có RVB=1 nhưng ISS Bremen chưa có Zb*).
ARCH    := -march=rv32imc_zicsr -mabi=ilp32
CFLAGS  := $(ARCH) -O2 -g -ffreestanding -nostdlib -nostartfiles -fno-pic \
           -msmall-data-limit=0 -Wall -Wextra -Isrc
LDFLAGS := -T linker.ld -Wl,--gc-sections

SRCS    := startup.S src/main.c
TARGET  := fu3_cva6_smoke.elf

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@
	$(OBJDUMP) -d $@ > fu3_cva6_smoke.dis

clean:
	rm -f $(TARGET) fu3_cva6_smoke.dis

.PHONY: all clean
