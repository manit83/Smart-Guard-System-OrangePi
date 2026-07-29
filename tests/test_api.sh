#!/usr/bin/env bash
set -uo pipefail

BOARD_HOST="${BOARD_HOST:-127.0.0.1}"
HTTPS_PORT="${HTTPS_PORT:-443}"
RUN_REBOOT_TEST=0
PASS_COUNT=0
FAIL_COUNT=0

usage() {
  cat <<'EOF'
Usage:
  ./tests/test_api.sh [--host IP_OR_NAME] [--port PORT] [--reboot-test]

The optional reboot test is destructive to the current session and always runs
last. The normal test set validates the command endpoint with an unsupported
command and does not reboot the board.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      BOARD_HOST=$2
      shift 2
      ;;
    --port)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      HTTPS_PORT=$2
      shift 2
      ;;
    --reboot-test)
      RUN_REBOOT_TEST=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

BASE_URL="https://${BOARD_HOST}"
[[ "${HTTPS_PORT}" == "443" ]] ||
  BASE_URL="${BASE_URL}:${HTTPS_PORT}"

pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  printf '[PASS] %s\n' "$1"
}

fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  printf '[FAIL] %s\n' "$1" >&2
}

expect_code() {
  local description=$1
  local expected=$2
  shift 2
  local actual

  actual="$(curl -k -sS --max-time 12 -o /dev/null -w '%{http_code}' "$@" \
    2>/dev/null)"
  if [[ "${actual}" == "${expected}" ]]; then
    pass "${description} returns HTTP ${expected}"
  else
    fail "${description} expected HTTP ${expected}, received ${actual:-none}"
  fi
}

echo "Testing Smart Guard API at ${BASE_URL}"

expect_code "Swagger UI" 200 "${BASE_URL}/docs/"
expect_code "OpenAPI document" 200 "${BASE_URL}/openapi.yaml"

TELEMETRY="$(
  curl -k -sS --max-time 12 "${BASE_URL}/api/v1/telemetry" 2>/dev/null
)"
if printf '%s' "${TELEMETRY}" |
    grep -Eq '"temperature_c":(null|-?[0-9]+(\.[0-9]+)?)' &&
   printf '%s' "${TELEMETRY}" |
    grep -Eq '"memory_available_mb":(null|[0-9]+(\.[0-9]+)?)' &&
   printf '%s' "${TELEMETRY}" |
    grep -Eq '"cpu_percent":(null|[0-9]+(\.[0-9]+)?)'; then
  pass "Telemetry contains direct CPU, memory, and temperature values"
else
  fail "Telemetry JSON is missing required fields"
fi

PERSONS="$(
  curl -k -sS --max-time 12 "${BASE_URL}/api/v1/persons" 2>/dev/null
)"
if printf '%s' "${PERSONS}" |
    grep -Eq '"persons":[0-9]+' &&
   printf '%s' "${PERSONS}" |
    grep -Eq '"timestamp":"[^"]+"'; then
  pass "Persons response contains count and timestamp"
else
  fail "Persons endpoint is unavailable or malformed"
fi

STREAM_FILE="$(mktemp)"
trap 'rm -f -- "${STREAM_FILE}"' EXIT
STREAM_CODE="$(
  curl -k -sS --max-time 15 \
    -o "${STREAM_FILE}" \
    -w '%{http_code}' \
    "${BASE_URL}/api/v1/stream?frames=1" 2>/dev/null
)"
if [[ "${STREAM_CODE}" == "200" ]] &&
   grep -aq -- '--frame' "${STREAM_FILE}" &&
   grep -aq 'Content-Type: image/jpeg' "${STREAM_FILE}"; then
  pass "Finite MJPEG test returned one framed JPEG"
else
  fail "MJPEG stream test failed with HTTP ${STREAM_CODE:-none}"
fi

expect_code "History" 200 "${BASE_URL}/api/v1/history?limit=5"
expect_code "Guard mode" 200 "${BASE_URL}/api/v1/guard-mode"
expect_code \
  "Unsupported command validation" \
  422 \
  -H "Content-Type: application/json" \
  -d '{"cmd":"not-allowed"}' \
  "${BASE_URL}/api/v1/command"

if [[ "${RUN_REBOOT_TEST}" -eq 1 ]]; then
  expect_code \
    "Reboot command" \
    202 \
    -H "Content-Type: application/json" \
    -d '{"cmd":"reboot"}' \
    "${BASE_URL}/api/v1/command"
fi

echo
echo "Result: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[[ "${FAIL_COUNT}" -eq 0 ]]
