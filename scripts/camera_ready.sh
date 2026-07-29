#!/usr/bin/env bash
set -euo pipefail

CAMERA_SOURCE="${CAMERA_SOURCE:-v4l2}"
CAMERA_DEVICE="${CAMERA_DEVICE:-/dev/video0}"
CAMERA_READY_TIMEOUT_SEC="${CAMERA_READY_TIMEOUT_SEC:-30}"
VISION_PYTHON="${VISION_PYTHON:-/usr/bin/python3}"
ONVIF_HOST="${ONVIF_HOST:-}"
ONVIF_PORT="${ONVIF_PORT:-80}"
ONVIF_USERNAME="${ONVIF_USERNAME:-}"
ONVIF_PASSWORD_FILE="${ONVIF_PASSWORD_FILE:-/etc/smart-guard/onvif-password}"

if [[ ! "${CAMERA_READY_TIMEOUT_SEC}" =~ ^[1-9][0-9]*$ ]] ||
   (( CAMERA_READY_TIMEOUT_SEC > 300 )); then
  echo "CAMERA_READY_TIMEOUT_SEC must be between 1 and 300." >&2
  exit 1
fi

case "${CAMERA_SOURCE}" in
  v4l2)
    if [[ "${CAMERA_DEVICE}" == "auto" ]]; then
      CAMERA_DEVICE="/dev/video*"
    elif [[ ! "${CAMERA_DEVICE}" =~ ^/dev/(video[0-9]+|v4l/(by-id|by-path)/[^/]+)$ ]]; then
      echo "Invalid CAMERA_DEVICE: ${CAMERA_DEVICE}" >&2
      exit 1
    fi

    for ((second = 0; second < CAMERA_READY_TIMEOUT_SEC; second++)); do
      for candidate in ${CAMERA_DEVICE}; do
        [[ -c "${candidate}" ]] || continue
        if "${VISION_PYTHON}" - "${candidate}" <<'PY'
import errno
import os
import sys

device = sys.argv[1]
flags = os.O_RDWR | getattr(os, "O_NONBLOCK", 0)
try:
    descriptor = os.open(device, flags)
except OSError as error:
    name = errno.errorcode.get(error.errno, "UNKNOWN")
    print(
        f"V4L2 read/write probe failed for {device}: "
        f"errno={error.errno} ({name}): {error.strerror}",
        file=sys.stderr,
    )
    raise SystemExit(1)
else:
    os.close(descriptor)
PY
        then
          echo "V4L2 camera read/write probe passed: ${candidate}"
          exit 0
        fi
      done
      sleep 1
    done

    echo "Camera did not become ready: ${CAMERA_DEVICE}" >&2
    echo "Run: v4l2-ctl --list-devices" >&2
    exit 1
    ;;

  onvif)
    if [[ -z "${ONVIF_HOST}" || -z "${ONVIF_USERNAME}" ]]; then
      echo "ONVIF_HOST and ONVIF_USERNAME are required." >&2
      exit 1
    fi
    if [[ ! "${ONVIF_PORT}" =~ ^[1-9][0-9]*$ ]] ||
       (( ONVIF_PORT > 65535 )); then
      echo "Invalid ONVIF_PORT: ${ONVIF_PORT}" >&2
      exit 1
    fi
    if [[ ! -r "${ONVIF_PASSWORD_FILE}" ]]; then
      echo "ONVIF password file is not readable: ${ONVIF_PASSWORD_FILE}" >&2
      exit 1
    fi

    for ((second = 0; second < CAMERA_READY_TIMEOUT_SEC; second++)); do
      if /usr/bin/python3 - "${ONVIF_HOST}" "${ONVIF_PORT}" <<'PY'
import socket
import sys

try:
    with socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=2):
        pass
except OSError:
    raise SystemExit(1)
PY
      then
        echo "ONVIF device service is reachable: ${ONVIF_HOST}:${ONVIF_PORT}"
        exit 0
      fi
      sleep 1
    done

    echo "ONVIF device service is not reachable." >&2
    exit 1
    ;;

  *)
    echo "CAMERA_SOURCE must be v4l2 or onvif." >&2
    exit 1
    ;;
esac
