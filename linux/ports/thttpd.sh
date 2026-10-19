#!/usr/bin/env bash
# Builds thttpd 2.29 (BSD-2-Clause) as /usr/sbin/thttpd, with makeweb and htpasswd, in place of
# upstream's. thttpd.patch: it stays in the foreground, writes a directory's listing itself, and runs
# a CGI through itself started again (thttpd -H), which feeds the CGI and parses its headers:
# every child it forked ran on after fork().
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh ports/thttpd.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
source "$HERE/fetch.sh"
OUT=${PORTS_OUT:-$REPO/build/images/linux/root}	# (PORTS_OUT: a private image's overlay)
fetch thttpd https://www.acme.com/software/thttpd/thttpd-2.29.tar.gz \
    99c09f47da326b1e7b5295c45549d2b65534dce27c44812cf7eef1441681a397 "$HERE/thttpd.patch"
T=$WORK/src/thttpd
# its config.sub knows no RISC-V: the two names configure asks about, as they are
rm -f "$T/config.sub"; printf '#!/bin/sh\ncase $1 in riscv32*) echo riscv32-unknown-linux-musl ;; *) echo "$1" ;; esac\n' > "$T/config.sub"
# (an old configure: the compiler from the environment)
CONF="--host=riscv32-linux-musl --prefix=/usr"
if [ "$(cat "$T/.conf" 2>/dev/null)" != "$CONF" ]; then
    (cd "$T" && CC=rv32-cc CFLAGS=-O2 ./configure -q $CONF > /dev/null) && echo "$CONF" > "$T/.conf"
fi
make -s -C "$T" thttpd > /dev/null
make -s -C "$T/extras" makeweb htpasswd > /dev/null
mkdir -p "$OUT/usr/sbin" "$OUT/usr/bin" "$OUT/usr/share/thttpd"
riscv32-linux-strip -o "$OUT/usr/sbin/thttpd" "$T/thttpd"
riscv32-linux-strip -o "$OUT/usr/bin/makeweb" "$T/extras/makeweb"
riscv32-linux-strip -o "$OUT/usr/bin/htpasswd" "$T/extras/htpasswd"
sed -n '/Copyright/,/SUCH DAMAGE/p' "$T/thttpd.c" > "$OUT/usr/share/thttpd/LICENSE"
ls -l "$OUT/usr/sbin/thttpd" "$OUT/usr/bin/makeweb" "$OUT/usr/bin/htpasswd"
