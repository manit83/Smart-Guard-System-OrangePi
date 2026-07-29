#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
CONFIG_PATH="${1:-${PROJECT_DIR}/config/config.env}"
OUTPUT_DIR="${2:-${PROJECT_DIR}/build/tls}"
DAYS="${TLS_CERT_DAYS:-365}"

read_config_value() {
  local key="$1"
  awk -F= -v wanted="${key}" '
    $1 == wanted {
      value = substr($0, index($0, "=") + 1)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
      if (value ~ /^".*"$/ || value ~ /^\047.*\047$/) {
        value = substr(value, 2, length(value) - 2)
      }
      print value
      exit
    }
  ' "${CONFIG_PATH}"
}

if [[ ! -r "${CONFIG_PATH}" ]]; then
  echo "Config file is not readable: ${CONFIG_PATH}" >&2
  exit 1
fi

STUDENT_ID="$(read_config_value STUDENT_ID)"
if [[ -z "${STUDENT_ID}" || "${STUDENT_ID}" == "YOUR_STUDENT_ID" ]]; then
  echo "Set a real STUDENT_ID in ${CONFIG_PATH} first." >&2
  exit 1
fi

if [[ ! "${STUDENT_ID}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "STUDENT_ID contains unsupported characters: ${STUDENT_ID}" >&2
  exit 1
fi

if [[ ! "${DAYS}" =~ ^[0-9]+$ || "${DAYS}" -lt 1 ]]; then
  echo "TLS_CERT_DAYS must be a positive integer." >&2
  exit 1
fi

install -d -m 0750 "${OUTPUT_DIR}"

openssl req \
  -x509 \
  -newkey rsa:2048 \
  -sha256 \
  -nodes \
  -days "${DAYS}" \
  -keyout "${OUTPUT_DIR}/server.key" \
  -out "${OUTPUT_DIR}/server.crt" \
  -subj "/CN=${STUDENT_ID}" \
  -addext "subjectAltName=DNS:${STUDENT_ID}" \
  -addext "keyUsage=digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth"

chmod 0600 "${OUTPUT_DIR}/server.key"
chmod 0644 "${OUTPUT_DIR}/server.crt"

echo
echo "Certificate created:"
openssl x509 -in "${OUTPUT_DIR}/server.crt" \
  -noout -subject -issuer -dates -fingerprint -sha256
echo "Key:         ${OUTPUT_DIR}/server.key"
echo "Certificate: ${OUTPUT_DIR}/server.crt"
