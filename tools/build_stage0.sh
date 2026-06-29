#!/usr/bin/env sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="$root/build/stage0"
build_dir="$out/cmake"
mkdir -p "$out" "$build_dir"

if [ -n "${CMAKE:-}" ]; then
  cmake_bin="$CMAKE"
elif command -v cmake >/dev/null 2>&1; then
  cmake_bin="$(command -v cmake)"
elif [ -x /opt/homebrew/bin/cmake ]; then
  cmake_bin=/opt/homebrew/bin/cmake
else
  echo "stage0 build requires CMake; install cmake or set CMAKE=/path/to/cmake" >&2
  exit 1
fi

if [ -n "${NINJA:-}" ]; then
  ninja_bin="$NINJA"
elif command -v ninja >/dev/null 2>&1; then
  ninja_bin="$(command -v ninja)"
elif [ -x /opt/homebrew/bin/ninja ]; then
  ninja_bin=/opt/homebrew/bin/ninja
else
  echo "stage0 build requires Ninja; install ninja or set NINJA=/path/to/ninja" >&2
  exit 1
fi

llvm_dir="${LLVM_DIR:-}"
if [ -z "$llvm_dir" ] && [ -d /opt/homebrew/opt/llvm@21/lib/cmake/llvm ]; then
  llvm_dir=/opt/homebrew/opt/llvm@21/lib/cmake/llvm
fi
if [ -z "$llvm_dir" ]; then
  echo "stage0 build requires LLVM 21; set LLVM_DIR=/path/to/llvm/lib/cmake/llvm" >&2
  exit 1
fi

"$cmake_bin" -S "$root" -B "$build_dir" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$ninja_bin" \
  -DLLVM_DIR="$llvm_dir" \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$out" >/dev/null
"$cmake_bin" --build "$build_dir" --target zeno-stage0 >/dev/null

printf '%s\n' "$out/zeno"
