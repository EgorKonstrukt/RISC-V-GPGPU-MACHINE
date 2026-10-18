#!/usr/bin/env bash
# Builds OpenSSH 9.9p2's sftp and sftp-server (BSD, LICENCE) without OpenSSL, in place of upstream's:
# the two of OpenSSH that fork nothing but a program they exec. sftp runs /usr/bin/ssh, which is
# Dropbear's client (dropbear.sh); Dropbear's server runs /usr/libexec/sftp-server for "sftp".
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh ports/sftp.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
source "$HERE/fetch.sh"
OUT=${PORTS_OUT:-$REPO/build/images/linux/root}	# (PORTS_OUT: a private image's overlay)
fetch openssh https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-9.9p2.tar.gz \
    91aadb603e08cc285eddf965e1199d02585fa94d994d6cae5b41e1721e215673
O=$WORK/src/openssh
CONF="--host=riscv32-linux-musl CC=rv32-cc AR=riscv32-linux-ar RANLIB=riscv32-linux-ranlib CFLAGS=-O2
    --without-openssl --without-zlib --without-pam --without-selinux --without-security-key-builtin
    --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --without-hardening
    --without-stackprotect --without-pie --prefix=/usr --sysconfdir=/etc/ssh --libexecdir=/usr/libexec"
if [ "$(cat "$O/.conf" 2>/dev/null)" != "$CONF" ]; then
    (cd "$O" && ./configure -q $CONF > /dev/null) && echo "$CONF" > "$O/.conf"
fi
# (its static select() meets musl's, declared by sys/types.h, which nommu_fork.h includes first)
grep -q 'smult_select' "$O/smult_curve25519_ref.c" || sed -i 's/\bselect(/smult_select(/g' "$O/smult_curve25519_ref.c"
make -s -C "$O" -j"$(nproc)" sftp sftp-server > /dev/null
mkdir -p "$OUT/usr/bin" "$OUT/usr/libexec" "$OUT/usr/share/openssh"
riscv32-linux-strip -o "$OUT/usr/bin/sftp" "$O/sftp"
riscv32-linux-strip -o "$OUT/usr/libexec/sftp-server" "$O/sftp-server"
cp "$O/LICENCE" "$OUT/usr/share/openssh/LICENCE"
ls -l "$OUT/usr/bin/sftp" "$OUT/usr/libexec/sftp-server"
