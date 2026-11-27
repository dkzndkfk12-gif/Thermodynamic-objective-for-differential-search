#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if (( $# == 0 )) || [[ ${1:-} == --help || ${1:-} == -h ]]; then
  printf '%s\n' 'bash run.sh VARIANT OUTPUT_DIR [--cores N --hours H --seed N]'
  printf '%s\n' 'SKINNY: skinny64_8 skinny64_9 skinny64_10 skinny64_11 skinny128_14 skinny128_15 skinny128_16'
  printf '%s\n' 'Midori: midori64_8 midori64_9 midori64_10 midori128_9 midori128_10'
  printf '%s\n' 'PRESENT: present16 present17'
  printf '%s\n' 'GIFT: gift64_13 gift64_14 gift128_20 gift128_21'
  printf '%s\n' 'TWINE: twine64_14 twine64_15'
  printf '%s\n' 'CRAFT: craft64_13 craft64_14'
  exit 0
fi
variant=$1
shift
case "$variant" in
  skinny64_8) script="$root/skinny_midori/skinny64_8/run.sh" ;;
  skinny64_9) script="$root/skinny_midori/skinny64_9/run.sh" ;;
  skinny64_10) script="$root/skinny_midori/skinny64_10/run.sh" ;;
  skinny64_11) script="$root/skinny_midori/skinny64_11/run.sh" ;;
  skinny128_14) script="$root/skinny_midori/skinny128_14/run.sh" ;;
  skinny128_15) script="$root/skinny_midori/skinny128_15/run.sh" ;;
  skinny128_16) script="$root/skinny_midori/skinny128_16/run.sh" ;;
  midori64_8) script="$root/skinny_midori/midori64_8/run.sh" ;;
  midori64_9) script="$root/skinny_midori/midori64_9/run.sh" ;;
  midori64_10) script="$root/skinny_midori/midori64_10/run.sh" ;;
  midori128_9) script="$root/skinny_midori/midori128_9/run.sh" ;;
  midori128_10) script="$root/skinny_midori/midori128_10/run.sh" ;;
  present16) script="$root/present/present16/run.sh" ;;
  present17) script="$root/present/present17/run.sh" ;;
  gift64_13) script="$root/gift/gift64/gift64_13/run.sh" ;;
  gift64_14) script="$root/gift/gift64/gift64_14/run.sh" ;;
  gift128_20) script="$root/gift/gift128_20/gift128_20/run.sh" ;;
  gift128_21) script="$root/gift/gift128_21/gift128_21/run.sh" ;;
  twine64_14) script="$root/twine/twine64_14/run.sh" ;;
  twine64_15) script="$root/twine/twine64_15/run.sh" ;;
  craft64_13) script="$root/craft/craft13/craft64_13/run.sh" ;;
  craft64_14) script="$root/craft/craft14/craft64_14/run.sh" ;;
  *) printf 'Unknown variant: %s\n' "$variant" >&2; exit 2 ;;
esac
if (( $# )) && [[ $1 != -* ]]; then out=$1; shift; else out="runs/$variant"; fi
args=("$@")
while (( $# )); do
  case "$1" in
    --cores|--hours|--seed) if (( $# < 2 )); then printf 'Missing value: %s\n' "$1" >&2; exit 2; fi; shift 2 ;;
    --resume) case "$variant" in twine*|craft*) shift ;; *) printf '%s\n' 'This variant resumes automatically from its output directory.' >&2; exit 2 ;; esac ;;
    --help|-h) exec bash "$script" --help ;;
    *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
  esac
done
mkdir -p -- "$out"
out=$(cd -- "$out" && pwd)
exec bash "$script" "$out" "${args[@]}"
