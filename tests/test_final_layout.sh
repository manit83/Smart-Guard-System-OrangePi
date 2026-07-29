#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

required=(
  Makefile
  README.md
  config/config.example.env
  docs/SECTION2_TESTS.md
  include/email_sender.h
  include/guard_state.h
  include/history_db.h
  include/mongoose.h
  include/mqtt_client.h
  include/smart_guard_notifier.h
  include/telemetry.h
  include/thermal_policy.h
  src/core.c
  src/email_sender.c
  src/command_runner.c
  src/guard_state.c
  src/history_db.c
  src/mongoose.c
  src/mqtt_client.c
  src/notifier.c
  src/telemetry.c
  src/thermal_manager.c
  src/thermal_policy.c
  src/watchdog.c
  src/web_server.c
  scripts/install.sh
  scripts/set_secret.sh
  tests/test_api.sh
  systemd/smart-guard-camera.service
  systemd/smart-guard-command.service
  systemd/smart-guard-notifier.service
  systemd/smart-guard-thermal-manager.service
  systemd/smart-guard-vision.service
  systemd/smart-guard-vision.service.d/thermal-override.conf
  systemd/smart-guard-watchdog.service
  systemd/smart-guard-web.service
  vision/vision_app.py
  web/dashboard.js
  web/index.html
  web/openapi.yaml
  web/style.css
  web/docs/index.html
  web/docs/swagger-ui.css
  web/docs/swagger-ui-bundle.js
  web/docs/swagger-ui-standalone-preset.js
  web/docs/swagger-initializer.js
)

for path in "${required[@]}"; do
  test -s "${ROOT}/${path}" || {
    echo "Missing final project file: ${path}" >&2
    exit 1
  }
done

legacy=(
  Makefile.section4
  Makefile.thermal
  scripts/install_section1.sh
  scripts/install_section3.sh
  scripts/install_section4.sh
  scripts/install_thermal_manager.sh
  config/section3.env
  config/section4.env
  config/thermal.env
)

for path in "${legacy[@]}"; do
  if [[ -e "${ROOT}/${path}" ]]; then
    echo "Legacy sectional file is still present: ${path}" >&2
    exit 1
  fi
done

for endpoint in stream persons telemetry command history guard-mode; do
  grep -q "/api/v1/${endpoint}" "${ROOT}/src/web_server.c"
  grep -q "/api/v1/${endpoint}" "${ROOT}/web/openapi.yaml"
done
grep -q 'multipart/x-mixed-replace' "${ROOT}/src/web_server.c"
grep -q 'VISION_DETECTOR must be none, face, or person_hog' \
  "${ROOT}/vision/vision_app.py"
grep -q 'alarm/%s/home' "${ROOT}/src/mqtt_client.c"
grep -q 'WATCHDOG_FRAME_TIMEOUT_SEC=30' \
  "${ROOT}/config/config.example.env"
grep -q 'THERMAL_HIGH_TEMP_C=70' \
  "${ROOT}/config/config.example.env"
grep -q 'THERMAL_RECOVERY_TEMP_C=60' \
  "${ROOT}/config/config.example.env"
grep -q 'ENABLED' "${ROOT}/web/dashboard.js"
grep -q 'font-family: Consolas' "${ROOT}/web/style.css"

echo "Final Smart Guard layout is complete."
