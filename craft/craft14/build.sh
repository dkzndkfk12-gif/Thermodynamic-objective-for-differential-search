#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd -- "$project_root"
mkdir -p bin
exec 9>bin/.build.lock
flock 9
inputs=$( { sha256sum -- build.sh; find src -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum --; } | sha256sum)
inputs="${inputs%% *}"
stamp=bin/.craft_mass.build.sha256
if [[ ${1:-} == --if-needed && -x bin/craft_mass && -f $stamp ]]; then
  if read -r previous expected_binary < "$stamp" && [[ $previous == "$inputs" ]]; then
    binary=$(sha256sum -- bin/craft_mass)
    if [[ ${binary%% *} == "$expected_binary" ]]; then exit 0; fi
  fi
fi
cxx="${CXX:-g++}"
"$cxx" -std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic \
  -fno-fast-math -ffp-contract=off src/main.cpp \
  -lgmpxx -lgmp -l:libzstd.so.1 -lz -lsqlite3 -o bin/craft_mass.tmp
binary=$(sha256sum -- bin/craft_mass.tmp)
printf '%s %s\n' "$inputs" "${binary%% *}" > "$stamp.tmp"
mv -- bin/craft_mass.tmp bin/craft_mass
mv -- "$stamp.tmp" "$stamp"
