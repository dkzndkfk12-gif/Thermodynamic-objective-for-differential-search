#!/usr/bin/env bash
set -euo pipefail
variant_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  printf '%s\n' 'bash run.sh OUTPUT_DIR [--cores N --hours H --seed N]'
  exit 0
fi
if (($#)) && [[ $1 != -* ]]; then out=$1; shift; else out=runs/run; fi
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)
started=$(date +%s%N)
printf '%s\n' "C++ build/check: $out/build.log"
if ! bash "$variant_dir/../build.sh" "$(basename -- "$variant_dir")" >> "$out/build.log" 2>&1; then
  tail -n 40 "$out/build.log" >&2
  exit 1
fi
build_ns=$(( $(date +%s%N) - started ))
build_seconds=$(printf '%d.%09d' "$((build_ns / 1000000000))" "$((build_ns % 1000000000))")
exec "$variant_dir/../bin/campaign" --variant "$variant_dir" --build-seconds "$build_seconds" "$out" "$@"
