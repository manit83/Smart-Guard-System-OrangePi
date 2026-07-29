#!/usr/bin/env bash
set -euo pipefail

VISION_PYTHON="${VISION_PYTHON:-/usr/bin/python3}"

if [[ ! -x "${VISION_PYTHON}" ]]; then
  echo "VISION_PYTHON is not executable: ${VISION_PYTHON}" >&2
  exit 1
fi

exec "${VISION_PYTHON}" -u /opt/smart-guard/vision/vision_app.py "$@"
