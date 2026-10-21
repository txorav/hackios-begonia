# HackiOS-begonia — top level
#
# Builds (in dependency order):
#   1. xnuloader     (bare-metal ELF->BIN, needs aarch64 cross gcc/clang)
#   2. kernelcache   (XNU 8796 MT6785 config; see xnu/config + xnu-patches/)
#   3. platform kexts (kexts/, built against xnu headers via xnu-deps)
#   4. ramdisk       (userland/ramdisk)
#   5. boot.img      (mkxnuimg.py: loader+ADT+kernelcache+ramdisk, gz, v2)
#   6. `xl boot out/boot.img` (M1)

SHELL      := /bin/bash
PYTHON     ?= python3
BUILD      := build
OUT        := out

_arch_cc := $(shell PATH="$(PATH):/media/dev/work/tcbin" which aarch64-none-elf-gcc aarch64-linux-gnu-gcc aarch64-linux-android-gcc 2>/dev/null | head -1)
cross_check:
ifeq ($(strip $(_arch_cc)),)
	@echo ""
	@echo "No aarch64 cross gcc found on PATH (aarch64-none-elf / aarch64-linux-gnu / aarch64-linux-android)."
	@echo "Install with scripts/setup_host.sh or tools/install_llvm_toolchain.sh"
	@echo ""
	@exit 1
endif

.PHONY: all cross_check xnuloader kexts kernel ramdisk boot.img payload tests qemu clean init
.NOTPARALLEL:

all: xnuloader boot.img

.init:
	@mkdir -p $(BUILD) $(OUT)

# ---- 1. stage-2 loader -------------------------------------------------
xnuloader: .init cross_check
	@echo "== [1/6] xnuloader =="
	$(MAKE) -C xnuloader
	@cp xnuloader/xnuloader.bin $(OUT)/xnuloader.bin
	@cp xnuloader/xnuloader.elf $(BUILD)/xnuloader.elf 2>/dev/null || true

# ---- 2. XNU kernel (MT6785) --------------------------------------------
kernel:
	@echo "== [2/6] XNU (MT6785) — see xnu-patches/0000-overview.md =="
	@echo "Build with:"
	@echo "  $(MAKE) -C xnu MT6785_BUILD=1 2>&1 | tee $(BUILD)/xnu-build.log"
	@echo "kernld artifact: $(BUILD)/kernelcache.macho"

# ---- 3. platform kexts --------------------------------------------------
kexts: cross_check
	@echo "== [3/6] platform kexts (GICv3/expert/stubs) =="
	$(MAKE) -C kexts -f build.mk build || true
	@echo "(see kexts/README for per-kext build notes)"

# ---- 4. userland ramdisk (M4) -------------------------------------------
ramdisk:
	@echo "== [4/6] ramdisk =="
	bash userland/ramdisk/mkramdisk.sh --out $(OUT)/ramdisk.img --no-shell || true

# ---- 5. payload + boot.img ----------------------------------------------
payload: xnuloader
	$(PYTHON) mkxnuimg.py --out $(OUT)/boot.img.tmp --loader $(OUT)/xnuloader.bin \
	    --kernel $(BUILD)/kernelcache.macho --adt $(BUILD)/adt.bin \
	    --ramdisk $(OUT)/ramdisk.img 2>/dev/null || \
	$(PYTHON) mkxnuimg.py --out $(OUT)/boot.img.tmp --loader $(OUT)/xnuloader.bin \
	    --kernel $(BUILD)/kernelcache.macho

boot.img: .init
	@echo "== [5/6] boot.img (payload requires kernel artifacts; see docs) =="
	@if [ -f $(BUILD)/kernelcache.macho ]; then \
	  $(PYTHON) mkxnuimg.py --out $(OUT)/boot.img \
	     --loader $(OUT)/xnuloader.bin \
	     --kernel $(BUILD)/kernelcache.macho \
	     --ramdisk $(OUT)/ramdisk.img; \
	else \
	  echo "kernelcache missing — run the kernel build first (see kernel target)."; \
	  echo "M1-only loader test image instead:"; \
	  $(PYTHON) tools/mk_placeholder_kernel.py $(BUILD)/placeholder.macho; \
	  $(PYTHON) mkxnuimg.py --out $(OUT)/boot.img.m1 \
	     --loader $(OUT)/xnuloader.bin --kernel $(BUILD)/placeholder.macho; \
	fi

# ---- 5b. host unit tests & begonia contract evaluation ------------------
tests: .init
	@echo "== [2f/6] host unit tests (loader FDT/ADT logic) =="
	@cd xnuloader && \
	  gcc -std=gnu11 -O1 -Wall -o $(CURDIR)/build/xldr_test_host \
	      test_host.c adt.c fdt.c string.c && \
	  $(CURDIR)/build/xldr_test_host
	@echo "== begonia hardware contract & boot image evaluation =="
	$(PYTHON) -m unittest -v tests/test_begonia_contract.py

# ---- QEMU rehearsal (guide Phase A) ---------------------------------------
qemu:
	@echo "== QEMU bring-up: loader stage (qemu/README.md) =="
	bash qemu/run_qemu.sh

# ---- helpers -------------------------------------------------------------
syscheck:
	$(PYTHON) tools/check_env.py || bash tools/check_env.sh

clean:
	rm -rf $(BUILD) $(OUT)
	$(MAKE) -C xnuloader clean || true
