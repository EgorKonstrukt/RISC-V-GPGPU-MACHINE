#!/usr/bin/env bash
# Builds at 3.2.5 (GPL-2.0+): at (atq and atrm are the ROM's links to it) and atd, in place of
# upstream's. atd stays in the foreground, and a job's process is atd run again (at.patch): its
# child used to go on after fork(). Output is mailed through /usr/sbin/sendmail, which the machine lacks.
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh ports/at.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
source "$HERE/fetch.sh"
OUT=${PORTS_OUT:-$REPO/build/images/linux/root}	# (PORTS_OUT: a private image's overlay)
fetch at http://software.calhariz.com/at/at_3.2.5.orig.tar.gz \
    bb066b389d7c9bb9d84a35738032b85c30cba7d949f758192adc72c9477fd3b8 "$HERE/at.patch"
A=$WORK/src/at
# the kernel build's bison and flex, as linux/kernel/build.sh runs them
export BISON_PKGDATADIR="$WORK/tools/usr/share/bison" M4="$WORK/tools/usr/bin/m4"
# upstream's paths (buildroot's at.mk); no libfl for the guest: parsetime.l asks for no yywrap
CONF="--host=riscv32-linux-musl CC=rv32-cc CFLAGS=-O2 --with-jobdir=/var/spool/cron/atjobs
    --with-atspool=/var/spool/cron/atspool --with-daemon_username=root --with-daemon_groupname=root
    SENDMAIL=/usr/sbin/sendmail ac_cv_lib_fl_yywrap=no ac_cv_lib_l_yywrap=no --without-selinux"
# its config.sub (2011) knows no RISC-V: the two names configure asks about, as they are
rm -f "$A/config.sub"; printf '#!/bin/sh\ncase $1 in riscv32*) echo riscv32-unknown-linux-musl ;; *) echo "$1" ;; esac\n' > "$A/config.sub"
if [ "$(cat "$A/.conf" 2>/dev/null)" != "$CONF" ]; then
    (cd "$A" && ./configure -q $CONF > /dev/null) && echo "$CONF" > "$A/.conf"
fi
make -s -C "$A" at atd LEXLIB= > /dev/null
mkdir -p "$OUT/usr/bin" "$OUT/usr/sbin" "$OUT/usr/share/at"
riscv32-linux-strip -o "$OUT/usr/bin/at" "$A/at"
riscv32-linux-strip -o "$OUT/usr/sbin/atd" "$A/atd"
cp "$A/COPYING" "$A/Copyright" "$OUT/usr/share/at/"
ls -l "$OUT/usr/bin/at" "$OUT/usr/sbin/atd"
