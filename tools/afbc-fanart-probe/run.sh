#!/bin/sh
# SPDX-License-Identifier: MIT
# Offscreen only: no service control, sysfs writes, framebuffer submission, or swap.
set -u
umask 077
ulimit -c 0 2>/dev/null || true
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
library=/usr/lib/libMali.so
profile=both
label=unspecified
image_a=
image_b=
output=
frames=60
rounds=2
fps=60
cycles=1
mode=gpu
while [ "$#" -gt 0 ]; do
    case "$1" in
        --library) [ "$#" -ge 2 ] || exit 1; library=$2; shift 2 ;;
        --profile) [ "$#" -ge 2 ] || exit 1; profile=$2; shift 2 ;;
        --label) [ "$#" -ge 2 ] || exit 1; label=$2; shift 2 ;;
        --image-a) [ "$#" -ge 2 ] || exit 1; image_a=$2; shift 2 ;;
        --image-b) [ "$#" -ge 2 ] || exit 1; image_b=$2; shift 2 ;;
        --output) [ "$#" -ge 2 ] || exit 1; output=$2; shift 2 ;;
        --frames) [ "$#" -ge 2 ] || exit 1; frames=$2; shift 2 ;;
        --rounds) [ "$#" -ge 2 ] || exit 1; rounds=$2; shift 2 ;;
        --fps) [ "$#" -ge 2 ] || exit 1; fps=$2; shift 2 ;;
        --cycles) [ "$#" -ge 2 ] || exit 1; cycles=$2; shift 2 ;;
        --quick) frames=20; rounds=1; shift ;;
        --memory-only) mode=memory; shift ;;
        *) echo 'Usage: sh run.sh [--memory-only] [--label idle|h264|hevc] [--image-a JPEG_OR_PNG --image-b JPEG_OR_PNG] [--profile both|plain|split] [--quick] [--frames 2..600] [--rounds 1..4] [--cycles 1..5] [--fps 0..120] [--library PATH] [--output NEW_DIRECTORY]' >&2; exit 1 ;;
    esac
done
case "$profile" in both|plain|split) ;; *) echo 'Invalid profile' >&2; exit 1;; esac
case "$label" in ''|*[!a-zA-Z0-9_-]*) echo 'Label must contain only letters, digits, underscores and hyphens' >&2; exit 1;; esac
# Validate numeric options before passing them to any driver process.
for pair in "$frames:2:600" "$rounds:1:4" "$cycles:1:5" "$fps:0:120"; do
    value=${pair%%:*}; limits=${pair#*:}; low=${limits%%:*}; high=${limits#*:}
    case "$value" in ''|*[!0-9]*|??????????*|0?*) echo 'Invalid numeric option (use unpadded decimal integers)' >&2; exit 1;; esac
    if [ "$value" -lt "$low" ] || [ "$value" -gt "$high" ]; then echo 'Numeric option out of range' >&2; exit 1; fi
done
watchdog=180
if [ "$fps" -gt 0 ]; then
    estimated_seconds=$(( (4 * rounds * (frames + 4) * cycles + fps - 1) / fps ))
    watchdog=$((estimated_seconds + 90))
    [ "$watchdog" -ge 180 ] || watchdog=180
    if [ "$watchdog" -gt 900 ]; then echo 'Requested pacing duration exceeds the 900-second per-case limit; reduce frames/rounds/cycles or increase fps' >&2; exit 1; fi
fi
if [ -n "$image_a" ] || [ -n "$image_b" ]; then
    if [ ! -r "$image_a" ] || [ ! -r "$image_b" ]; then echo 'Provide two readable JPEG/PNG paths' >&2; exit 1; fi
    image_a=$(readlink -f -- "$image_a") || exit 1
    image_b=$(readlink -f -- "$image_b") || exit 1
fi
if [ -z "$output" ]; then
    if [ -d /storage ]; then root=/storage; else root=${TMPDIR:-/tmp}; fi
    output=$root/afbc-fanart-results-$label-$(date -u +%Y%m%dT%H%M%SZ)-$$
fi
if [ -e "$output" ]; then echo "Output already exists: $output" >&2; exit 1; fi
mkdir -p -- "$output" || exit 1
output=$(CDPATH= cd -- "$output" && pwd) || exit 1
cd -- "$output" || exit 1
printf 'case\tstatus\texit_code\n' > summary.tsv
finish()
{
    dmesg 2>&1 | tail -n 200 > dmesg-after.txt
    if dmesg 2>/dev/null | grep -F 'ion: ARM64 device-less page cache sync enabled' > kernel-cache-sync-after.txt; then :
    else echo 'not found in available kernel log' > kernel-cache-sync-after.txt; fi
    echo
    cat summary.tsv
    echo "Results: $output"
    if tar -czf "$output.tar.gz" -C "$(dirname -- "$output")" "$(basename -- "$output")"; then
        echo "Send back this archive: $output.tar.gz"
    else
        echo 'Could not create archive; send the results directory.'
    fi
}
{
    echo 'fanart_probe_version=2'
    echo "mode=$mode"
    echo "label=$label utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "profile=$profile frames=$frames rounds=$rounds cycles=$cycles pacing_fps=$fps"
    echo "watchdog_seconds=$watchdog"
    echo 'presented_frames=0 output_contains_source_images=0'
    uname -a
    [ ! -r /etc/os-release ] || cat /etc/os-release
    for f in /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/osd_afbcd /sys/class/display/mode /sys/class/thermal/thermal_zone0/temp; do
        if [ -r "$f" ]; then printf '%s=' "$f"; cat "$f"; fi
    done
    [ ! -r /proc/meminfo ] || head -n 5 /proc/meminfo
    if command -v systemctl >/dev/null 2>&1; then printf 'kodi_service='; systemctl is-active kodi 2>/dev/null || true; fi
    printf 'kernel_ion_cache_sync_marker='
    if dmesg 2>/dev/null | grep -F 'ion: ARM64 device-less page cache sync enabled'; then :; else echo 'not found in available kernel log'; fi
} > system.txt 2>&1
sha=not-used
if [ "$mode" != memory ]; then
if [ ! -r "$library" ]; then echo 'Cannot read Mali library' > error.txt; finish; exit 1; fi
library=$(readlink -f -- "$library") || { finish; exit 1; }
case "$library" in /*) ;; *) echo 'Library must resolve to an absolute path' > error.txt; finish; exit 1;; esac
sha=$(sha256sum "$library" | awk '{print $1}')
fi
{
    echo "library=$library sha256=$sha"
    if [ -n "$image_a" ]; then
        echo "image_a=$image_a image_b=$image_b"
        sha256sum "$image_a" "$image_b"
    else
        echo 'real_photo_inputs=not_provided (procedural patterns only)'
    fi
} >> system.txt
probe=$base/fanart-probe-armhf
if [ ! -x "$probe" ]; then echo 'Missing fanart-probe-armhf executable' > error.txt; finish; exit 1; fi
dmesg 2>&1 | tail -n 200 > dmesg-before.txt
run_case()
{
    name=$1; candidate=$2; pattern=$3; test_width=$4; test_height=$5
    mkdir -- "$name" || return 1
    echo "Running $name (offscreen, $watchdog-second watchdog)..."
    set -- "$probe" --watchdog "$watchdog" --library "$library" --driver-sha256 "$sha" \
        --width "$test_width" --height "$test_height" --pattern "$pattern" \
        --frames "$frames" --rounds "$rounds" --cycles "$cycles" --fps "$fps"
    if [ "$candidate" = local ]; then set -- "$@" --local
    elif [ "$candidate" = memory ]; then set -- "$@" --memory-check
    else set -- "$@" --profile "$candidate"; fi
    if [ "$pattern" = photos ]; then set -- "$@" --image-a "$image_a" --image-b "$image_b"; fi
    telemetry > "$name/telemetry-before.txt" 2>&1
    if (cd -- "$name" && "$@") > "$name/probe.log" 2>&1; then
        telemetry > "$name/telemetry-after.txt" 2>&1
        printf '%s\tPASS\t0\n' "$name" >> summary.tsv
        tail -n 7 "$name/probe.log"
        return 0
    else
        rc=$?
        telemetry > "$name/telemetry-after.txt" 2>&1
        case "$rc" in 2) status=SKIP;; 124) status=TIMEOUT;; *) if [ "$rc" -ge 128 ]; then status=CRASH; else status=FAIL; fi;; esac
        printf '%s\t%s\t%s\n' "$name" "$status" "$rc" >> summary.tsv
        tail -n 16 "$name/probe.log"
        return 1
    fi
}
telemetry()
{
    echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    for f in /sys/class/thermal/thermal_zone*/temp /sys/class/devfreq/*/cur_freq /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq; do
        if [ -r "$f" ]; then printf '%s=' "$f"; cat "$f"; fi
    done
    [ ! -r /proc/meminfo ] || head -n 5 /proc/meminfo
}
if "$probe" --watchdog 20 --self-test > self-test.log 2>&1; then
    printf 'self-test\tPASS\t0\n' >> summary.tsv
else
    rc=$?; printf 'self-test\tFAIL\t%s\n' "$rc" >> summary.tsv
    cat self-test.log; finish; exit 1
fi
if [ "$mode" = memory ]; then
    # A separate cold-boot experiment: never warm these pools before the normal
    # GPU test, which must retain its original first-allocation behavior.
    if ! run_case memory-128 memory detail 128 128; then finish; exit 1; fi
    if ! run_case memory-1080 memory detail 1920 1080; then finish; exit 1; fi
    if ! run_case memory-4k memory detail 3840 2160; then finish; exit 1; fi
    finish; exit 0
fi
case "$sha" in
    949b043bd373a7779d4338a87ecdd929e0430dfddcb092de7a4e6cda3a905397|789adf85dc9c630bb049249bf7e28ed40380e919ae1f379a352167d3107ec445) ;;
    *) printf 'private-imports\tSKIP_UNRECOGNIZED_LIBRARY\t2\n' >> summary.tsv; finish; exit 2;;
esac
# Stop immediately after any failed GPU case. Never continue after a hang/crash.
if ! run_case local-128-alpha local alpha 128 128; then finish; exit 1; fi
if ! run_case local-4k-detail local detail 3840 2160; then finish; exit 1; fi
profiles='afbc afbc-split'
[ "$profile" != plain ] || profiles=afbc
[ "$profile" != split ] || profiles=afbc-split
for candidate in $profiles; do
    if ! run_case "$candidate-128-alpha" "$candidate" alpha 128 128; then finish; exit 1; fi
    if ! run_case "$candidate-1080-detail" "$candidate" detail 1920 1080; then finish; exit 1; fi
    if ! run_case "$candidate-4k-detail" "$candidate" detail 3840 2160; then finish; exit 1; fi
    if ! run_case "$candidate-4k-noise" "$candidate" noise 3840 2160; then finish; exit 1; fi
    if ! run_case "$candidate-odd-detail" "$candidate" detail 3839 2159; then finish; exit 1; fi
    if [ -n "$image_a" ]; then
        if ! run_case "$candidate-4k-photos" "$candidate" photos 3840 2160; then finish; exit 1; fi
    fi
done
finish
