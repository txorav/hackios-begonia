#!/bin/bash
# Build everything a device needs, in order, skipping what cannot be built on this machine.
#
# usage: common/build.sh <device> [userland/build.sh variables...]
#   common/build.sh qemu-virt
#   common/build.sh redwood GL=1
#
#   1. kernel   xnu/apply.sh, xnu-deps/setup.sh, xnu-build.sh   -- only with theos + iPhoneOS16.5.sdk,
#                                                                  otherwise prebuilt/ is used
#   2. loader   xnuloader/build.sh                              -- only with clang + lld-link
#   3. images   userland/build.sh (REST=1 and the profile's USERLAND_VARS)
#                                                               -- needs ios165/fetch.sh to have run
#   4. boot     mkxnuimg.py -> out/<device>-boot.img             -- phones: the UEFI image for `fastboot boot`
#
#   SKIP="kernel loader images boot"   leave steps out
. "$(dirname "$(readlink -f "$0")")/lib.sh"
load_device "$1"; shift
skip() { [[ " $SKIP " == *" $1 "* ]]; }
mkdir -p out

if ! skip kernel; then
	if [ -d "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk" ] && command -v clang >/dev/null; then
		[ -d xnu-src ] || xnu/apply.sh
		[ -x xnu-deps/host/bin/ld64 ] || xnu-deps/setup.sh
		# Wi-Fi radio driver (phones with a WCN6750): fetched and built once
		[ "$WIFI_ATH11K" = 1 ] && [ ! -f xnu-deps/build/ath11k/out/ath11k.o ] && { tools/mk-ath11k.sh && tools/mk-wlfw.sh; }
		# the export check fails after the kernel is linked, on purpose (xnu/README.md): judge by the file
		touch out/.kernel-start
		./xnu-build.sh -j"$(nproc)" -k > out/kernel-build.log 2>&1 || true
		K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
		[ -f $K ] && { [ $K -nt out/.kernel-start ] || ! grep -q "error:" out/kernel-build.log; } &&
			echo "build: kernel $(stat -c %s $K) bytes" || die "kernel build failed, see out/kernel-build.log"
	else
		echo "build: no theos/iPhoneOS16.5.sdk: using prebuilt/mach.development.bcm2837"
	fi
fi

if ! skip loader; then
	if [ "$DEVICE" = begonia ]; then
		make xnuloader && echo "build: loader out/xnuloader.bin"
	elif command -v clang >/dev/null && command -v lld-link >/dev/null; then
		xnuloader/build.sh && echo "build: loader xnuloader/loader.efi"
	else
		echo "build: no clang/lld-link: using prebuilt/loader.efi"
	fi
fi

if ! skip images; then
	have_userland
	if [ -d ios165/rootfs ]; then
		env REST=1 $USERLAND_VARS "$@" userland/build.sh
	elif [ -f out/root.apfs ]; then
		echo "build: using existing out/root.apfs"
	else
		echo "build: no ios165/rootfs: creating base APFS root.apfs from prebuilt userland"
		mkdir -p build/test_apfs/root/bin build/test_apfs/root/sbin build/test_apfs/root/lib build/test_apfs/root/etc build/test_apfs/root/dev build/test_apfs/root/share build/test_apfs/root/private/var build/test_apfs/root/tmp
		cp -f prebuilt/userland/init build/test_apfs/root/sbin/launchd
		cp -f prebuilt/userland/init build/test_apfs/root/sbin/init
		cp -f prebuilt/userland/init build/test_apfs/root/bin/init
		cp -f prebuilt/userland/child build/test_apfs/root/bin/child 2>/dev/null || true
		cp -f prebuilt/userland/hello build/test_apfs/root/bin/hello 2>/dev/null || true
		cp -f prebuilt/userland/libredwood*.dylib build/test_apfs/root/lib/ 2>/dev/null || true
		for prog in cpu_test mg_test net_test surf_test svc_test hello_dyn; do
			[ -f prebuilt/userland/$prog ] && cp -f prebuilt/userland/$prog build/test_apfs/root/bin/
		done
		echo "Darwin on redwood: begonia userland" > build/test_apfs/root/etc/motd
		python3 -c "import sys; sys.stdout.buffer.write(bytes((i * 31 + (i >> 8)) & 0xff for i in range(300000)))" > build/test_apfs/root/share/pattern.bin
		echo '/bin/hello' > build/test_apfs/root/etc/rc
		python3 tools/apfs/mkapfs.py -z -o out/root.apfs redwood=build/test_apfs/root | tail -1
	fi
fi

if ! skip boot && [ "$KIND" = phone ]; then
	if [ "$DEVICE" = begonia ]; then
		K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
		[ -f "$K" ] || K=prebuilt/mach.development.bcm2837
		[ -f "$K" ] || die "kernel missing"
		L=out/xnuloader.bin
		[ -f "$L" ] || L=xnuloader/xnuloader.bin
		[ -f "$L" ] || die "loader missing"
		RD_ARG=
		if [ -f out/root.apfs ]; then
			RD_ARG="--ramdisk out/root.apfs"
		fi
		python3 mkxnuimg.py --out out/$DEVICE-boot.img --loader "$L" --kernel "$K" $RD_ARG \
			--cmdline "$BOOTARGS" \
			--fb "${FB_WIDTH}x${FB_HEIGHT}x${FB_STRIDE}x${FB_DEPTH}" \
			--fb-base "$FB_BASE"
		echo "build: out/$DEVICE-boot.img -- boot it with: fastboot boot out/$DEVICE-boot.img (or ./xl boot out/$DEVICE-boot.img)"
	else
		# the UEFI with the loader inside, the way `xl enter` makes it: the loader then brings up its USB link
		L=xnuloader/loader.efi; [ -f $L ] || L=prebuilt/loader.efi
		python3 xnuloader/mkpayload.py -o out/usb-payload.bin loader.efi=$L --usb > /dev/null
		FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE UEFI_FD=$UEFI_FD DTB=$DTB UEFI_TARGET=$UEFI_TARGET \
			python3 mkxnuimg.py -p out/usb-payload.bin -o out/$DEVICE-boot.img
		echo "build: out/$DEVICE-boot.img -- boot it with: fastboot boot out/$DEVICE-boot.img (or common/run.sh $DEVICE)"
	fi
fi

