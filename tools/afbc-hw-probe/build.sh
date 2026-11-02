#!/bin/sh
set -eu
cd "$(dirname "$0")"
if [ "${1:-}" = "--zig" ]; then
    shift
    zig_bin=${1:-zig}
    "$zig_bin" cc -target arm-linux-gnueabihf.2.17 -mcpu=cortex_a7 -fPIC -pie -O2 -std=c11 -Wall -Wextra -Werror probe.c -o probe-armhf -ldl
else
    "${CC:-arm-linux-gnueabihf-gcc}" -fPIC -pie -O2 -std=c11 -Wall -Wextra -Werror probe.c -o probe-armhf -ldl
fi
