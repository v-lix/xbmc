#!/bin/sh
# SPDX-License-Identifier: MIT
# No system configuration changes, service control, OSD submission, or swap.
set -u
umask 077
ulimit -c 0 2>/dev/null || true
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
mode=all
profile=both
library=/usr/lib/libMali.so
heap_mask=1
output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --info) mode=info; shift ;;
        --all) mode=all; shift ;;
        --profile) [ "$#" -ge 2 ] || exit 1; profile=$2; shift 2 ;;
        --library) [ "$#" -ge 2 ] || exit 1; library=$2; shift 2 ;;
        --heap-mask) [ "$#" -ge 2 ] || exit 1; heap_mask=$2; shift 2 ;;
        --output) [ "$#" -ge 2 ] || exit 1; output=$2; shift 2 ;;
        *) echo "Usage: sh run.sh [--info|--all] [--profile both|plain|split] [--library PATH] [--heap-mask MASK] [--output NEW_DIRECTORY]" >&2; exit 1 ;;
    esac
done
case "$profile" in both|plain|split) ;; *) echo "Invalid AFBC profile: $profile" >&2; exit 1;; esac
if [ -z "$output" ]; then
    if [ -d /storage ]; then root=/storage; else root=${TMPDIR:-/tmp}; fi
    output=$root/afbc-probe-results-$(date -u +%Y%m%dT%H%M%SZ)-$$
fi
if [ -e "$output" ]; then echo "Output directory already exists: $output" >&2; exit 1; fi
mkdir -p -- "$output" || exit 1
output=$(CDPATH= cd -- "$output" && pwd) || exit 1
cd -- "$output" || exit 1
printf 'case\tstatus\texit_code\n' > summary.tsv
known_sha=949b043bd373a7779d4338a87ecdd929e0430dfddcb092de7a4e6cda3a905397
known_stripped_sha=789adf85dc9c630bb049249bf7e28ed40380e919ae1f379a352167d3107ec445
{
    echo "probe_version=2"
    echo "timeout_backend=native-supervisor"
    echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "library_requested=$library"
    echo "mode=$mode profile=$profile heap_mask=$heap_mask"
    uname -a
    [ ! -r /etc/os-release ] || cat /etc/os-release
    if [ -r /proc/device-tree/model ]; then printf 'model='; tr '\000' '\n' < /proc/device-tree/model; fi
    for dev in /dev/mali0 /dev/mali /dev/ion /dev/fb0 /dev/dma_heap/system; do
        [ ! -e "$dev" ] || ls -l "$dev"
    done
    if command -v systemctl >/dev/null 2>&1; then
        printf 'kodi_service='; systemctl is-active kodi 2>/dev/null || true
    fi
    for f in /sys/class/graphics/fb0/name /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/osd_afbcd /sys/module/mali/version; do
        if [ -r "$f" ]; then printf '%s=' "$f"; cat "$f"; fi
    done
} > system.txt 2>&1
finish()
{
    if command -v dmesg >/dev/null 2>&1; then dmesg 2>&1 | tail -n 200 > dmesg-after.txt; fi
    echo
    cat summary.tsv
    echo
    echo "Results: $output"
    archive=$output.tar.gz
    if tar -czf "$archive" -C "$(dirname -- "$output")" "$(basename -- "$output")"; then
        echo "Send back this archive: $archive"
    else
        echo "Could not create archive; send the results directory."
    fi
}
if [ ! -r "$library" ]; then
    echo "ERROR: cannot read $library" | tee error.txt
    finish; exit 1
fi
resolved=$(readlink -f -- "$library" 2>/dev/null) || resolved=$library
library=$resolved
case "$library" in /*) ;; *) echo 'ERROR: library path must resolve to an absolute path' > error.txt; finish; exit 1;; esac
sha=unavailable
if command -v sha256sum >/dev/null 2>&1; then
    sha=$(sha256sum "$library" | awk '{print $1}')
fi
{
    echo "library_resolved=$library"
    echo "sha256=$sha"
    if command -v strings >/dev/null 2>&1; then
        strings "$library" | grep -E '^afbc=[01] .*winsys=' || true
    fi
} >> system.txt 2>&1
probe=$base/probe-armhf
if [ ! -x "$probe" ]; then
    echo "ERROR: missing executable $probe; extract the complete package." | tee error.txt
    finish; exit 1
fi
if command -v dmesg >/dev/null 2>&1; then dmesg 2>&1 | tail -n 200 > dmesg-before.txt; fi
status_for_exit()
{
    case "$1" in
        0) echo PASS ;;
        2) echo SKIP ;;
        124) echo TIMEOUT ;;
        *) if [ "$1" -ge 128 ]; then echo CRASH; else echo FAIL; fi ;;
    esac
}
run_case()
{
    test_case=$1
    echo "Running $test_case (60-second process timeout)..."
    if "$probe" --watchdog 60 --library "$library" --case "$test_case" --driver-sha256 "$sha" --heap-mask "$heap_mask" > "$test_case.log" 2>&1; then
        printf '%s\tPASS\t0\n' "$test_case" >> summary.tsv
        tail -n 4 "$test_case.log"
        return 0
    else
        rc=$?
        status=$(status_for_exit "$rc")
        printf '%s\t%s\t%s\n' "$test_case" "$status" "$rc" >> summary.tsv
        tail -n 12 "$test_case.log"
        return 1
    fi
}
if "$probe" --watchdog 20 --self-test > self-test.log 2>&1; then
    printf 'self-test\tPASS\t0\n' >> summary.tsv
else
    rc=$?
    status=$(status_for_exit "$rc")
    printf 'self-test\t%s\t%s\n' "$status" "$rc" >> summary.tsv
    cat self-test.log
    echo 'ARM loader/architecture error or probe self-test failure. No GPU test was run.'
    finish; exit 1
fi
if ! run_case info; then finish; exit 1; fi
if [ "$mode" = info ]; then finish; exit 0; fi
if ! run_case local; then
    echo 'Ordinary offscreen GLES control failed; import tests skipped.'
    finish; exit 1
fi
case "$sha" in
    "$known_sha"|"$known_stripped_sha") ;;
    *)
        printf 'private-imports\tSKIP_UNRECOGNIZED_LIBRARY\t2\n' >> summary.tsv
        echo 'The library differs from the verified original/stripped builds. AFBC remains untested.'
        finish; exit 0
        ;;
esac
if ! run_case linear; then
    echo 'Linear native-pixmap import control failed; AFBC tests skipped.'
    finish; exit 1
fi
# Do not submit another GPU test after a crash, a timeout, or an import/render
# failure. Logs tell us whether the ABI or the format needs further work.
if [ "$profile" != split ]; then
    if ! run_case afbc; then finish; exit 1; fi
fi
if [ "$profile" != plain ]; then
    if ! run_case afbc-split; then finish; exit 1; fi
fi
finish
