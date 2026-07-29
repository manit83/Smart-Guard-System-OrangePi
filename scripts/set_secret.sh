#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi

if [[ $# -ne 1 || ( "$1" != "smtp" && "$1" != "mqtt" ) ]]; then
  echo "Usage: sudo ./scripts/set_secret.sh smtp|mqtt" >&2
  exit 2
fi

case "$1" in
  smtp)
    TARGET=/etc/smart-guard/smtp-password
    LABEL="SMTP app password"
    ;;
  mqtt)
    TARGET=/etc/smart-guard/mqtt-password
    LABEL="MQTT password"
    ;;
esac

read -r -s -p "Enter ${LABEL}: " FIRST
echo
read -r -s -p "Enter it again: " SECOND
echo

if [[ -z "${FIRST}" ]]; then
  echo "The secret cannot be empty." >&2
  exit 1
fi
if [[ "${FIRST}" != "${SECOND}" ]]; then
  echo "The two values do not match." >&2
  exit 1
fi
if [[ "${FIRST}" == *$'\n'* || "${FIRST}" == *$'\r'* ]]; then
  echo "The secret cannot contain a line break." >&2
  exit 1
fi

install -d -o root -g smartguard -m 0750 /etc/smart-guard
TEMPORARY="$(mktemp /etc/smart-guard/.secret.XXXXXX)"
trap 'rm -f -- "${TEMPORARY:-}"' EXIT
printf '%s\n' "${FIRST}" > "${TEMPORARY}"
chown root:smartguard "${TEMPORARY}"
chmod 0640 "${TEMPORARY}"
mv -f -- "${TEMPORARY}" "${TARGET}"
trap - EXIT

unset FIRST SECOND
echo "Secret saved to ${TARGET} with mode 0640."
