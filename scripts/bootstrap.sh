#!/usr/bin/env bash
# Prepares everything `cmake --preset <preset>` needs: a local tool virtualenv and
# the locked Conan dependencies. Usage: scripts/bootstrap.sh [debug|release|asan|tsan]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
preset="${1:-debug}"

case "${preset}" in
  release) build_type=Release; deps_dir=release ;;
  debug | asan | tsan) build_type=Debug; deps_dir=debug ;;
  *) echo "unknown preset: ${preset}" >&2; exit 2 ;;
esac

cd "${repo_root}"
export PATH="${repo_root}/.venv/bin:${PATH}"

if [[ ! -x .venv/bin/pip ]]; then
  # Falls back to fetching pip by hand where python3-venv's ensurepip is absent.
  if ! python3 -m venv .venv 2>/dev/null; then
    python3 -m venv --without-pip .venv
    wheel_dir="$(mktemp -d)"
    trap 'rm -rf "${wheel_dir}"' EXIT
    python3 - "${wheel_dir}" <<'PY'
import json, sys, urllib.request
req = urllib.request.Request("https://pypi.org/simple/pip/",
                             headers={"Accept": "application/vnd.pypi.simple.v1+json"})
files = json.load(urllib.request.urlopen(req))["files"]
wheels = [f for f in files if f["filename"].endswith(".whl") and not f.get("yanked")
          and f["filename"].split("-")[1].replace(".", "").isdigit()]
urllib.request.urlretrieve(wheels[-1]["url"], f"{sys.argv[1]}/{wheels[-1]['filename']}")
PY
    wheel="$(echo "${wheel_dir}"/pip-*.whl)"
    .venv/bin/python "${wheel}/pip" install -q --disable-pip-version-check "${wheel}"
  fi
fi
.venv/bin/pip install -q --disable-pip-version-check -r requirements-dev.txt

# The lockfile pins recipe revisions, so rebuilds resolve the same dependency graph.
conan install . \
  --lockfile=conan.lock \
  --profile:all=conan/profiles/linux-gcc \
  --settings:host="build_type=${build_type}" \
  --build=missing \
  --output-folder="build/deps/${deps_dir}"
