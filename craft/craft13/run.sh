#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  printf '%s\n' 'bash run.sh ROUNDS OUTPUT_DIR [--cores N --hours H --seed N --resume]'
  exit 0
fi
if (( $# < 2 )); then
  printf '%s\n' 'Rounds and output directory required.' >&2
  exit 2
fi
rounds=$1
out=$2
shift 2
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)
exec 7>"$out/launch.lock"
flock -n 7 || { printf '%s\n' 'This output is already running.' >&2; exit 1; }
if ! bash "$project_root/build.sh" --if-needed >> "$out/build.log" 2>&1; then
  tail -n 30 "$out/build.log" >&2
  exit 1
fi
exec "$project_root/bin/craft_mass" --rounds "$rounds" --out "$out" "$@"
