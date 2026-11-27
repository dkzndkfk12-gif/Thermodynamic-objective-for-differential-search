#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
variant=${1:?variant required}
case "$variant" in present16|present17) ;; *) printf '%s\n' 'Unknown PRESENT variant' >&2; exit 2 ;; esac
mkdir -p -- "$root/bin" "$root/$variant/bin"
exec 8>"$root/bin/build.lock"
flock 8
read -r -a compiler <<< "${CXX:-g++}"
flags=(-O3 -std=c++17 -Wall -Wextra -fno-fast-math -ffp-contract=off -pthread)
printf '%s\n' "C++ compiler: ${compiler[*]}" "Flags: ${flags[*]}"
key=$({ printf '%s\n' "${compiler[*]} ${flags[*]}"; find "$root/common" -type f -print0 | sort -z | xargs -0 sha256sum; } | sha256sum | cut -d' ' -f1)
if [[ ! -x "$root/bin/campaign" || ! -f "$root/bin/campaign.sha256" || $(cat "$root/bin/campaign.sha256") != "$key" ]]; then
  "${compiler[@]}" "${flags[@]}" "$root/common/main.cpp" -lgmpxx -lgmp -lsqlite3 -o "$root/bin/campaign.next"
  mv -f -- "$root/bin/campaign.next" "$root/bin/campaign"
  printf '%s\n' "$key" > "$root/bin/campaign.sha256"
  printf '%s\n' 'Controller: rebuilt'
else
  printf '%s\n' 'Controller: cached'
fi
native_key=$({ printf '%s\n' "${compiler[*]} ${flags[*]} -fopenmp"; find "$root/$variant/src" -type f -print0 | sort -z | xargs -0 sha256sum; } | sha256sum | cut -d' ' -f1)
if [[ ! -x "$root/$variant/bin/search" || ! -x "$root/$variant/bin/exact" || ! -x "$root/$variant/bin/pair" || ! -f "$root/$variant/bin/native.sha256" || $(cat "$root/$variant/bin/native.sha256") != "$native_key" ]]; then
  for name in search exact pair; do
    "${compiler[@]}" "${flags[@]}" -fopenmp "$root/$variant/src/$name.cpp" -o "$root/$variant/bin/$name.next"
    mv -f -- "$root/$variant/bin/$name.next" "$root/$variant/bin/$name"
  done
  printf '%s\n' "$native_key" > "$root/$variant/bin/native.sha256"
  printf '%s\n' "Native kernels ($variant): rebuilt"
else
  printf '%s\n' "Native kernels ($variant): cached"
fi
