#!/usr/bin/env bash
# Rebuild MaterialXRemoteServer in a fresh WSL build directory with EGL-only flags.
# Run from the repo root: bash scripts/build_remote_egl.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-wsl"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMATERIALX_BUILD_REMOTE=ON \
  -DMATERIALX_BUILD_VIEWER=OFF \
  -DMATERIALX_BUILD_GRAPH_EDITOR=OFF \
  -DMATERIALX_REMOTE_EGL_ONLY=ON \
  -DMATERIALX_EGL_ONLY=ON \
  -DCMAKE_MODULE_PATH="${REPO_ROOT}/cmake/modules" \
  ..

ninja MaterialXRemoteServer
