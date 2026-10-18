#!/usr/bin/env bash
# Sourced by the scripts beside it, after toolchain.sh.  fetch NAME URL SHA256 [PATCH...]
# unpacks a release into $WORK/src/NAME once, checked by its sha256, with the patches applied; it is
# unpacked anew (and $WORK/src/NAME-build removed) only when the release or a patch changes.
fetch() {
    local name=$1 url=$2 sum=$3; shift 3
    local dir=$WORK/src/$name tar=$WORK/src/$name.tar
    local stamp; stamp="$sum $(cat "$@" /dev/null | sha256sum | cut -c1-16)"
    [ "$(cat "$dir/.shaderemu" 2>/dev/null)" = "$stamp" ] && return 0
    echo "== $name"
    [ -f "$tar" ] && echo "$sum  $tar" | sha256sum -c --quiet 2>/dev/null || curl -sSfL "$url" -o "$tar"
    echo "$sum  $tar" | sha256sum -c --quiet || { echo "$name: the download is not the release"; exit 1; }
    rm -rf "$dir" "$WORK/src/$name-build"; mkdir -p "$dir"
    tar -xf "$tar" -C "$dir" --strip-components=1 && chmod -R u+w "$dir"
    local p; for p in "$@"; do patch -s -d "$dir" -p1 < "$p"; done
    echo "$stamp" > "$dir/.shaderemu"
}
