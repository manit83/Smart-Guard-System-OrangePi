#!/usr/bin/env bash
set -uo pipefail

CONFIG_PATH="${CONFIG_PATH:-/etc/smart-guard/config.env}"
BOARD_HOST="${BOARD_HOST:-127.0.0.1}"
PASS_COUNT=0
FAIL_COUNT=0

read_config_value() {
  local key="$1"
  local fallback="$2"

  if [[ ! -r "${CONFIG_PATH}" ]]; then
    printf '%s\n' "${fallback}"
    return
  fi

  local value
  value="$(
    awk -F= -v wanted="${key}" '
      $1 == wanted {
        value = substr($0, index($0, "=") + 1)
        gsub(/^[[:space:]"]+|[[:space:]"]+$/, "", value)
        print value
        exit
      }
    ' "${CONFIG_PATH}"
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

CAMERA_DEVICE="$(read_config_value CAMERA_DEVICE /dev/video0)"
if [[ "${CAMERA_DEVICE}" == "auto" ]]; then
  CAMERA_DEVICE="$(
    find /dev -maxdepth 1 -type c -name 'video*' -print 2>/dev/null |
      sort -V |
      head -n 1
  )"
fi
LATEST_FRAME_PATH="$(
  read_config_value LATEST_FRAME_PATH /run/smart-guard/latest.jpg
)"
HTTPS_PORT="$(read_config_value HTTPS_PORT 443)"
FRAME_MAX_AGE_SEC="$(read_config_value FRAME_MAX_AGE_SEC 5)"
HTTPS_URL="https://${BOARD_HOST}"
[[ "${HTTPS_PORT}" == "443" ]] || HTTPS_URL="${HTTPS_URL}:${HTTPS_PORT}"

if [[ -n "${CAMERA_DEVICE}" && -c "${CAMERA_DEVICE}" ]]; then
  pass "Camera device exists (${CAMERA_DEVICE})"
else
  fail "Camera device does not exist (${CAMERA_DEVICE})"
fi

if systemctl is-active --quiet smart-guard-camera.service; then
  pass "smart-guard-camera.service is active"
else
  fail "smart-guard-camera.service is not active"
fi

if systemctl is-enabled --quiet smart-guard-camera.service; then
  pass "smart-guard-camera.service is enabled at boot"
else
  fail "smart-guard-camera.service is not enabled"
fi

if systemctl is-active --quiet smart-guard-vision.service; then
  pass "smart-guard-vision.service is active"
else
  fail "smart-guard-vision.service is not active"
fi

if [[ -s "${LATEST_FRAME_PATH}" ]]; then
  frame_age=$(( $(date +%s) - $(stat -c %Y "${LATEST_FRAME_PATH}") ))
  if (( frame_age >= 0 && frame_age <= FRAME_MAX_AGE_SEC )); then
    pass "Latest camera frame is fresh (${frame_age}s old)"
  else
    fail "Camera frame is stale (${frame_age}s old)"
  fi

  jpeg_magic="$(od -An -tx1 -N2 "${LATEST_FRAME_PATH}" | tr -d '[:space:]')"
  if [[ "${jpeg_magic}" == "ffd8" ]]; then
    pass "Latest frame has a valid JPEG header"
  else
    fail "Latest frame is not a JPEG"
  fi
else
  fail "Latest camera frame is missing (${LATEST_FRAME_PATH})"
fi

frame_headers="$(
  curl -k -sS --max-time 8 -o /dev/null -D - \
    "${HTTPS_URL}/api/v1/stream?frames=1" 2>/dev/null
)"
frame_code="$(printf '%s\n' "${frame_headers}" | awk 'NR == 1 {print $2}')"
frame_type="$(
  printf '%s\n' "${frame_headers}" |
    awk 'BEGIN {IGNORECASE=1} /^Content-Type:/ {
      sub(/\r$/, "")
      print $2
      exit
    }'
)"

if [[ "${frame_code}" == "200" &&
      "${frame_type}" == "multipart/x-mixed-replace;" ]]; then
  pass "HTTPS camera endpoint returns an MJPEG stream"
else
  fail "Camera endpoint returned code=${frame_code:-none} type=${frame_type:-none}"
fi

echo
echo "Result: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[[ "${FAIL_COUNT}" -eq 0 ]]
