CLANG_BIN       ?= /mnt/9f4d8733-a954-435d-b92f-f33c068c3b1e/begoker/kerdevdep/clang/bin
GCC_BIN         ?= /mnt/9f4d8733-a954-435d-b92f-f33c068c3b1e/begoker/kerdevdep/gcc/bin
CC              = $(CLANG_BIN)/clang -target aarch64-linux-gnu
OBJCOPY         = $(GCC_BIN)/aarch64-linux-android-objcopy

all: BootShim.elf BootShim.bin 

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) -c $< -o $@ -DUEFI_BASE=$(UEFI_BASE) -DUEFI_SIZE=$(UEFI_SIZE)

BootShim.S:

clean:
	rm -f BootShim.elf BootShim.bin