#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")"
if [ "${1:-}" = "--zig" ]; then
    shift
    "${1:-zig}" cc -target arm-linux-gnueabihf.2.17 -mcpu=cortex_a7 \
        -fPIC -pie -O2 -s -std=c11 -Wall -Wextra -Werror fanart-probe.c -o fanart-probe-armhf -ldl -lm
else
    "${CC:-arm-linux-gnueabihf-gcc}" -fPIC -pie -O2 -s -std=c11 \
        -Wall -Wextra -Werror fanart-probe.c -o fanart-probe-armhf -ldl -lm
fi
