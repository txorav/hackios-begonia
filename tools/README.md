# tools/ — cross-compilation tooling for HackiOS-begonia

Goal of this directory: make any Linux (Ubuntu 22.04+ tested) or macOS host
able to build, in order:

1. **`xnuloader`** — bare-metal EL1/EL2 loader, `aarch64-none-elf`,
   freestanding (built with plain clang; loads with a 64-byte ARM64 Linux
   header per plan §Phase 2 — `magic 0x644d5241 "ARM\x64"`).
2. **kexts** (Apple-style arm64 Mach-O, kernel-loadable) — for the XNU
   port; compiled with the host clang from apt (`llvm-16` toolchain) per the
   XNU-deps work, using Mach-O only.
3. **iOS-side binaries** — Apple-flavored arm64 Mach-O: needs a
   Darwin-targeted compiler (`clang -target aarch64-apple-darwin`, + ld64)
   and `ldid` for ad-hoc fake signing (`ldid -S`).

Contents:

| File | Purpose |
| :--- | --- |
| `install_llvm_toolchain.sh` | installs the llvm toolchain + cctools-port (ld64) + ldid; the *only* script allowed to `git clone`/`git fetch` — see its `--no-net` for offline verification |
| `install_hosttoolchain.sh` | installs the apt-side host deps (kext build helpers, dtc, python3, etc.) |
| `check_env.py` / `check_env.sh` | PASS/FAIL matrix of every binary this project needs |
| `sysroot.md` | OPTIONAL notes for iPhone arm64 iOS SDK extraction from Xcode |
| `adb/snippets.md` | safe one-liners for device-state probing before/without touching anything |
| `adb/snippets/create_patches.sh` | one-shot patch-diff helper used on-device (recovery) with an arg check it never auto-pushes |

## Host prerequisites (tested)

### Ubuntu 22.04+ / Debian 12+

```
sudo apt install clang lld llvm python3 python3-dev build-essential \
     bc bison flex libssl-dev libelf-dev device-tree-compiler \
     mkbootimg fastboot adb hfsprogs dosfstools
```
(`mkbootimg` is in Android SDK-tools or `apt install mkbootimg` on newer
releases; `fastboot`/`adb` via `android-sdk-platform-tools-common` if not
packaged.)

### macOS 13+

```
xcode-select --install          # provides clang/lld/libtool
brew install ldid python dtc e2fsprogs        # macports alternates slower
```
macOS HFS+ images don't need `hfsprogs` (native `hdiutil` covers it).

## What runs where (matrix)

| Artifact | Host | Cross flags | Sign |
| :--- | :--- | :--- | :--- |
| `xnuloader` | Ubuntu/macOS | `clang --target=aarch64-none-elf -nostdlib -ffreestanding` | n/a (boot-chain bypassed: bootloader-unlocked mode) |
| `fdt2adt` helpers | Ubuntu | `clang --target=aarch64-none-elf` or host | n/a |
| xnu kernelcache | Ubuntu | XNU-deps host toolchain (xnu-deps/) | trust caches/skip (AMFI-stub) |
| kexts | Ubuntu | `clang --target=...-apple-darwin` + cctools-port ld64 | `ldid -S`-style or trust cache stub |
| static shell (userland M4) | Ubuntu/macOS | `clang -target aarch64-apple-darwin` + ld64 (macOS: plain `clang`) | `ldid -S` where present |

## Ordering

```
./install_hosttoolchain.sh      # apt packages (no source builds except cctools)
./install_llvm_toolchain.sh     # toolchains + cctools-port (ld64) + ldid
./check_env.sh                  # everything greens?
```

## Open questions

* Whether kernel-port's kext toolchain ends up linking against Apple's
  `-flat_namespace` Mach-O rules — monitor xnu-deps' `Makefile.inc`.
* Official `mkbootimg` versions differ on header_version 2 flag defaults;
  the packer (`mkxnuimg.py`, root repo) pins the values matching
  `docs/hardware_map_mt6785.md` §5. Don't "fix" flags ad hoc here.
