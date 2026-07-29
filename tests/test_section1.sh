#!/usr/bin/env bash
set -uo pipefail

CONFIG_PATH="${CONFIG_PATH:-/etc/smart-guard/config.env}"
BOARD_HOST="${BOARD_HOST:-127.0.0.1}"
RUN_RESTART_TEST=0
NETWORK_ONLY=0
PASS_COUNT=0
FAIL_COUNT=0

usage() {
  cat <<'EOF'
Usage:
  ./tests/test_section1.sh [--host IP_OR_NAME] [--network-only] [--restart-test]

The restart test sends SIGKILL to the web-server main process. Run that mode
with sudo on the board.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      BOARD_HOST="$2"
      shift 2
      ;;
    --restart-test)
      RUN_RESTART_TEST=1
      shift
      ;;
    --network-only)
      NETWORK_ONLY=1
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

HTTP_PORT="$(read_config_value HTTP_PORT 80)"
HTTPS_PORT="$(read_config_value HTTPS_PORT 443)"
STUDENT_ID="$(read_config_value STUDENT_ID STUDENT_ID)"

HTTP_URL="http://${BOARD_HOST}"
HTTPS_URL="https://${BOARD_HOST}"
[[ "${HTTP_PORT}" == "80" ]] || HTTP_URL="${HTTP_URL}:${HTTP_PORT}"
[[ "${HTTPS_PORT}" == "443" ]] || HTTPS_URL="${HTTPS_URL}:${HTTPS_PORT}"

echo "Testing ${HTTP_URL} and ${HTTPS_URL}"

HTTP_HEADERS="$(curl -sS --max-time 8 -o /dev/null -D - "${HTTP_URL}/" 2>/dev/null)"
HTTP_CODE="$(printf '%s\n' "${HTTP_HEADERS}" | awk 'NR == 1 {print $2}')"
LOCATION="$(printf '%s\n' "${HTTP_HEADERS}" |
  awk 'BEGIN {IGNORECASE=1} /^Location:/ {
    sub(/\r$/, "")
    print $2
    exit
  }')"

if [[ "${HTTP_CODE}" == "301" ]]; then
  pass "HTTP returns status 301"
else
  fail "Expected HTTP 301, received ${HTTP_CODE:-no response}"
fi

if [[ "${LOCATION}" == https://* ]]; then
  pass "HTTP Location header points to HTTPS (${LOCATION})"
else
  fail "Missing or invalid HTTPS Location header"
fi

HTTPS_CODE="$(curl -k -sS --max-time 8 -o /dev/null -w '%{http_code}' "${HTTPS_URL}/" 2>/dev/null)"
if [[ "${HTTPS_CODE}" == "200" ]]; then
  pass "HTTPS dashboard returns status 200"
else
  fail "Expected HTTPS 200, received ${HTTPS_CODE:-no response}"
fi

STATUS_BODY="$(
  curl -k -sS --max-time 8 "${HTTPS_URL}/api/v1/telemetry" 2>/dev/null
)"
if printf '%s' "${STATUS_BODY}" |
    grep -Eq '"temperature_c":(null|-?[0-9]+(\.[0-9]+)?)' &&
   printf '%s' "${STATUS_BODY}" |
    grep -Eq '"memory_available_mb":(null|[0-9]+(\.[0-9]+)?)' &&
   printf '%s' "${STATUS_BODY}" |
    grep -Eq '"cpu_percent":(null|[0-9]+(\.[0-9]+)?)'; then
  pass "Internal status contains CPU, memory, and temperature fields"
else
  fail "Internal status JSON is missing telemetry fields"
fi

if printf '%s' "${STATUS_BODY}" |
    grep -Fq "\"student_id\":\"${STUDENT_ID}\""; then
  pass "Dashboard status contains student ID ${STUDENT_ID}"
else
  fail "Student ID is missing from dashboard status"
fi

CERT_SUBJECT="$(
  timeout 8 openssl s_client \
    -connect "${BOARD_HOST}:${HTTPS_PORT}" \
    -servername "${BOARD_HOST}" </dev/null 2>/dev/null |
    openssl x509 -noout -subject -nameopt RFC2253 2>/dev/null
)"
if printf '%s' "${CERT_SUBJECT}" | grep -Fq "CN=${STUDENT_ID}"; then
  pass "Certificate CN is ${STUDENT_ID}"
else
  fail "Certificate CN does not match ${STUDENT_ID}: ${CERT_SUBJECT:-unavailable}"
fi

if [[ "${NETWORK_ONLY}" -eq 0 ]]; then
  if command -v systemctl >/dev/null 2>&1 &&
     systemctl is-active --quiet smart-guard-web.service; then
    pass "smart-guard-web.service is active"
  else
    fail "smart-guard-web.service is not active"
  fi

  if command -v systemctl >/dev/null 2>&1 &&
     systemctl is-enabled --quiet smart-guard-web.service; then
    pass "smart-guard-web.service is enabled at boot"
  else
    fail "smart-guard-web.service is not enabled"
  fi
fi

if [[ "${RUN_RESTART_TEST}" -eq 1 ]]; then
  if [[ "${NETWORK_ONLY}" -eq 1 ]]; then
    fail "Restart test cannot be combined with --network-only"
  elif [[ "${EUID}" -ne 0 ]]; then
    fail "Restart test requires sudo"
  else
    OLD_PID="$(systemctl show -p MainPID --value smart-guard-web.service)"
    if [[ "${OLD_PID}" =~ ^[1-9][0-9]*$ ]]; then
      kill -9 "${OLD_PID}"
      NEW_PID=""
      for _ in {1..12}; do
        sleep 1
        NEW_PID="$(systemctl show -p MainPID --value smart-guard-web.service)"
        if [[ "${NEW_PID}" =~ ^[1-9][0-9]*$ &&
              "${NEW_PID}" != "${OLD_PID}" &&
              "$(systemctl is-active smart-guard-web.service)" == "active" ]]; then
          break
        fi
      done

      if [[ "${NEW_PID}" =~ ^[1-9][0-9]*$ && "${NEW_PID}" != "${OLD_PID}" ]]; then
        pass "systemd restarted the SIGKILLed process (${OLD_PID} -> ${NEW_PID})"
      else
        fail "systemd did not restart the killed process"
      fi
    else
      fail "Could not find the web-server MainPID"
    fi
  fi
fi

echo
echo "Result: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[[ "${FAIL_COUNT}" -eq 0 ]]
