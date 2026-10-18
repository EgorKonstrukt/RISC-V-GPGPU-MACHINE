#!/usr/bin/env bash
# Builds curl 8.16.0 (curl licence) with Mbed TLS 3.6.4 (Apache-2.0) as /usr/bin/curl in place of
# upstream's dynamic one: http, https, ftp, file and the rest that needs no other library, no IPv6
# (the kernel has none). The image has no CA bundle: https wants -k or --cacert FILE.
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh ports/curl.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
source "$HERE/fetch.sh"
OUT=${PORTS_OUT:-$REPO/build/images/linux/root}	# (PORTS_OUT: a private image's overlay)
fetch mbedtls https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2 \
    ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110
fetch curl https://curl.se/download/curl-8.16.0.tar.xz \
    40c8cddbcb6cc6251c03dea423a472a6cea4037be654ba5cf5dec6eb2d22ff1d
M=$WORK/src/mbedtls
C=$WORK/src/curl
DEPS=$WORK/src/curl-deps
# getrandom() waits for a pool that is never ready here: /dev/urandom instead
if grep -q '^#if defined(SYS_getrandom)$' "$M/library/entropy_poll.c"; then
    sed -i 's|^#if defined(SYS_getrandom)$|#if 0 /* ShaderEmu: the kernel pool is never ready, read /dev/urandom */|' "$M/library/entropy_poll.c"
fi
grep -q '^#if 0 /\* ShaderEmu' "$M/library/entropy_poll.c" || { echo "mbedtls's getrandom moved"; exit 1; }
if [ ! -f "$DEPS/lib/libmbedtls.a" ] || [ "$M/.shaderemu" -nt "$DEPS/lib/libmbedtls.a" ]; then
    make -s -C "$M" -j"$(nproc)" lib CC=rv32-cc AR=riscv32-linux-ar CFLAGS="-O2" > /dev/null
    mkdir -p "$DEPS/lib" "$DEPS/include"
    cp "$M"/library/libmbed*.a "$DEPS/lib/"
    cp -r "$M/include/mbedtls" "$M/include/psa" "$DEPS/include/"
fi
CONF="--host=riscv32-linux-musl CC=rv32-cc AR=riscv32-linux-ar RANLIB=riscv32-linux-ranlib CFLAGS=-O2
    --disable-shared --enable-static --with-mbedtls=$DEPS --disable-ipv6 --disable-threaded-resolver
    --disable-ldap --disable-ldaps --disable-rtsp --disable-docs --without-zlib --without-brotli --without-zstd
    --without-libpsl --without-libidn2 --without-nghttp2 --without-libssh2 --without-librtmp
    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --without-ca-path"
if [ "$(cat "$C/.conf" 2>/dev/null)" != "$CONF" ]; then
    (cd "$C" && ./configure -q $CONF > /dev/null) && echo "$CONF" > "$C/.conf"
fi
make -s -C "$C" -j"$(nproc)" > /dev/null
mkdir -p "$OUT/usr/bin" "$OUT/usr/share/curl"
riscv32-linux-strip -o "$OUT/usr/bin/curl" "$C/src/curl"
cp "$C/COPYING" "$OUT/usr/share/curl/COPYING"
cp "$M/LICENSE" "$OUT/usr/share/curl/LICENSE-mbedtls"
ls -l "$OUT/usr/bin/curl"
