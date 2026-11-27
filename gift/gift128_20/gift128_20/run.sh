#!/usr/bin/env bash
set -euo pipefail
variant_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname -- "$variant_dir")
name=$(basename -- "$variant_dir")
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  printf '%s\n' 'bash run.sh OUTPUT_DIR [--cores N --hours H --seed N]'
  exit 0
fi
if (($#)) && [[ $1 != -* ]]; then out=$1; shift; else out=runs/cpp_01; fi
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)
exec 7>"$out/launch.lock"
flock -n 7 || { echo 'This output is already running.' >&2; exit 1; }
started=$(date +%s%N)
printf '%s\n' "C++ compile cache: $out/build.log"
if ! bash "$root/build.sh" "$name" >> "$out/build.log" 2>&1; then
  tail -n 40 "$out/build.log" >&2
  exit 1
fi
build_ns=$(( $(date +%s%N) - started ))
build_seconds=$(printf '%d.%09d' "$((build_ns / 1000000000))" "$((build_ns % 1000000000))")
export OMP_DYNAMIC=FALSE OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
exec "$root/bin/campaign" --variant "$variant_dir" --out "$out" --build-seconds "$build_seconds" "$@"
