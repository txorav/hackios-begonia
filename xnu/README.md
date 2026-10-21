# HackiOS Kernel Research Tree

HackiOS is an experimental systems-research project exploring low-level operating-system architecture, Darwin/XNU internals, and the engineering challenges involved in adapting system components to non-Apple hardware.

This repository is organized around a Darwin/XNU-style source tree. It provides material for source-level study, build-system investigation, and platform research. The presence of source files does **not** imply that a bootable HackiOS image exists or that any particular device is supported.

## Project Scope

The current focus is on understanding the interfaces and dependencies that make up a modern hybrid kernel:

- Mach-based kernel facilities and core OS abstractions.
- BSD process, networking, filesystem, and compatibility layers.
- IOKit and related driver frameworks.
- Platform-specific initialization and architecture support.
- Kernel build configuration, diagnostics, and test infrastructure.

Device bring-up is a separate engineering effort. A functional port requires compatible platform code, hardware drivers, a suitable boot chain, and extensive validation on the intended device.

## Source Tree

The top-level directories follow the layout used by the included XNU-derived source tree.

| Directory | Purpose |
| --- | --- |
| `EXTERNAL_HEADERS/` | External and shared headers used to reduce dependency cycles. |
| `SETUP/` | Build-time utilities for configuration, installation, and symbol processing. |
| `bsd/` | BSD-derived kernel facilities, including process, networking, and filesystem code. |
| `config/` | Kernel and extension configuration files, templates, and metadata. |
| `doc/` | Build-system notes and technical documentation. |
| `iokit/` | IOKit and related driver-framework implementation. |
| `libkdd/` | Kernel-data decoding support. |
| `libkern/` | Kernel C/C++ support libraries and runtime facilities. |
| `libsa/` | Standalone and bootstrap support code. |
| `libsyscall/` | System-call interface support. |
| `makedefs/` | Shared make definitions and build rules. |
| `osfmk/` | Mach kernel subsystems and core low-level facilities. |
| `pexpert/` | Platform and machine-dependent support. |
| `san/` | Sanitizer-related kernel support. |
| `security/` | Kernel security interfaces and implementations. |
| `tests/` | Test sources and validation infrastructure. |
| `tools/` | Development, debugging, and profiling utilities. |

The precise contents and build targets are determined by the checked-in makefiles and configuration files.

## Architecture Notes

XNU is a hybrid kernel combining Mach facilities with BSD-derived components and Apple's IOKit driver framework. Its implementation is tightly coupled to the operating-system build environment, platform configuration, and supporting system components.

Source-level compatibility is not the same as binary compatibility. Compiling a kernel component for ARM64 does not make it compatible with an arbitrary ARM64 phone. Device-specific boot support, memory layout, interrupt routing, clocks, power management, storage, display, and other drivers must be addressed independently.

## Build Environment

Build requirements vary with the source revision and target configuration. Review the documentation under `doc/building/`, the top-level `Makefile`, `makedefs/`, and the relevant platform configuration before attempting a build.

A typical XNU-style build invocation has the following form:

```sh
make SDKROOT=<sdkroot> ARCH_CONFIGS=<architecture> KERNEL_CONFIGS=<configuration>
```

Common configuration names in XNU build systems include `DEVELOPMENT`, `DEBUG`, `RELEASE`, and `PROFILE`; availability depends on the specific source revision and build environment. For example, `ARM64` or `X86_64` may be valid architecture selections only when supported by the checked-in configuration and SDK.

This command is a build-system pattern, not a guarantee that this tree builds successfully on every host. Apple-internal SDKs, unavailable dependencies, signing requirements, or platform-specific tools may prevent a complete build outside the intended environment.

## Device-Porting Considerations

Research involving a non-Apple device should proceed in stages:

1. Audit the source tree and identify its upstream revision and license notices.
2. Establish a reproducible build environment for supported targets.
3. Separate generic kernel facilities from platform-specific code.
4. Document hardware requirements and missing driver interfaces.
5. Test in an emulator or recoverable development environment where possible.
6. Validate each component before attempting any device-level integration.

Do not install experimental kernel images on a primary device without a verified recovery path and a backup of the original firmware.

## Repository Status

This tree is intended for source inspection and experimental development. Device support, bootability, and feature completeness must be established by reproducible builds and documented tests; they should not be inferred from directory names or source-file presence alone.

## Licensing and Attribution

This repository contains Apple-originated source material and associated notices. Review `APPLE_LICENSE` and the license headers in individual files before using, modifying, or redistributing any component. Additional third-party components may have their own terms.

HackiOS is an independent research project and is not affiliated with, endorsed by, or supported by Apple Inc. or OnePlus. Apple, iOS, Darwin, XNU, and OnePlus are referenced only to identify the technologies or platforms discussed.
