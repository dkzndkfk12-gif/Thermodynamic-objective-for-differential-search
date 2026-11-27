#!/usr/bin/env bash
set -euo pipefail
variant_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  printf '%s\n' 'bash run.sh OUTPUT_DIR [--cores N --hours H --seed N --resume]'
  exit 0
fi
if (( $# < 1 )); then
  printf '%s\n' 'Output directory required.' >&2
  exit 2
fi
exec bash "$variant_root/../run.sh" 13 "$@"
