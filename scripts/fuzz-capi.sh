#!/usr/bin/env bash
# Coverage-guided libFuzzer campaign over the public C API, with ASan for the
# C shim and Rust adapter. Usage: fuzz-capi.sh [seconds] [jobs] [-- libFuzzer args]
# VTIFF_FUZZ_TARGET=capi_diff instead compares every call with the pinned
# libtiff built by test-compat.sh (.tools/lerc-tiff-source).
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$TIFF_ROOT"
seconds="${1:-60}"; jobs="${2:-1}"; shift $(( $# < 2 ? $# : 2 )); [ "${1:-}" = -- ] && shift
nightly="$(python3 -c 'import json;print(json.load(open("toolchain-lock.json"))["fuzz_rust"])')"
rustup toolchain list | grep -q "^$nightly" || rustup toolchain install "$nightly" --profile minimal
# cargo-fuzz's instrumentation, spelled out so no extra tool is needed. The
# explicit target keeps sanitizer flags away from build scripts and macros.
export RUSTFLAGS="-Cpasses=sancov-module -Cllvm-args=-sanitizer-coverage-level=4 \
 -Cllvm-args=-sanitizer-coverage-inline-8bit-counters -Cllvm-args=-sanitizer-coverage-pc-table \
 -Cllvm-args=-sanitizer-coverage-trace-compares --cfg fuzzing -Zsanitizer=address -Cforce-frame-pointers"
target=x86_64-unknown-linux-gnu
bin="${VTIFF_FUZZ_TARGET:-capi}"; features=()
[ "$bin" = capi_diff ] && features=(--features reference)
cargo "+$nightly" build --locked --release --manifest-path fuzz/capi/Cargo.toml --target "$target" --bin "$bin" "${features[@]}"
binary="$TIFF_ROOT/fuzz/capi/target/$target/release/$bin"
[ "${VTIFF_FUZZ_BUILD_ONLY:-0}" = 1 ] && exit 0
work="$TIFF_ROOT/target/fuzz-$bin"; mkdir -p "$work/corpus" "$work/artifacts"
# Seed from every checked-in fixture; libFuzzer merges them into its corpus.
seeds=("$TIFF_ROOT"/tests/fixtures "$TIFF_ROOT"/tests/upstream "$TIFF_ROOT"/fuzz/capi/regressions)
# libFuzzer writes per-job logs into the working directory.
cd "$work"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1:allocator_may_return_null=1}" \
 "$binary" corpus "${seeds[@]}" -artifact_prefix=artifacts/ -max_total_time="$seconds" \
 -jobs="$jobs" -workers="$jobs" -max_len=1048576 -rss_limit_mb=2048 -timeout=10 "$@"
