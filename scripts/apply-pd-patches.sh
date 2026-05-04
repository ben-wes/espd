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
  "pd-d_fft_fftsg-FFTFLT-ifndef.patch"
  "x_net-esp-socket-compat.patch"
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
  elif git -C "${PD_DIR}" apply --reverse --check "${patch_path}" >/dev/null 2>&1; then
    echo "skipping ${p} (already applied)"
  else
    echo "error: ${p} does not apply (conflict or wrong pd revision); try:" >&2
    echo "  git -C ${PD_DIR} status" >&2
    git -C "${PD_DIR}" apply --check "${patch_path}" >&2 || true
    exit 1
  fi
done

echo "done"

