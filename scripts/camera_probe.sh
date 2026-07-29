#!/usr/bin/env bash
set -euo pipefail

CONFIG_PATH="${1:-/etc/smart-guard/config.env}"
VISION_APP="/opt/smart-guard/vision/vision_app.py"

if [[ ! -r "${CONFIG_PATH}" ]]; then
  echo "Config file is not readable: ${CONFIG_PATH}" >&2
  exit 1
fi

if [[ ! -r "${VISION_APP}" ]]; then
  echo "Vision application is missing: ${VISION_APP}" >&2
  exit 1
fi

run_probe() {
  set -a
  # shellcheck disable=SC1090
  source "${CONFIG_PATH}"
  set +a
  exec "${VISION_PYTHON:-/usr/bin/python3}" \
    -u "${VISION_APP}" --probe-camera
}

if [[ "${EUID}" -eq 0 ]]; then
  exec runuser -u smartguard -- \
    /opt/smart-guard/bin/camera_probe.sh "${CONFIG_PATH}"
fi

run_probe
