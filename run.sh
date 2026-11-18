#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${project_dir}/build"

for tool in cmake ctest; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Error: %s is required but was not found.\n' "$tool" >&2
        exit 1
    fi
done

cmake -S "${project_dir}" -B "$build_dir" \
    -DBWT_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure
