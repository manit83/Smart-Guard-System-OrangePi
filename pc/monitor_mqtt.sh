#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "Usage: ./pc/monitor_mqtt.sh STUDENT_ID [USERNAME] [PORT]" >&2
  exit 2
fi

STUDENT_ID=$1
MQTT_USERNAME=${2:-smartguard}
MQTT_PORT=${3:-1883}

if [[ ! "${STUDENT_ID}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "Invalid student ID." >&2
  exit 1
fi

read -r -s -p "MQTT password: " MQTT_PASSWORD
echo
trap 'unset MQTT_PASSWORD' EXIT

mosquitto_sub \
  -h 127.0.0.1 \
  -p "${MQTT_PORT}" \
  -u "${MQTT_USERNAME}" \
  -P "${MQTT_PASSWORD}" \
  -q 1 \
  -v \
  -t "home/${STUDENT_ID}/#" \
  -t "alarm/${STUDENT_ID}/home"
