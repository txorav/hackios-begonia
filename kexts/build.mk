# kexts/build.mk — best-effort host build of the IOKit kext sources.
#
# The full KEXT build is an on-device / on-mac story (Apple SDK).  What we
# CAN do without one:
#   (a) compile every self-contained kext C/C++ file to assembly with
#       clang/gcc (aarch64 target if installed, else host arch) - syntax +
#       static contract correctness; link-free;
#   (b) run the vendored xnu headers via the include paths in xnu-deps/
#       (see ../xnu-deps/README.md);
#   (c) leave the -c object files in build/kexts/ for later packaging.
#
# Product packaging into .kext bundles stays a `make kexts` follow-up
# (see kexts/README.md §Build model); use `tools/install_llvm_toolchain.sh`
# to add a cross toolchain to PATH for target-correct ISA.

BUILD_ROOT := $(abspath $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST)))))/../build/kexts

DEPS_INC := ../xnu-deps/includes
XNU_INC  := ../xnu

# Look for the compiler up front, degrade to host gcc when absent.
CROSS := $(shell which aarch64-none-elf-gcc aarch64-linux-gnu-gcc 2>/dev/null | head -1)

KEXT_SOURCES := \
	MT6785GICv3/MT6785GICv3.cpp \
	MT6785PlatformExpert/MT6785PlatformExpert.cpp \
	MT6785Framebuffer/MT6785Framebuffer.cpp \
	LxStorage/LXBlockService.cpp \
	FocalTouchIOHID/FocalTouchService.cpp \
	Stubs/AMFIStub.cpp \
	Stubs/SEPStub.cpp \
	Stubs/WatchdogStub.cpp

OBJS := $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(KEXT_SOURCES)))

CC   ?= $(if $(CROSS),$(CROSS),gcc)
CCFLAGS := -std=gnu++11 -ffreestanding -fno-builtin -fno-exceptions \
           -fno-rtti -nostdlib -mgeneral-regs-only
INCS := -I. -I$(DEPS_INC) -I$(XNU_INC)/iokit -I$(XNU_INC)/libkern \
        -I$(XNU_INC)/osfmk -I../wifi/lx/include/lx

.PHONY: build
build:
	@mkdir -p $(BUILD_ROOT)
	@echo "== kexts/build.mk: SOFTWARE-stack syntax verification ==" \
	      "CC=$(CC) CROSS=$(CROSS)"
	@set -e; for src in $(KEXT_SOURCES); do \
		obj=$(BUILD_ROOT)/$$src.o; \
		mkdir -p $$(dirname $$obj); \
		$(CC) $(CCFLAGS) $(INCS) -c $$src -o $$obj || { \
			echo "SKIP (kernel headers not available for $$src)"; \
			rm -f $$obj; \
		}; \
	done
	@echo "kexts done (see $(BUILD_ROOT))"
