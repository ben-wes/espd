#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PD_DIR="${ROOT_DIR}/pd"
PATCH_DIR="${ROOT_DIR}/patches"

if [[ ! -e "${PD_DIR}/.git" ]]; then
  echo "error: pd submodule not initialized at ${PD_DIR}" >&2
  echo "run: git submodule update --init --recursive" >&2
  exit 1
fi

patches=(
  "0004-x_net-esp-socket-compat.patch"
)

for p in "${patches[@]}"; do
  patch_path="${PATCH_DIR}/${p}"
  if [[ ! -f "${patch_path}" ]]; then
    echo "error: missing patch ${patch_path}" >&2
    exit 1
  fi
  if git -C "${PD_DIR}" apply --check "${patch_path}" >/dev/null 2>&1; then
    echo "applying ${p}"
    git -C "${PD_DIR}" apply "${patch_path}"
  else
    echo "skipping ${p} (already applied or conflict)"
  fi
done

echo "done: pd patches applied"

