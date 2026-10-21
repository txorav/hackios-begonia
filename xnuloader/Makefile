# xnuloader — stage-2 bare-metal loader build (Linux host, aarch64 cross)
#
# Output: xnuloader.bin (raw, contains the 64-byte boot header + PI code).
# mkxnuimg.py appends the payload manifest + ADT + kernelcache + ramdisk.
#
# Toolchain detection: aarch64-none-elf-gcc > aarch64-linux-gnu-gcc > clang
# (clang needs the -target switch and a bare-metal sysroot, see tools/).
#
# QEMU bring-up variant (qemu/run_qemu.sh):
#   make -C xnuloader clean
#   make -C xnuloader LINK_SCRIPT=linker_qemu.ld CFLAGS_EXTRA='-DXLDR_UART_PL011' all
#   cp xnuloader/xnuloader.elf ../build/xnuloader_qemu.elf
CROSS_CC := $(shell which aarch64-none-elf-gcc 2>/dev/null)
CROSS_GNU := $(shell which aarch64-linux-gnu-gcc 2>/dev/null)
CROSS_ANDROID := $(shell PATH="$(PATH):/media/dev/work/tcbin" which aarch64-linux-android-gcc 2>/dev/null)
CLANG := $(shell which clang 2>/dev/null)

ifdef CROSS_CC
    CC      := $(CROSS_CC)
    CC_TYPE := gcc-none-elf
else ifdef CROSS_GNU
    CC      := $(CROSS_GNU)
    CC_TYPE := gcc-linux-gnu
else ifdef CROSS_ANDROID
    CC      := $(CROSS_ANDROID)
    CC_TYPE := gcc-android
else ifdef CLANG
    CC      := $(CLANG)
    CC_TYPE := clang
    CCFLAGS_TARGET := --target=aarch64-none-elf -fuse-ld=lld -nostdlib
    LDRAW := $(CLANG)
else
$(error install a cross toolchain: see tools/README.md)
endif

ifeq ($(CC_TYPE),gcc-none-elf)
    OBJCOPY := aarch64-none-elf-objcopy
else ifeq ($(CC_TYPE),gcc-linux-gnu)
    OBJCOPY := aarch64-linux-gnu-objcopy
else ifeq ($(CC_TYPE),gcc-android)
    OBJCOPY := $(shell PATH="$(PATH):/media/dev/work/tcbin" which aarch64-linux-android-objcopy 2>/dev/null)
else
    OBJCOPY := objcopy
endif

CFLAGS_EXTRA ?=
LINK_SCRIPT  ?= linker.ld

OBJS := entry.o console.o fdt.o adt.o macho.o psci.o main.o string.o

ARCHFLAGS := -std=gnu11 -Wall -Wextra -ffreestanding -nostdlib -nostdinc \
	     -O2 -g -march=armv8-a -mgeneral-regs-only -mstrict-align \
	     -I./include -Wno-unused-function -Wno-unused-parameter \
	     -Wno-missing-prototypes

all: xnuloader.bin

%.o: %.c $(wildcard *.h) $(wildcard include/*.h) payload.h
	$(CC) $(CCFLAGS_TARGET) $(ARCHFLAGS) $(CFLAGS_EXTRA) -c $< -o $@

%.o: %.S
	$(CC) $(CCFLAGS_TARGET) $(ARCHFLAGS) $(CFLAGS_EXTRA) -c $< -o $@

xnuloader.elf: $(OBJS) $(LINK_SCRIPT)
	$(CC) $(CCFLAGS_TARGET) -ffreestanding -nostdlib \
	      -Wl,-T,$(LINK_SCRIPT) \
	      -Wl,--build-id=none -o $@ $(OBJS)

xnuloader.bin: xnuloader.elf
	$(OBJCOPY) -O binary $< $@
	$(OBJCOPY) -R .comment -R .note.ABI-tag -R .eh_frame $< 2>/dev/null || true
	@echo "xnuloader.bin built ($(words $(OBJS)) objects, $(CC_TYPE))"

clean:
	rm -f *.o *.elf *.bin

install-to-img: xnuloader.bin
	@echo "run:  python3 ../mkxnuimg.py --kernel ../build/kernelcache.macho --loader xnuloader.bin ..."
.PHONY: all clean install-to-img
