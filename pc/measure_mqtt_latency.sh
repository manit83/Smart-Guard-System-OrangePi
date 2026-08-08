#!/usr/bin/env bash
set -euo pipefail

BOARD_IP="192.168.1.123"

BROKER_HOST="127.0.0.1"
BROKER_PORT="1883"
STUDENT_ID="402170408"
MQTT_USERNAME="smartguard"
TOPIC="home/${STUDENT_ID}/persons"

for command_name in mosquitto_sub sed date; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "Required command not found: ${command_name}" >&2
    exit 1
  fi
done

if command -v ping >/dev/null 2>&1; then
  if ping -c 1 -W 2 "${BOARD_IP}" >/dev/null 2>&1; then
    echo "Board is reachable at ${BOARD_IP}."
  else
    echo "Warning: board ${BOARD_IP} did not answer ping." >&2
    echo "The MQTT subscriber will still start; verify the board IP if no messages arrive." >&2
  fi
fi

read -r -s -p "MQTT password for ${MQTT_USERNAME}: " MQTT_PASSWORD
echo
trap 'unset MQTT_PASSWORD' EXIT

echo "Listening on ${TOPIC} via broker ${BROKER_HOST}:${BROKER_PORT} ..."
echo "Press Ctrl+C to stop."

mosquitto_sub \
  -h "${BROKER_HOST}" \
  -p "${BROKER_PORT}" \
  -u "${MQTT_USERNAME}" \
  -P "${MQTT_PASSWORD}" \
  -q 1 \
  -t "${TOPIC}" |
while IFS= read -r message; do
  # Record the PC time immediately after mosquitto_sub outputs the message.
  received_at_ns=$(date +%s%3N)
  received_at_ms=$((received_at_ns / 1000000))

  sent_at_ms=$(
    sed -nE \
      's/.*"sent_at_ms"[[:space:]]*:[[:space:]]*([0-9]+).*/\1/p' \
      <<< "${message}"
  )

  if [[ -z "${sent_at_ms}" ]]; then
    echo "Ignored message without sent_at_ms: ${message}" >&2
    continue
  fi

  persons=$(
    sed -nE \
      's/.*"persons"[[:space:]]*:[[:space:]]*([0-9]+).*/\1/p' \
      <<< "${message}"
  )
  persons=${persons:-unknown}

  latency_ms=$((received_at_ms - sent_at_ms))

  printf \
    'persons=%s sent_at_ms=%s received_at_ms=%s one_way_latency=%s ms\n' \
    "${persons}" \
    "${sent_at_ms}" \
    "${received_at_ms}" \
    "${latency_ms}"
done
