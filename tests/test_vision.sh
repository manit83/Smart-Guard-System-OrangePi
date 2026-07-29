#!/usr/bin/env bash
set -uo pipefail

CONFIG_PATH="${CONFIG_PATH:-/etc/smart-guard/config.env}"
BOARD_HOST="${BOARD_HOST:-127.0.0.1}"
PASS_COUNT=0
FAIL_COUNT=0

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this integration test with sudo." >&2
  exit 1
fi

read_config_value() {
  local key="$1"
  local fallback="$2"
  local value

  value="$(
    awk -F= -v wanted="${key}" '
      $1 == wanted {
        value = substr($0, index($0, "=") + 1)
        gsub(/^[[:space:]"]+|[[:space:]"]+$/, "", value)
        print value
        exit
      }
    ' "${CONFIG_PATH}" 2>/dev/null
  )"
  printf '%s\n' "${value:-${fallback}}"
}

pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  printf '[PASS] %s\n' "$1"
}

fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  printf '[FAIL] %s\n' "$1" >&2
}

LATEST_FRAME_PATH="$(
  read_config_value LATEST_FRAME_PATH /run/smart-guard/latest.jpg
)"
VISION_STATE_PATH="$(
  read_config_value VISION_STATE_PATH /run/smart-guard/vision-state.json
)"
VISION_PYTHON="$(read_config_value VISION_PYTHON /usr/bin/python3)"
FRAME_MAX_AGE_SEC="$(read_config_value FRAME_MAX_AGE_SEC 5)"
HTTPS_PORT="$(read_config_value HTTPS_PORT 443)"
HTTPS_URL="https://${BOARD_HOST}"
[[ "${HTTPS_PORT}" == "443" ]] || HTTPS_URL="${HTTPS_URL}:${HTTPS_PORT}"

if "${VISION_PYTHON}" -c 'import cv2, numpy' >/dev/null 2>&1; then
  pass "OpenCV and numpy are importable"
else
  fail "OpenCV or numpy cannot be imported by ${VISION_PYTHON}"
fi

for service in smart-guard-camera.service smart-guard-vision.service; do
  if systemctl is-active --quiet "${service}"; then
    pass "${service} is active"
  else
    fail "${service} is not active"
  fi

  if systemctl is-enabled --quiet "${service}"; then
    pass "${service} is enabled at boot"
  else
    fail "${service} is not enabled"
  fi
done

requires="$(
  systemctl show smart-guard-vision.service -p Requires --value
)"
after="$(systemctl show smart-guard-vision.service -p After --value)"
if [[ " ${requires} " == *" smart-guard-camera.service "* &&
      " ${after} " == *" smart-guard-camera.service "* ]]; then
  pass "Vision service has Requires/After camera dependency"
else
  fail "Vision service dependency on camera is incomplete"
fi

check_fresh_file() {
  local path="$1"
  local label="$2"

  if [[ ! -s "${path}" ]]; then
    fail "${label} is missing (${path})"
    return
  fi

  local age
  age=$(( $(date +%s) - $(stat -c %Y "${path}") ))
  if (( age >= 0 && age <= FRAME_MAX_AGE_SEC )); then
    pass "${label} is fresh (${age}s old)"
  else
    fail "${label} is stale (${age}s old)"
  fi
}

check_fresh_file "${LATEST_FRAME_PATH}" "Annotated frame"
check_fresh_file "${VISION_STATE_PATH}" "Vision state"

if [[ -s "${LATEST_FRAME_PATH}" ]]; then
  jpeg_magic="$(
    od -An -tx1 -N2 "${LATEST_FRAME_PATH}" | tr -d '[:space:]'
  )"
  if [[ "${jpeg_magic}" == "ffd8" ]]; then
    pass "Annotated output has a JPEG header"
  else
    fail "Annotated output is not a JPEG"
  fi
fi

if "${VISION_PYTHON}" - "${VISION_STATE_PATH}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as state_file:
    state = json.load(state_file)

required = {
    "persons",
    "faces",
    "timestamp",
    "fps",
    "detector",
    "camera_source",
    "frame_width",
    "frame_height",
}
if not required.issubset(state):
    raise SystemExit(1)
if not isinstance(state["persons"], int) or state["persons"] < 0:
    raise SystemExit(1)
if state["faces"] is not None and state["persons"] != state["faces"]:
    raise SystemExit(1)
PY
then
  pass "Vision state JSON has all required fields"
else
  fail "Vision state JSON is invalid"
fi

persons_response="$(
  curl -k -sS --max-time 8 \
    "${HTTPS_URL}/api/v1/persons" 2>/dev/null
)"
if printf '%s' "${persons_response}" |
   "${VISION_PYTHON}" -c '
import json
import sys
state = json.load(sys.stdin)
raise SystemExit(0 if isinstance(state.get("persons"), int) else 1)
'; then
  pass "GET /api/v1/persons returns the live face count"
else
  fail "GET /api/v1/persons did not return valid state"
fi

frame_code="$(
  curl -k -sS --max-time 8 -o /dev/null -w '%{http_code}' \
    "${HTTPS_URL}/api/v1/stream?frames=1" 2>/dev/null
)"
if [[ "${frame_code}" == "200" ]]; then
  pass "Required MJPEG stream endpoint returns HTTP 200"
else
  fail "Dashboard frame endpoint returned HTTP ${frame_code:-none}"
fi

echo
echo "Result: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[[ "${FAIL_COUNT}" -eq 0 ]]
