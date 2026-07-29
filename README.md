# Smart Guard

Smart Guard is the final Embedded Systems course project for the Department of
Electrical Engineering at Sharif University of Technology, instructed by
Dr. Gholampour in the second semester of 1404-1405.

The project runs on an Orange Pi Zero Plus H5 and provides a secure HTTPS
dashboard, a C REST API with local Swagger UI, live MJPEG streaming, OpenCV
person/face detection, C email and MQTT notifications, guard mode, SQLite
history, a software watchdog, and adaptive thermal management.

## 1. Project architecture

| Project section | Main implementation | systemd services |
|---|---|---|
| 1. Web server | HTTP-to-HTTPS redirect, self-signed TLS, dashboard | `smart-guard-web.service` |
| 2. RESTful API | MJPEG, persons, telemetry, command, history, Swagger UI | `smart-guard-web.service`, `smart-guard-command.service` |
| 3. Vision, email, MQTT | Camera input, OpenCV, C notifications, MQTT QoS 1 and LWT | `smart-guard-camera.service`, `smart-guard-vision.service`, `smart-guard-notifier.service` |
| 4. Final features | Guard mode, SQLite circular history, watchdog, thermal control | `smart-guard-watchdog.service`, `smart-guard-thermal-manager.service` |

All persistent settings are stored in:

```text
/etc/smart-guard/config.env
```

Runtime camera and telemetry files are stored in:

```text
/run/smart-guard/
```

Persistent state and SQLite data are stored in:

```text
/var/lib/smart-guard/
```

## 2. Source layout

```text
smart-guard/
├── config/
│   └── config.example.env
├── docs/
│   └── SECTION2_TESTS.md
├── include/
├── pc/
│   ├── monitor_mqtt.sh
│   └── setup_mosquitto_pc.sh
├── scripts/
│   ├── install.sh
│   ├── set_secret.sh
│   ├── camera_probe.sh
│   ├── camera_ready.sh
│   ├── generate_tls_cert.sh
│   └── vision_start.sh
├── src/
│   ├── web_server.c
│   ├── command_runner.c
│   └── ...
├── systemd/
├── tests/
│   ├── test_api.sh
│   └── ...
├── vision/
│   └── vision_app.py
├── web/
│   ├── docs/
│   │   └── local Swagger UI assets
│   ├── index.html
│   ├── style.css
│   ├── dashboard.js
│   └── openapi.yaml
├── Makefile
└── README.md
```

## 3. Configure the project on the Ubuntu PC

Keep the primary source copy on the Ubuntu PC:

```bash
cd ~/smart-guard
cp config/config.example.env config/config.env
nano config/config.env
```

At minimum, set:

```ini
STUDENT_ID=402XXXXXX
STUDENT_NAME="Your Full Name"
BOARD_PUBLIC_HOST=192.168.1.123
```

`BOARD_PUBLIC_HOST` is the Orange Pi address used by browsers. Leave
`BOARD_BIND_IP=0.0.0.0` unchanged unless a specific interface must be used.

Passwords are not committed to `config.env`. SMTP and MQTT secrets are stored
in protected files under `/etc/smart-guard`.

## 4. Section 1 - Secure web server

The C web server provides:

- HTTP on port 80 with a permanent `301` redirect to HTTPS.
- HTTPS on port 443.
- A self-signed certificate whose common name is `STUDENT_ID`.
- A local HTML, CSS, and JavaScript dashboard.
- CPU temperature, CPU load, available memory, uptime, camera state, and
  detection state.
- Automatic recovery through `Restart=on-failure`.

Relevant configuration:

```ini
BOARD_BIND_IP=0.0.0.0
BOARD_PUBLIC_HOST=192.168.1.123
HTTP_PORT=80
HTTPS_PORT=443
TELEMETRY_INTERVAL_SEC=2
FRAME_MAX_AGE_SEC=5
```

Open the dashboard:

```text
https://BOARD_IP/
```

A browser warning is expected because the certificate is self-signed.

## 5. Section 2 - RESTful API and live monitoring

All required endpoints are implemented directly in `src/web_server.c`.
Temperature and memory are read directly by C code from `/sys` and `/proc`;
the API does not run Linux shell commands to collect telemetry.

| Method | Endpoint | Function |
|---|---|---|
| `GET` | `/api/v1/stream` | Continuous annotated MJPEG stream |
| `GET` | `/api/v1/persons` | Current person count and timestamp |
| `GET` | `/api/v1/telemetry` | CPU temperature, available memory, CPU load, and system state |
| `POST` | `/api/v1/command` | Extensible, allowlisted command API; currently supports `reboot` |
| `GET` | `/api/v1/history?limit=5` | Five newest SQLite detection records by default |
| `GET/POST` | `/api/v1/guard-mode` | Read or change persistent guard mode |

### 5.1 Live MJPEG stream

Open the continuous stream directly:

```text
https://BOARD_IP/api/v1/stream
```

For tests that must finish automatically, request a finite number of frames:

```bash
curl -k \
  "https://BOARD_IP/api/v1/stream?frames=3" \
  --output stream-sample.mjpeg
```

The response type is:

```text
multipart/x-mixed-replace; boundary=frame
```

`MJPEG_STREAM_FPS` limits how frequently the C server sends a newly published
JPEG:

```ini
MJPEG_STREAM_FPS=2
```

### 5.2 Persons and telemetry

```bash
curl -k "https://BOARD_IP/api/v1/persons"
curl -k "https://BOARD_IP/api/v1/telemetry"
```

Example persons response:

```json
{
  "persons": 1,
  "timestamp": "2026-07-29T15:30:20+04:00"
}
```

The telemetry response includes the required fields:

```json
{
  "timestamp": "2026-07-29T11:30:20Z",
  "temperature_c": 53.8,
  "memory_available_mb": 312.4,
  "cpu_percent": 27.1
}
```

It also includes dashboard identity, uptime, guard mode, and camera readiness.

### 5.3 Command API and reboot service

The API validates `cmd` against an allowlist and writes a request to the
restricted command queue. Input is never passed to a shell. The separate
root-owned `smart-guard-command.service` performs privileged operations with
only `CAP_SYS_BOOT`.

Configuration:

```ini
COMMAND_REQUEST_PATH=/var/lib/smart-guard/command-request
COMMAND_POLL_INTERVAL_MS=250
COMMAND_REBOOT_ENABLED=1
COMMAND_REBOOT_DELAY_SEC=2
```

Test reboot last because it disconnects the board:

```bash
curl -k \
  -H "Content-Type: application/json" \
  -d '{"cmd":"reboot"}' \
  "https://BOARD_IP/api/v1/command"
```

The C API returns `202 Accepted` before the delayed reboot:

```json
{
  "status": "accepted",
  "cmd": "reboot"
}
```

To disable remote reboot without removing the endpoint:

```ini
COMMAND_REBOOT_ENABLED=0
```

### 5.4 Swagger UI

Swagger UI and its JavaScript/CSS assets are installed locally on the board.
No internet connection or FastAPI process is required.

Open:

```text
https://BOARD_IP/docs/
```

The OpenAPI source is available at:

```text
https://BOARD_IP/openapi.yaml
```

To test an endpoint:

1. Open the endpoint row.
2. Select **Try it out**.
3. Enter its query parameter or JSON body.
4. Select **Execute**.
5. Record the request URL, response code, and response body.

For `/api/v1/stream`, set `frames=1` or `frames=3`; otherwise the request is a
continuous stream and Swagger remains in the loading state. Run
`POST /api/v1/command` last.

The full Section 2 experiment procedure is in
[`docs/SECTION2_TESTS.md`](docs/SECTION2_TESTS.md).

## 6. Section 3 - Vision, email, and MQTT

### 6.1 Camera and OpenCV

Default USB-camera configuration:

```ini
CAMERA_SOURCE=v4l2
CAMERA_DEVICE=auto
CAMERA_BACKEND=auto
CAMERA_WIDTH=640
CAMERA_HEIGHT=480
CAMERA_INPUT_FPS=10

VISION_OUTPUT_FPS=2
VISION_JPEG_QUALITY=82
VISION_DETECTION_WIDTH=320
VISION_DETECTOR=face
FACE_CASCADE_PATH=auto
VISION_PYTHON=/usr/bin/python3
```

Supported detector modes are:

| Value | Result |
|---|---|
| `face` | OpenCV Haar face detection |
| `person_hog` | OpenCV HOG person detection |
| `none` | Stream and overlay only, with detection disabled |

`VISION_DETECTOR=none` is intended for the Section 2 stream-only temperature
experiment. It keeps camera capture, JPEG encoding, overlay, and MJPEG active
without running a detector.

The vision process atomically publishes:

```text
/run/smart-guard/latest.jpg
/run/smart-guard/vision-state.json
```

For ONVIF/RTSP input, configure:

```ini
CAMERA_SOURCE=onvif
ONVIF_HOST=192.168.1.50
ONVIF_PORT=80
ONVIF_USERNAME=camera_user
ONVIF_PASSWORD_FILE=/etc/smart-guard/onvif-password
ONVIF_PROFILE_TOKEN=
```

### 6.2 Email

Detection emails can contain the person count, timestamp, CPU temperature, and
latest annotated image. Email attempts are limited to one per 30 seconds by
default.

```ini
EMAIL_ENABLED=1
EMAIL_TO=receiver@example.com
SMTP_HOST=smtp.gmail.com
SMTP_PORT=587
SMTP_SECURITY=starttls
SMTP_USERNAME=sender@gmail.com
SMTP_FROM=sender@gmail.com
SMTP_PASSWORD_FILE=/etc/smart-guard/smtp-password
EMAIL_DEBOUNCE_SEC=30
```

For Gmail, use an App Password rather than the normal account password.

### 6.3 MQTT

The Mosquitto broker runs on the Ubuntu PC and the C MQTT client runs on the
Orange Pi.

On the PC:

```bash
cd ~/smart-guard
sudo ./pc/setup_mosquitto_pc.sh \
  --student-id 402XXXXXX \
  --board-ip 192.168.1.123
```

Then configure the board:

```ini
MQTT_ENABLED=1
MQTT_BROKER_HOST=192.168.1.10
MQTT_BROKER_PORT=1883
MQTT_USERNAME=smartguard
MQTT_PASSWORD_FILE=/etc/smart-guard/mqtt-password
MQTT_KEEPALIVE_SEC=15
MQTT_PERSONS_INTERVAL_SEC=1
MQTT_TELEMETRY_INTERVAL_SEC=5
```

The QoS 1 topics are:

```text
home/<student_id>/persons
home/<student_id>/telemetry
home/<student_id>/status
alarm/<student_id>/home
```

The retained status topic has a Last Will and Testament for unexpected board
disconnects.

Monitor all project messages on the PC:

```bash
./pc/monitor_mqtt.sh 402XXXXXX
```

## 7. Section 4 - Final features

### 7.1 Guard mode

The dashboard button and `/api/v1/guard-mode` API persist the state in:

```text
/var/lib/smart-guard/guard-mode
```

When guard mode is armed, a detection sends an immediate email with the image
and publishes the emergency MQTT alarm.

### 7.2 SQLite circular history

The watchdog records detection events in:

```text
/var/lib/smart-guard/history.db
```

Only the newest `HISTORY_CAPACITY` rows are retained, while lifetime detection
and person totals remain available.

### 7.3 Software watchdog

If no new frame arrives for 30 seconds, the watchdog records the incident,
optionally emails a camera-tampering warning, and restarts the vision service.

```ini
WATCHDOG_FRAME_TIMEOUT_SEC=30
WATCHDOG_EMAIL_ENABLED=1
WATCHDOG_RESTART_ENABLED=1
WATCHDOG_RESTART_SERVICE=smart-guard-vision.service
```

### 7.4 Adaptive thermal management

The C thermal manager uses hysteresis:

- At or above 70 °C, it reduces resolution, input FPS, output FPS, and
  detection width.
- At or below 60 °C, it restores normal settings.
- It can send an email whenever the thermal mode changes.

```ini
THERMAL_MANAGER_ENABLED=1
THERMAL_HIGH_TEMP_C=70
THERMAL_RECOVERY_TEMP_C=60
THERMAL_POLL_INTERVAL_SEC=2
```

## 8. Synchronize the PC project with the Orange Pi

The trailing slash after the local project directory is important:

```bash
SG_IP=192.168.1.123

rsync -avh --progress --delete \
  --exclude='.git/' \
  --exclude='build/' \
  --exclude='config/config.env' \
  ~/smart-guard/ \
  "orangepi@${SG_IP}:/home/orangepi/smart-guard/"
```

`--delete` makes the board source tree match the PC source tree. It only affects
the exact remote directory shown above. Remove `--delete` if unrelated files
were intentionally placed inside that directory.

The exclusion preserves the board's source-side `config/config.env`. The
installed configuration under `/etc/smart-guard/config.env` is also preserved
by the installer.

Connect:

```bash
ssh "orangepi@${SG_IP}"
cd ~/smart-guard
```

## 9. Install or update the board

For the first installation:

```bash
cp config/config.example.env config/config.env
nano config/config.env
sudo ./scripts/install.sh --no-start
```

Store enabled secrets:

```bash
sudo ./scripts/set_secret.sh smtp
sudo ./scripts/set_secret.sh mqtt
```

Start all seven services:

```bash
sudo systemctl start \
  smart-guard-command.service \
  smart-guard-camera.service \
  smart-guard-vision.service \
  smart-guard-watchdog.service \
  smart-guard-notifier.service \
  smart-guard-thermal-manager.service \
  smart-guard-web.service
```

For a normal direct installation:

```bash
sudo ./scripts/install.sh
```

For later code-only updates:

```bash
sudo ./scripts/install.sh --skip-deps
```

When `config/config.env` is absent during an update, the installer preserves
and reuses `/etc/smart-guard/config.env`.

After an API update, verify:

```bash
systemctl --no-pager --full status \
  smart-guard-command.service \
  smart-guard-web.service

sudo journalctl -b \
  -u smart-guard-command.service \
  -u smart-guard-web.service \
  --no-pager
```

Then open:

```text
https://BOARD_IP/
https://BOARD_IP/docs/
```

## 10. Validation

Before synchronizing, run on the Ubuntu PC:

```bash
make clean
make check-offline
```

On the running board:

```bash
./tests/test_api.sh --host 127.0.0.1
```

From the Ubuntu PC:

```bash
./tests/test_api.sh --host 192.168.1.123
```

The normal API test does not reboot the board. To deliberately test the reboot
endpoint, run it last:

```bash
./tests/test_api.sh \
  --host 192.168.1.123 \
  --reboot-test
```

Display all service states:

```bash
systemctl --no-pager --full status \
  smart-guard-command.service \
  smart-guard-camera.service \
  smart-guard-vision.service \
  smart-guard-watchdog.service \
  smart-guard-notifier.service \
  smart-guard-thermal-manager.service \
  smart-guard-web.service
```

Follow all project logs:

```bash
sudo journalctl -f \
  -u smart-guard-command.service \
  -u smart-guard-camera.service \
  -u smart-guard-vision.service \
  -u smart-guard-watchdog.service \
  -u smart-guard-notifier.service \
  -u smart-guard-thermal-manager.service \
  -u smart-guard-web.service
```

The `.gitignore` excludes local build output, `config/config.env`, certificates,
private keys, logs, and generated test binaries. Never commit password files or
private keys.
