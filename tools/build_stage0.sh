#!/usr/bin/env sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="$root/build/stage0"
mkdir -p "$out"

cxx="${CXX:-c++}"
"$cxx" -std=c++20 -Wall -Wextra -pedantic \
  "$root/compiler/stage0/src/main.cpp" \
  -o "$out/zeno"

printf '%s\n' "$out/zeno"
