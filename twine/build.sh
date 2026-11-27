#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$project_root/bin"
exec 9>"$project_root/bin/.build.lock"
flock 9
cxx="${CXX:-g++}"
"$cxx" -std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -Wpedantic \
  -fno-fast-math -ffp-contract=off "$project_root/src/main.cpp" \
  -lgmpxx -lgmp -lz -o "$project_root/bin/twine_mass.tmp"
mv "$project_root/bin/twine_mass.tmp" "$project_root/bin/twine_mass"
