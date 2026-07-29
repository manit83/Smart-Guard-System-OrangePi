#!/usr/bin/env bash
set -euo pipefail

MONGOOSE_VERSION="7.22"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
TEMP_DIR="$(mktemp -d)"

cleanup() {
  rm -rf -- "${TEMP_DIR}"
}
trap cleanup EXIT

BASE_URL="https://raw.githubusercontent.com/cesanta/mongoose/refs/tags/${MONGOOSE_VERSION}"

echo "Downloading Mongoose ${MONGOOSE_VERSION} from the official repository..."
curl -fL --retry 3 --connect-timeout 20 \
  "${BASE_URL}/mongoose.c" \
  -o "${TEMP_DIR}/mongoose.c"
curl -fL --retry 3 --connect-timeout 20 \
  "${BASE_URL}/mongoose.h" \
  -o "${TEMP_DIR}/mongoose.h"
curl -fL --retry 3 --connect-timeout 20 \
  "${BASE_URL}/LICENSE" \
  -o "${TEMP_DIR}/MONGOOSE_LICENSE"

grep -q "Mongoose" "${TEMP_DIR}/mongoose.c"
grep -q "mg_http_listen" "${TEMP_DIR}/mongoose.h"
grep -q "MG_TLS_OPENSSL" "${TEMP_DIR}/mongoose.h"
test -s "${TEMP_DIR}/MONGOOSE_LICENSE"

install -m 0644 "${TEMP_DIR}/mongoose.c" "${PROJECT_DIR}/src/mongoose.c"
install -m 0644 "${TEMP_DIR}/mongoose.h" "${PROJECT_DIR}/include/mongoose.h"
install -d -m 0755 "${PROJECT_DIR}/THIRD_PARTY_LICENSES"
install -m 0644 \
  "${TEMP_DIR}/MONGOOSE_LICENSE" \
  "${PROJECT_DIR}/THIRD_PARTY_LICENSES/MONGOOSE_LICENSE"

echo "Installed:"
echo "  ${PROJECT_DIR}/src/mongoose.c"
echo "  ${PROJECT_DIR}/include/mongoose.h"
echo "  ${PROJECT_DIR}/THIRD_PARTY_LICENSES/MONGOOSE_LICENSE"
