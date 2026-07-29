#!/usr/bin/env bash
set -euo pipefail

STUDENT_ID=
BOARD_IP=
MQTT_USERNAME=smartguard
MQTT_PORT=1883
CONFIG_PATH=/etc/mosquitto/conf.d/smart-guard.conf
PASSWORD_PATH=/etc/mosquitto/passwd-smart-guard

usage() {
  cat <<'EOF'
Usage:
  sudo ./pc/setup_mosquitto_pc.sh \
      --student-id STUDENT_ID \
      --board-ip BOARD_LAN_IP \
      [--username smartguard] [--port 1883]
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --student-id)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      STUDENT_ID=$2
      shift 2
      ;;
    --board-ip)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      BOARD_IP=$2
      shift 2
      ;;
    --username)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      MQTT_USERNAME=$2
      shift 2
      ;;
    --port)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      MQTT_PORT=$2
      shift 2
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

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this script with sudo on the personal computer." >&2
  exit 1
fi
if [[ ! "${STUDENT_ID}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "Invalid or missing --student-id." >&2
  exit 1
fi
if [[ ! "${BOARD_IP}" =~ ^[0-9A-Fa-f:.]+$ ]]; then
  echo "Invalid or missing --board-ip." >&2
  exit 1
fi
if [[ ! "${MQTT_USERNAME}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "Invalid MQTT username." >&2
  exit 1
fi
if [[ ! "${MQTT_PORT}" =~ ^[0-9]+$ ]] ||
   (( MQTT_PORT < 1 || MQTT_PORT > 65535 )); then
  echo "Invalid MQTT port." >&2
  exit 1
fi

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
  mosquitto \
  mosquitto-clients

install -d -o root -g root -m 0755 /etc/mosquitto/conf.d

echo "Create the broker password for user ${MQTT_USERNAME}."
if [[ -s "${PASSWORD_PATH}" ]]; then
  mosquitto_passwd "${PASSWORD_PATH}" "${MQTT_USERNAME}"
else
  mosquitto_passwd -c "${PASSWORD_PATH}" "${MQTT_USERNAME}"
fi
chown root:mosquitto "${PASSWORD_PATH}"
chmod 0640 "${PASSWORD_PATH}"

TEMPORARY="$(mktemp)"
trap 'rm -f -- "${TEMPORARY:-}"' EXIT
{
  echo "per_listener_settings true"
  echo "listener ${MQTT_PORT} 0.0.0.0"
  echo "allow_anonymous false"
  echo "password_file ${PASSWORD_PATH}"
  echo "persistence true"
  echo "persistence_location /var/lib/mosquitto/"
} > "${TEMPORARY}"
install -o root -g root -m 0644 "${TEMPORARY}" "${CONFIG_PATH}"
rm -f -- "${TEMPORARY}"
trap - EXIT

if ! grep -qE '^[[:space:]]*include_dir[[:space:]]+/etc/mosquitto/conf.d([[:space:]]|$)' \
     /etc/mosquitto/mosquitto.conf; then
  echo "include_dir /etc/mosquitto/conf.d" >> /etc/mosquitto/mosquitto.conf
fi

systemctl enable --now mosquitto
systemctl restart mosquitto
systemctl --no-pager --full status mosquitto

if command -v ufw >/dev/null 2>&1 &&
   ufw status | grep -q '^Status: active'; then
  ufw allow from "${BOARD_IP}" to any port "${MQTT_PORT}" proto tcp
fi

PC_IP="$(hostname -I | awk '{print $1}')"
echo
echo "Mosquitto is ready."
echo "PC LAN IP: ${PC_IP}"
echo "Board config:"
echo "  MQTT_BROKER_HOST=${PC_IP}"
echo "  MQTT_BROKER_PORT=${MQTT_PORT}"
echo "  MQTT_USERNAME=${MQTT_USERNAME}"
echo "Required topic filter: home/${STUDENT_ID}/#"
