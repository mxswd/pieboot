# SPDX-License-Identifier: BSD-2-Clause
# pieboot: boot shim that starts xnu from Linux-style arm64 boot firmware
# (QEMU -kernel, U-Boot booti, Raspberry Pi firmware kernel8.img).

CC      := clang-20
LD      := ld.lld-20
OBJCOPY := llvm-objcopy-20

CFLAGS  := --target=aarch64-none-elf -mcpu=cortex-a53+nocrypto -mgeneral-regs-only \
           -ffreestanding -fno-builtin -nostdlib -fPIE -fvisibility=hidden \
           -fno-stack-protector -fno-common -O2 -g -std=c11 \
           -Wall -Wextra -Werror -Wno-unused-parameter
ASFLAGS := --target=aarch64-none-elf -mcpu=cortex-a53+nocrypto -fPIE -g
LDFLAGS := -static -pie --no-dynamic-linker -z norelro -z noexecstack -T link.ld

OBJS := start.o main.o fdt.o adt.o macho.o lib.o uart.o psci.o mbox.o fbcon.o font.o pie.o led.o place.o

all: pieboot.bin

pieboot.elf: $(OBJS) link.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

pieboot.bin: pieboot.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c *.h
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

# place.c's placement of the kernel, run on the build host.
HOSTCC  ?= cc
test: place-test
	./place-test

place-test: place-test.c place.c place.h
	$(HOSTCC) -O2 -std=c11 -Wall -Wextra -Werror -o $@ place-test.c place.c

clean:
	rm -f *.o pieboot.elf pieboot.bin place-test

.PHONY: all clean test
