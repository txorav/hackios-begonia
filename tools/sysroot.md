# tools/sysroot.md — OPTIONAL iPhone/arm64 iOS SDK notes

This note explains when the project needs a real **iOS SDK sysroot** on the
host, how to obtain it *without any download during the build*, and which
flags pair it with the toolchain from `install_llvm_toolchain.sh`.

**It is optional.** Phases 1–5 as planned do not require an Apple SDK at all:

| What we build | Needs Apple SDK? | Why |
| :--- | :--- | :--- |
| `xnuloader` (repo, core) | no | freestanding, `--target=aarch64-none-elf`, no libc |
| xnu kernelcache (xnu-deps toolchain) | no | kernel build uses XNU's own headers |
| kexts (`kexts/`) | no | built against XNU headers + `xnu-deps` include set |
| **static shell** (`userland/ramdisk/static_shell`) | **no** | pure freestanding: syscalls declared in its own `syscalls.h` (numbers verified against `bsd/kern/syscalls.master` of xnu-8796) |
| iOS-side userland bins linked against real libSystem/CoreFoundation *later* | **yes** | that is when a sysroot is required |

## 1. Where a compliant SDK comes from (and from nowhere else)

The contract is **local-only**: the SDK must already exist on an Apple
developer workstation in your possession. We never download it here —
redistributing Apple SDKs violates their EULA, and the offline-first rule
of this repo forbids fetches inside build scripts anyway.

* From **Xcode 14.3.1** (the release aligned with the **iOS 16.5 / 20F66**
  target we restored for this port — it ships `iPhoneOS16.4.sdk`, which is
  fine; targeting `16.4` against a `16.5` OS is supported, there is no
  functional delta for the subset we need):

  ```
  Xcode_14.3.1.xip → Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS16.4.sdk
  ```

* On a Mac, the canonical path query is:

  ```
  xcrun --sdk iphoneos --show-sdk-path
  ```

* Copy that whole SDK directory to the Linux build host, e.g.
  `/opt/hackios/iPhoneOS16.4.sdk`. Keep it primitive right there — the
  `install_llvm_toolchain.sh` script installs the *compiler/linker*;
  the sysroot is a separate, non-redistributed addition.

## 2. Sanity-checking the copied sysroot

Run these and confirm every path exists before wiring the toolchain up:

```
iPhoneOS16.4.sdk/
    usr/include/TargetConditionals.h      # dropped by Apple toolchains, mandatory include
    usr/include/mach/mach.h
    usr/include/sys/syscall.h             # matches bsd/kern/syscalls.master numbering
    usr/include/ar && usr/lib/libSystem.tbd
```

(`.tbd` = *text-based dylib stubs*. Our `ld64` from cctools-port reads
them; BFD linkers do not — do not attempt GNU `ld` with an Apple sysroot.)

## 3. Flags that pair the sysroot with our toolchain

Compile (any clang ≥ 15, including the one installed by
`install_llvm_toolchain.sh`):

```
clang -target arm64-apple-ios16.4 \
      -isysroot /path/iPhoneOS16.4.sdk \
      -miphoneos-version-min=16.4  # implicit with an -apple-ios target triple
```

Link **on Linux** (cctools-port `ld64`, installed by
`install_llvm_toolchain.sh`):

```
ld64 -arch arm64 \
     -syslibroot /path/iPhoneOS16.4.sdk \
     -platform_version ios 16.4 16.5 \
     -o hello main.o
```

Link **on macOS** (native ld64 from `xcode-select`): same flags as Apple
intend them — `-isysroot` on ld too.

## 4. When *not* to reach for the sysroot

* Anytime you are tempted to pull headers for **kernel-side code** — use the
  in-tree `xnu/` headers; kernel and user ABI headers differ
  (`pexpert/` and `libsyscall/` boundaries are XNU's own, not the SDK's).
* For the **static shell**: it declares only what it calls — `exit=1,
  read=3, write=4, open=5, close=6, getpid=20, ioctl=54, execve=59,
  fsync=95` plus `mach_msg_trap = -31` (see
  `userland/ramdisk/static_shell/syscalls.h`) — all verified against
  `bsd/kern/syscalls.master` / `osfmk/mach/syscall_sw.h` of 8796.
  Linking it against libSystem would pull dyld paths we don't have yet.
* For **anything device-tree/ADT/loader related** — that data comes from
  `xnuloader` and `docs/hardware_map_mt6785.md`, not from any SDK.

## 5. Checklist before first sysroot-based build

1. `install_hosttoolchain.sh` + `install_llvm_toolchain.sh` finished cleanly.
2. `./check_env.sh` shows clang with **“Mach-O emission ok”** (that probe
   exercises the `-target aarch64-apple-darwin` path the sysroot needs).
3. Sysroot copies exist under `/opt/hackios/…` with the §2 layout verified.
4. `ldid` present (fake sign; the AMFI story is handled kernel-side).
