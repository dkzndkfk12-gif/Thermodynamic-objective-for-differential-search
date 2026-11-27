#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
variant=${1:?Usage: bash build.sh VARIANT }
case "$variant" in gift128_21) ;; *) echo "Unknown variant" >&2; exit 2;; esac
cxx=${CXX:-g++}
mkdir -p -- "$root/bin" "$root/$variant/bin"
flags=(-O3 -std=c++17 -pthread -Wall -Wextra -fno-fast-math -ffp-contract=off)
signature() { { "$cxx" --version; printf '%s\n' "${flags[*]}"; find "$1" -type f \( -name '*.inc' -o -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0 | sort -z | xargs -0 sha256sum; } | sha256sum | cut -d' ' -f1; }
exec 9>"$root/bin/build.lock"
flock 9
common_hash=$(printf '%s\n%s\n' "$(signature "$root/common")" "$(signature "$root/../shared")" | sha256sum | cut -d' ' -f1)
if [[ ! -x "$root/bin/campaign" || ! -f "$root/bin/common.sha256" || $(cat "$root/bin/common.sha256") != "$common_hash" ]]; then
  "$cxx" "${flags[@]}" "$root/common/main.cpp" -o "$root/bin/campaign.tmp" -lgmpxx -lgmp -lsqlite3
  mv -- "$root/bin/campaign.tmp" "$root/bin/campaign"
  printf '%s\n' "$common_hash" > "$root/bin/common.sha256"
fi
if [[ ! -x "$root/bin/graph" || ! -f "$root/bin/graph.sha256" || $(cat "$root/bin/graph.sha256") != "$common_hash" ]]; then
  "$cxx" "${flags[@]}" "$root/common/graph_main.cpp" -o "$root/bin/graph.tmp" -lgmpxx -lgmp
  mv -- "$root/bin/graph.tmp" "$root/bin/graph"
  printf '%s\n' "$common_hash" > "$root/bin/graph.sha256"
fi
if [[ ! -x "$root/bin/unseen" || ! -f "$root/bin/unseen.sha256" || $(cat "$root/bin/unseen.sha256") != "$common_hash" ]]; then
  "$cxx" "${flags[@]}" "$root/common/unseen_main.cpp" -o "$root/bin/unseen.tmp" -lgmpxx -lgmp
  mv -- "$root/bin/unseen.tmp" "$root/bin/unseen"
  printf '%s\n' "$common_hash" > "$root/bin/unseen.sha256"
fi
flock -u 9
exec 8>"$root/$variant/bin/build.lock"
flock 8
native_hash=$(printf '%s\n%s\n' "$(signature "$root/$variant/src")" "$common_hash" | sha256sum | cut -d' ' -f1)
if [[ ! -x "$root/$variant/bin/search" || ! -x "$root/$variant/bin/exact" || ( -f "$root/$variant/src/pair.cpp" && ! -x "$root/$variant/bin/pair" ) || ! -f "$root/$variant/bin/native.sha256" || $(cat "$root/$variant/bin/native.sha256") != "$native_hash" ]]; then
  for target in search exact; do
    "$cxx" "${flags[@]}" "$root/$variant/src/$target.cpp" -o "$root/$variant/bin/$target.tmp" -lgmpxx -lgmp
    mv -- "$root/$variant/bin/$target.tmp" "$root/$variant/bin/$target"
  done
  if [[ -f "$root/$variant/src/pair.cpp" ]]; then
    "$cxx" "${flags[@]}" -fopenmp "$root/$variant/src/pair.cpp" -o "$root/$variant/bin/pair.tmp"
    mv -- "$root/$variant/bin/pair.tmp" "$root/$variant/bin/pair"
  fi
  printf '%s\n' "$native_hash" > "$root/$variant/bin/native.sha256"
fi
