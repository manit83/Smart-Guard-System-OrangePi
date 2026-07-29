#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
CONFIG_SOURCE="${PROJECT_DIR}/config/config.env"
START_SERVICES=1
INSTALL_DEPENDENCIES=1

usage() {
  cat <<'EOF'
Usage:
  sudo ./scripts/install.sh [options]

Options:
  --config PATH  Use PATH as the Smart Guard configuration file.
  --no-start     Install and enable every service without starting it.
  --skip-deps    Do not run apt-get; require dependencies to be installed.
  -h, --help     Show this help.

If config/config.env is absent but /etc/smart-guard/config.env exists, the
installed configuration is preserved and reused.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      CONFIG_SOURCE="$(realpath "$2")"
      shift 2
      ;;
    --no-start)
      START_SERVICES=0
      shift
      ;;
    --skip-deps)
      INSTALL_DEPENDENCIES=0
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

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this installer with sudo." >&2
  exit 1
fi

if [[ ! -r "${CONFIG_SOURCE}" &&
      -r /etc/smart-guard/config.env &&
      "${CONFIG_SOURCE}" == "${PROJECT_DIR}/config/config.env" ]]; then
  CONFIG_SOURCE=/etc/smart-guard/config.env
fi

if [[ ! -r "${CONFIG_SOURCE}" ]]; then
  cat >&2 <<EOF
Configuration file not found: ${CONFIG_SOURCE}

Create it first:
  cp "${PROJECT_DIR}/config/config.example.env" \
     "${PROJECT_DIR}/config/config.env"
  nano "${PROJECT_DIR}/config/config.env"
EOF
  exit 1
fi

if [[ "${INSTALL_DEPENDENCIES}" -eq 1 ]]; then
  command -v apt-get >/dev/null 2>&1 || {
    echo "This installer expects an Ubuntu/Debian system with apt-get." >&2
    exit 1
  }

  echo "Installing build and runtime dependencies..."
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    pkg-config \
    libssl-dev \
    openssl \
    libcurl4-openssl-dev \
    libmosquitto-dev \
    libsqlite3-dev \
    sqlite3 \
    ca-certificates \
    python3 \
    python3-opencv \
    python3-numpy \
    opencv-data \
    v4l-utils \
    ffmpeg \
    curl
fi

required_commands=(
  awk
  gcc
  getent
  groupadd
  install
  journalctl
  make
  mv
  openssl
  pkg-config
  python3
  realpath
  sed
  systemctl
  systemd-analyze
  useradd
  usermod
)

for command_name in "${required_commands[@]}"; do
  command -v "${command_name}" >/dev/null 2>&1 || {
    echo "Missing required command: ${command_name}" >&2
    exit 1
  }
done

for package_name in libcurl libmosquitto sqlite3; do
  pkg-config --exists "${package_name}" || {
    echo "Missing development dependency: ${package_name}" >&2
    echo "Run the installer again without --skip-deps." >&2
    exit 1
  }
done

read_config_value() {
  local key=$1
  local fallback=${2:-}
  local value

  value="$(
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
    ' "${CONFIG_SOURCE}"
  )"

  printf '%s\n' "${value:-${fallback}}"
}

STUDENT_ID="$(read_config_value STUDENT_ID)"
STUDENT_NAME="$(read_config_value STUDENT_NAME)"
VISION_PYTHON="$(read_config_value VISION_PYTHON /usr/bin/python3)"
CAMERA_SOURCE="$(read_config_value CAMERA_SOURCE v4l2)"
CAMERA_BACKEND="$(read_config_value CAMERA_BACKEND auto)"

if [[ -z "${STUDENT_ID}" ||
      "${STUDENT_ID}" == "YOUR_STUDENT_ID" ||
      ! "${STUDENT_ID}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "Set a valid STUDENT_ID in ${CONFIG_SOURCE}." >&2
  exit 1
fi

if [[ -z "${STUDENT_NAME}" ||
      "${STUDENT_NAME}" == "YOUR FULL NAME" ]]; then
  echo "Set STUDENT_NAME in ${CONFIG_SOURCE}." >&2
  exit 1
fi

if [[ ! -x "${VISION_PYTHON}" ]]; then
  echo "VISION_PYTHON is not executable: ${VISION_PYTHON}" >&2
  exit 1
fi

if ! "${VISION_PYTHON}" -c 'import cv2, numpy' >/dev/null 2>&1; then
  echo "OpenCV or numpy is unavailable to ${VISION_PYTHON}." >&2
  echo "Run the installer again without --skip-deps." >&2
  exit 1
fi

if ! getent group video >/dev/null; then
  echo "The video group does not exist on this board." >&2
  exit 1
fi

CAMERA_SOURCE="${CAMERA_SOURCE,,}"
CAMERA_BACKEND="${CAMERA_BACKEND,,}"
if [[ "${CAMERA_SOURCE}" == "v4l2" &&
      ( "${CAMERA_BACKEND}" == "auto" ||
        "${CAMERA_BACKEND}" == "ffmpeg" ) &&
      ! -x /usr/bin/ffmpeg ]]; then
  echo "The selected camera backend requires ffmpeg." >&2
  exit 1
fi

if [[ "${CAMERA_SOURCE}" == "onvif" ]] &&
   ! "${VISION_PYTHON}" -c 'import onvif' >/dev/null 2>&1; then
  echo "CAMERA_SOURCE=onvif requires the onvif-zeep Python package." >&2
  exit 1
fi

required_project_files=(
  Makefile
  README.md
  docs/SECTION2_TESTS.md
  src/mongoose.c
  include/mongoose.h
  src/web_server.c
  src/command_runner.c
  src/notifier.c
  src/watchdog.c
  src/thermal_manager.c
  vision/vision_app.py
  web/index.html
  web/style.css
  web/dashboard.js
  web/openapi.yaml
  web/docs/index.html
  web/docs/swagger-ui.css
  web/docs/swagger-ui-bundle.js
  web/docs/swagger-ui-standalone-preset.js
  web/docs/swagger-initializer.js
  systemd/smart-guard-command.service
  systemd/smart-guard-web.service
  systemd/smart-guard-camera.service
  systemd/smart-guard-vision.service
  systemd/smart-guard-notifier.service
  systemd/smart-guard-watchdog.service
  systemd/smart-guard-thermal-manager.service
  systemd/smart-guard-vision.service.d/thermal-override.conf
  THIRD_PARTY_LICENSES/MONGOOSE_LICENSE
  THIRD_PARTY_LICENSES/SWAGGER_UI_LICENSE
  THIRD_PARTY_LICENSES/SWAGGER_UI_NOTICE
)

for relative_path in "${required_project_files[@]}"; do
  if [[ ! -s "${PROJECT_DIR}/${relative_path}" ]]; then
    echo "Missing project file: ${relative_path}" >&2
    exit 1
  fi
done

echo "Building all Smart Guard programs..."
make -C "${PROJECT_DIR}" all

if ! getent group smartguard >/dev/null; then
  groupadd --system smartguard
fi

if ! getent passwd smartguard >/dev/null; then
  useradd \
    --system \
    --gid smartguard \
    --home-dir /nonexistent \
    --no-create-home \
    --shell /usr/sbin/nologin \
    smartguard
fi

usermod -aG video smartguard

install -d -o root -g smartguard -m 0750 /etc/smart-guard
install -d -o root -g smartguard -m 0750 /etc/smart-guard/tls
install -d -o root -g root -m 0755 /opt/smart-guard
install -d -o root -g root -m 0755 /opt/smart-guard/docs
install -d -o root -g root -m 0755 /opt/smart-guard/bin
install -d -o root -g root -m 0755 /opt/smart-guard/vision
install -d -o root -g root -m 0755 /opt/smart-guard/web
install -d -o root -g root -m 0755 /opt/smart-guard/web/docs
install -d -o root -g root -m 0755 /opt/smart-guard/THIRD_PARTY_LICENSES
install -d -o root -g smartguard -m 0770 /var/lib/smart-guard
install -d -o root -g root -m 0755 \
  /etc/systemd/system/smart-guard-vision.service.d

if [[ "$(realpath "${CONFIG_SOURCE}")" != "/etc/smart-guard/config.env" ]]; then
  install -o root -g smartguard -m 0640 \
    "${CONFIG_SOURCE}" \
    /etc/smart-guard/config.env
else
  chown root:smartguard /etc/smart-guard/config.env
  chmod 0640 /etc/smart-guard/config.env
fi

for secret_path in \
  /etc/smart-guard/smtp-password \
  /etc/smart-guard/mqtt-password; do
  if [[ ! -e "${secret_path}" ]]; then
    install -o root -g smartguard -m 0640 /dev/null "${secret_path}"
  else
    chown root:smartguard "${secret_path}"
    chmod 0640 "${secret_path}"
  fi
done

atomic_install() {
  local source=$1
  local destination=$2
  local mode=$3
  local temporary="${destination}.smart-guard-new"

  install -o root -g root -m "${mode}" "${source}" "${temporary}"
  mv -f -- "${temporary}" "${destination}"
}

for binary_name in \
  smart-guard-command \
  smart-guard-web \
  smart-guard-notifier \
  smart-guard-watchdog \
  smart-guard-thermal-manager; do
  atomic_install \
    "${PROJECT_DIR}/build/${binary_name}" \
    "/opt/smart-guard/bin/${binary_name}" \
    0755
done

for helper_name in camera_probe.sh camera_ready.sh vision_start.sh; do
  atomic_install \
    "${PROJECT_DIR}/scripts/${helper_name}" \
    "/opt/smart-guard/bin/${helper_name}" \
    0755
done

atomic_install \
  "${PROJECT_DIR}/vision/vision_app.py" \
  /opt/smart-guard/vision/vision_app.py \
  0644

for asset_name in index.html style.css dashboard.js openapi.yaml; do
  atomic_install \
    "${PROJECT_DIR}/web/${asset_name}" \
    "/opt/smart-guard/web/${asset_name}" \
    0644
done

for asset_name in \
  index.html \
  swagger-ui.css \
  swagger-ui-bundle.js \
  swagger-ui-standalone-preset.js \
  swagger-initializer.js; do
  atomic_install \
    "${PROJECT_DIR}/web/docs/${asset_name}" \
    "/opt/smart-guard/web/docs/${asset_name}" \
    0644
done

atomic_install \
  "${PROJECT_DIR}/README.md" \
  /opt/smart-guard/README.md \
  0644
atomic_install \
  "${PROJECT_DIR}/docs/SECTION2_TESTS.md" \
  /opt/smart-guard/docs/SECTION2_TESTS.md \
  0644
atomic_install \
  "${PROJECT_DIR}/THIRD_PARTY_LICENSES/MONGOOSE_LICENSE" \
  /opt/smart-guard/THIRD_PARTY_LICENSES/MONGOOSE_LICENSE \
  0644
atomic_install \
  "${PROJECT_DIR}/THIRD_PARTY_LICENSES/SWAGGER_UI_LICENSE" \
  /opt/smart-guard/THIRD_PARTY_LICENSES/SWAGGER_UI_LICENSE \
  0644
atomic_install \
  "${PROJECT_DIR}/THIRD_PARTY_LICENSES/SWAGGER_UI_NOTICE" \
  /opt/smart-guard/THIRD_PARTY_LICENSES/SWAGGER_UI_NOTICE \
  0644

CERTIFICATE_ID="$(
  openssl x509 \
    -in /etc/smart-guard/tls/server.crt \
    -noout \
    -subject \
    -nameopt RFC2253 2>/dev/null |
    sed -n 's/^subject=CN=//p'
)" || true

if [[ ! -r /etc/smart-guard/tls/server.crt ||
      ! -r /etc/smart-guard/tls/server.key ||
      "${CERTIFICATE_ID}" != "${STUDENT_ID}" ]]; then
  TEMP_TLS_DIR="$(mktemp -d)"
  trap 'rm -rf -- "${TEMP_TLS_DIR:-}"' EXIT
  "${PROJECT_DIR}/scripts/generate_tls_cert.sh" \
    /etc/smart-guard/config.env \
    "${TEMP_TLS_DIR}"
  install -o root -g smartguard -m 0640 \
    "${TEMP_TLS_DIR}/server.crt" \
    /etc/smart-guard/tls/server.crt
  install -o root -g smartguard -m 0640 \
    "${TEMP_TLS_DIR}/server.key" \
    /etc/smart-guard/tls/server.key
  rm -rf -- "${TEMP_TLS_DIR}"
  trap - EXIT
fi

for unit_name in \
  smart-guard-command.service \
  smart-guard-web.service \
  smart-guard-camera.service \
  smart-guard-vision.service \
  smart-guard-notifier.service \
  smart-guard-watchdog.service \
  smart-guard-thermal-manager.service; do
  atomic_install \
    "${PROJECT_DIR}/systemd/${unit_name}" \
    "/etc/systemd/system/${unit_name}" \
    0644
done

atomic_install \
  "${PROJECT_DIR}/systemd/smart-guard-vision.service.d/thermal-override.conf" \
  /etc/systemd/system/smart-guard-vision.service.d/thermal-override.conf \
  0644

if [[ ! -e /var/lib/smart-guard/guard-mode ]]; then
  install -o root -g smartguard -m 0660 /dev/null \
    /var/lib/smart-guard/guard-mode
  printf '0\n' > /var/lib/smart-guard/guard-mode
fi
chown root:smartguard /var/lib/smart-guard/guard-mode
chmod 0660 /var/lib/smart-guard/guard-mode

systemd-analyze verify \
  /etc/systemd/system/smart-guard-command.service \
  /etc/systemd/system/smart-guard-camera.service \
  /etc/systemd/system/smart-guard-vision.service \
  /etc/systemd/system/smart-guard-watchdog.service \
  /etc/systemd/system/smart-guard-notifier.service \
  /etc/systemd/system/smart-guard-thermal-manager.service \
  /etc/systemd/system/smart-guard-web.service

service_names=(
  smart-guard-command.service
  smart-guard-camera.service
  smart-guard-vision.service
  smart-guard-watchdog.service
  smart-guard-notifier.service
  smart-guard-thermal-manager.service
  smart-guard-web.service
)

systemctl daemon-reload
systemctl enable "${service_names[@]}"

start_service() {
  local service_name=$1

  if ! systemctl restart "${service_name}"; then
    journalctl -u "${service_name}" -n 80 --no-pager || true
    exit 1
  fi
  if ! systemctl is-active --quiet "${service_name}"; then
    journalctl -u "${service_name}" -n 80 --no-pager || true
    exit 1
  fi
}

if [[ "${START_SERVICES}" -eq 1 ]]; then
  echo "Starting Smart Guard services..."
  for service_name in "${service_names[@]}"; do
    start_service "${service_name}"
  done

  echo
  systemctl --no-pager --full status "${service_names[@]}"
else
  echo
  echo "All files and services were installed and enabled."
  echo "Configure secrets, then start the services with:"
  echo "  sudo systemctl start ${service_names[*]}"
fi

PUBLIC_HOST="$(read_config_value BOARD_PUBLIC_HOST)"
if [[ -z "${PUBLIC_HOST}" ]]; then
  PUBLIC_HOST="$(hostname -I 2>/dev/null | awk '{print $1}')"
fi

echo
echo "Smart Guard installation completed."
if [[ -n "${PUBLIC_HOST}" ]]; then
  echo "Dashboard: https://${PUBLIC_HOST}/"
  echo "Swagger UI: https://${PUBLIC_HOST}/docs/"
fi
echo "Configuration: /etc/smart-guard/config.env"
echo "Runtime data:  /run/smart-guard"
echo "Persistent data: /var/lib/smart-guard"
