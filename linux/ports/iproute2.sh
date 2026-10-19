#!/usr/bin/env bash
# Builds iproute2 6.16.0 (GPL-2.0) for its programs other than ip, which is BusyBox's applet here:
# ss, bridge, tc, genl, ifstat, lnstat, nstat, rtacct and rtmon into /sbin, in place of upstream's.
# No libmnl, libbpf, libelf, libcap or SELinux; what the kernel lacks (no netfilter, few qdiscs) they say.
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh ports/iproute2.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
source "$HERE/fetch.sh"
OUT=${PORTS_OUT:-$REPO/build/images/linux/root}	# (PORTS_OUT: a private image's overlay)
fetch iproute2 https://mirrors.edge.kernel.org/pub/linux/utils/net/iproute2/iproute2-6.16.0.tar.xz \
    5900ccc15f9ac3bf7b7eae81deb5937123df35e99347a7f11a22818482f0a8d0
I=$WORK/src/iproute2
if [ ! -f "$I/config.mk" ]; then
    # configure looks for libraries with pkg-config: none for the guest
    (cd "$I" && PKG_CONFIG=/bin/false CC=rv32-cc AR=riscv32-linux-ar ./configure --prefix=/usr --libbpf_force off > /dev/null)
fi
# (the kernel build's bison, as linux/kernel/build.sh runs it)
export BISON_PKGDATADIR="$WORK/tools/usr/share/bison" M4="$WORK/tools/usr/bin/m4"
# SHARED_LIBS=n: tc's qdiscs and genl's ctrl from tables, not from dlsym, which a static program lacks
M="CC=rv32-cc AR=riscv32-linux-ar HOSTCC=gcc CCOPTS=-O2 V=0 SHARED_LIBS=n"
[ "$(cat "$I/.make" 2>/dev/null)" = "$M" ] || { make -s -C "$I" clean > /dev/null; echo "$M" > "$I/.make"; }
make -s -C "$I" $M SUBDIRS="lib ip tc bridge genl misc" > /dev/null	# (ip/ for rtmon; ip itself is not taken)
mkdir -p "$OUT/sbin" "$OUT/usr/share/iproute2"
for p in misc/ss bridge/bridge tc/tc genl/genl misc/ifstat misc/lnstat misc/nstat misc/rtacct ip/rtmon; do
    riscv32-linux-strip -o "$OUT/sbin/$(basename $p)" "$I/$p"
done
cp "$I/COPYING" "$OUT/usr/share/iproute2/COPYING"
ls -l "$OUT"/sbin/{ss,bridge,tc,genl,ifstat,lnstat,nstat,rtacct,rtmon}
