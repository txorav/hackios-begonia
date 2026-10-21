# M4 secondary-boot ramdisk (HackiOS-begonia userland)

Purpose: a small **filesystem image** that gets embedded into `boot.img` by
the root-repo packaging script and that XNU mounts as its first root at the
M4 milestone ("Single-User Shell", porting plan §4 M4):

> XNU mounts a RAMDisk and executes a static/dyld `/bin/bash` PID 1 console
> over USB serial / framebuffer.

This directory builds that image **and** the static ARM64 Mach-O binary that
runs as PID 1 inside it.

---

## 1. The exact boot contract (verified in-tree — not lore)

| Step | Who | Proof |
| :--- | :--- | :--- |
| ADT `/chosen/memory-map` carries key **`RAMDisk`**, `reg = <phys-base, size>` | xnuloader (repo root, other owner) | `docs/hardware_map_mt6785.md` §3 |
| XNU turns that memory region into a block device | XNU: `IOKitBSDInit.cpp:766-788` reads the OSData (2 × uintptr) and calls `mdevadd()` → `/dev/mdx` (first one = `md0`) | in-tree |
| Boot args mount it as root | `rd=md0 rootdev=md0` (canonical list: `docs/hardware_map_mt6785.md` §4) | in-tree (`IOKitBSDInit.cpp:725`) |
| PID 1 exec | XNU execs **`/sbin/launchd`** from that root (`kern_exec.c:7344`; DEBUG/DEVELOPMENT builds also try `launchdsuffix` paths) | in-tree |
| If it's missing | **kernel panic** `Process 1 exec of /sbin/launchd failed` | `kern_exec.c:7438` |

> **launchd caveat:** our shell is *not* a real launchd. XNU's M5+ flows
> (XPC bootstrap port publication, `launchd_checkin` messages) need a real
> launchd later; for M4 the REPL console is the goal. There is no
> `init=`-style boot-arg override in this XNU — ask kernel-port to add one or
> keep the `/sbin/launchd` masquerade (open question in
> `../IDEAS_OR_TODO.md`).

## 2. Filesystem format choice (and why)

Default image format: **HFS+ (non-journaled)** — realtime reasons:

* `mkfs.hfsplus` from `hfsprogs` is a standard Linux host tool; XNU's APFS
  container work is still pending (apfs.kext is Apple-closed, not in
  xnu-8796), so HFS+ images are the shortest path to M4 — **provided a
  minimal HFS+ mount capability exists kernel-side** (interface owner:
  `kexts/LxStorage`);
* iOS itself never mounts HFS in this form, but our bring-up drivers own the
  contract, so the format is ours to pick for the test ramdisk.

`--format vfat` is supported as an even simpler fallback (dosfstools) if the
HFS+ kext work drags.

**Note on HFS/APFS in XNU:** open-source XNU ships **neither** writable
filesystems filesystem implementations in-tree for these (HFS+ and APFS kexts
are Apple-closed); our port's mount kexts live in `kexts/` (kernel-port).
This is documented here so nobody looks for `bsd/hfs` in the reference tree.

## 3. The static shell (`static_shell/`)

* `main.c` — REPL over console I/O (`open("/dev/console")`; falls back to
  the manufactured stdio fds). Directly issues XNU syscalls (svc #0x80,
  number in x16; numbers verified against `xnu/bsd/kern/syscalls.master`:
  read=3, write=4, open=5, close=6, getpid=20, ioctl=54, execve=59).
* `mach_hello.c` — documents Mach-IPC reality: `mach_msg_trap = -31`
  (`osfmk/mach/syscall_sw.h:108`), no bootstrap port at bare boot, sends one
  message to `MACH_PORT_NULL` to show the expected `MACH_SEND_INVALID_DEST`.
* `syscalls.h` — the freestanding ABI subset (no lib System).
* `build.sh` — clang `-target aarch64-apple-darwin -nostdlib` + `ld64`
  (from `../../tools/install_llvm_toolchain.sh`) → static Mach-O; `ldid -S`
  ad-hoc sign when available.

Build:

```bash
cd static_shell && ./build.sh                       # shell + mach_hello
cd .. && ./mkramdisk.sh --out ~/hackios-begonia-work/package/ramdisk_m4.img \
    --size-mb 64 --shell static_shell/shell
```

If `ld64`/cctools aren't installed yet, `build.sh` prints exactly what is
missing instead of producing a broken artifact.

## 4. What the image must contain (M4 checklist)

```
ramdisk_m4.img (HFS+, non-journaled)
├── /sbin/launchd        ← static shell (PID1; exec'd by XNU directly)
├── /bin/bash            → symlink /sbin/launchd (muscle memory for M-tests)
├── /bin/mach_hello      ← IPC demo (optional, run with `exec /bin/mach_hello`)
├── /etc/passwd          (root::0:0:Operator:/:/sbin/launchd)
├── /dev/                (empty; XNU devfs creates files on need)
└── (optional) /logs/    writable dir for test dumps (writefile cmd)
```

The XNU-side devfs creates `/dev/console` at boot; we don't fill /dev.

## 5. Known limitations / honest gaps

* `sleep` needs the ARM Generic Timer userspace timer path — not wired in
  the shell yet (documented as a TODO inside `main.c`, no hidden stub).
* The shell cannot read the framebuffer console font rendering — everything
  lands on UART0 @ `0x11002000` and/or the lk framebuffer; host capture is
  `xl`'s job (root repo once that lands; see `../console/README.md`).
* No real filesystem beyond directory reads — `cat`, `hexdump`,
  `writefile` are the file ops. Enough for M4.
