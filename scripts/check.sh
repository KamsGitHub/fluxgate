#!/usr/bin/env bash
# Runs the static checks: formatting, then clang-tidy over a configured build.
# Usage: scripts/check.sh [preset]   (default: debug; run its cmake configure first)
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${1:-debug}"
cd "${repo_root}"
export PATH="${repo_root}/.venv/bin:${PATH}"

mapfile -t all_sources < <(find include src tests/unit -name '*.hpp' -o -name '*.cpp')
clang-format --dry-run --Werror "${all_sources[@]}"

mapfile -t tidy_sources < <(find src -name '*.cpp')
printf '%s\n' "${tidy_sources[@]}" |
  xargs -P "$(nproc)" -n 4 clang-tidy --quiet -p "build/${preset}"
