# Section 2 - REST API and Required Experiments

This guide follows the required experiment numbering in the final project
specification. Replace the example IP address with the real Orange Pi address.

## 1. Preparation

On the Ubuntu PC:

```bash
sudo apt-get update
sudo apt-get install -y curl jq

SG_IP=192.168.1.123
BASE="https://${SG_IP}"
```

Verify the board:

```bash
curl -k "${BASE}/api/v1/telemetry"
```

Open Swagger UI:

```text
https://192.168.1.123/docs/
```

Accept the browser's self-signed certificate warning.

## 2. Test every endpoint in Swagger UI

For each endpoint, expand its row, select **Try it out**, enter the described
input, and select **Execute**. Capture the request URL, response status, and
response body in the report.

### 2.1 `GET /api/v1/stream`

Set `frames` to `1` and execute. The expected result is:

```text
HTTP 200
Content-Type: multipart/x-mixed-replace; boundary=frame
```

The finite `frames` parameter is provided for Swagger and automated tests.
Without it, the endpoint is a continuous MJPEG stream. Open the following URL
in a separate browser tab to show the live video:

```text
https://192.168.1.123/api/v1/stream
```

### 2.2 `GET /api/v1/persons`

Execute once with no person in view and once with a person in front of the
camera. The response contains a count and live timestamp:

```json
{
  "persons": 1,
  "timestamp": "2026-07-29T15:30:20+04:00"
}
```

### 2.3 `GET /api/v1/telemetry`

Execute twice with a short delay. Confirm that `timestamp`, `temperature_c`,
`memory_available_mb`, and `cpu_percent` are present. These values are read by
C code directly from `/sys/class/thermal`, `/proc/meminfo`, and `/proc/stat`.

### 2.4 `GET /api/v1/history`

Enter `limit=5`. Create several detections before the test so the SQLite result
is not empty. Confirm that the response includes lifetime totals and at most
five newest events.

### 2.5 `GET/POST /api/v1/guard-mode`

Execute `GET` to record the current state. Execute `POST` with:

```json
{
  "enabled": true
}
```

Run `GET` again to prove the value was persisted. Repeat with `false` when the
test is complete.

### 2.6 `POST /api/v1/command`

Run this test last. Use:

```json
{
  "cmd": "reboot"
}
```

The expected response is `202 Accepted`. The board reboots after the configured
delay, Swagger disconnects, and all enabled services start automatically after
boot.

## 3. Experiment 2-1 - CPU temperature in three modes

Record one sample every 30 seconds for five minutes, from elapsed time 0 through
300 seconds. Keep room temperature, fan state, camera resolution, and starting
temperature as similar as possible between runs.

Back up the installed configuration on the board:

```bash
sudo cp /etc/smart-guard/config.env \
  /etc/smart-guard/config.env.section2-backup
```

### Mode A - Idle

On the board, stop processing services but leave the C web/API server running:

```bash
sudo systemctl stop \
  smart-guard-watchdog.service \
  smart-guard-notifier.service \
  smart-guard-thermal-manager.service \
  smart-guard-vision.service \
  smart-guard-camera.service
```

On the PC:

```bash
printf 'elapsed_s,temperature_c\n' > temp_idle.csv

for i in $(seq 0 10); do
  curl -sk "${BASE}/api/v1/telemetry" |
    jq -r --argjson t "$((i * 30))" \
      '[$t, .temperature_c] | @csv' >> temp_idle.csv

  (( i < 10 )) && sleep 30
done
```

### Mode B - Stream only

On the board, set the no-detection mode:

```bash
sudo sed -i \
  's/^VISION_DETECTOR=.*/VISION_DETECTOR=none/' \
  /etc/smart-guard/config.env

sudo systemctl start smart-guard-camera.service
sudo systemctl restart smart-guard-vision.service
```

On the PC, keep a continuous stream open for slightly more than five minutes:

```bash
timeout 305s curl -k -sS -N \
  "${BASE}/api/v1/stream" \
  -o /dev/null
```

At the same time, use a second terminal:

```bash
printf 'elapsed_s,temperature_c\n' > temp_stream.csv

for i in $(seq 0 10); do
  curl -sk "${BASE}/api/v1/telemetry" |
    jq -r --argjson t "$((i * 30))" \
      '[$t, .temperature_c] | @csv' >> temp_stream.csv

  (( i < 10 )) && sleep 30
done
```

### Mode C - Stream and detection

On the board:

```bash
sudo sed -i \
  's/^VISION_DETECTOR=.*/VISION_DETECTOR=face/' \
  /etc/smart-guard/config.env

sudo systemctl restart smart-guard-vision.service
```

Keep the continuous stream open again and record the same samples in:

```text
temp_detection.csv
```

The report must contain:

- One temperature-versus-time graph with three curves.
- Minimum, maximum, and average temperature for each mode.
- A final screenshot with detection active.
- A short explanation of the extra load caused by image processing.

Allow the board to cool between runs. Do not claim a fair comparison if the
three tests began at very different temperatures.

## 4. Experiment 2-2 - C web-server memory over five minutes

The relevant program memory is the resident memory of `smart-guard-web`, not
only system-wide available memory.

On the PC:

```bash
timeout 305s curl -k -sS -N \
  "${BASE}/api/v1/stream" \
  -o /dev/null
```

At the same time on the board:

```bash
printf 'elapsed_s,pid,rss_kb,vmsize_kb\n' > web_memory.csv

for i in $(seq 0 60); do
  PID="$(systemctl show -p MainPID --value smart-guard-web.service)"
  RSS="$(awk '/^VmRSS:/ {print $2}' "/proc/${PID}/status")"
  VSZ="$(awk '/^VmSize:/ {print $2}' "/proc/${PID}/status")"

  printf '%d,%s,%s,%s\n' \
    "$((i * 5))" "${PID}" "${RSS}" "${VSZ}" >> web_memory.csv

  (( i < 60 )) && sleep 5
done
```

Record restart information before and after:

```bash
systemctl show smart-guard-web.service -p MainPID -p NRestarts
```

A small increase followed by a stable plateau is not evidence of a leak. A
continuous upward trend that does not stabilize is suspicious. The report
should state whether a leak was observed during this five-minute interval,
rather than making an absolute lifetime claim.

## 5. Experiment 2-3 - Fifty concurrent telemetry workers

Measure the baseline on the PC:

```bash
for i in $(seq 1 50); do
  curl -k -sS -o /dev/null \
    -w '%{http_code},%{time_total}\n' \
    "${BASE}/api/v1/telemetry"
done > baseline.csv
```

In terminal 1, record board telemetry during the load:

```bash
printf 'elapsed_s,temperature_c,cpu_percent,memory_available_mb\n' \
  > load_telemetry.csv

for i in $(seq 0 35); do
  curl -sk "${BASE}/api/v1/telemetry" |
    jq -r --argjson t "${i}" \
      '[$t, .temperature_c, .cpu_percent, .memory_available_mb] | @csv' \
      >> load_telemetry.csv

  (( i < 35 )) && sleep 1
done
```

At the same time in terminal 2, run 50 concurrent workers for 30 seconds:

```bash
rm -f load-worker-*.csv

for worker in $(seq 1 50); do
  (
    end=$((SECONDS + 30))
    while (( SECONDS < end )); do
      curl -k -sS -o /dev/null \
        -w '%{http_code},%{time_total}\n' \
        "${BASE}/api/v1/telemetry"
    done
  ) > "load-worker-${worker}.csv" &
done

wait
cat load-worker-*.csv > load_results.csv
```

Summarize either response file:

```bash
awk -F, '
{
  count++
  sum += $2
  if ($2 > maximum) maximum = $2
  if ($1 != 200) errors++
}
END {
  printf "requests=%d\naverage_s=%.6f\nmaximum_s=%.6f\nerrors=%d\n",
         count, sum/count, maximum, errors
}' load_results.csv
```

Calculate:

```text
latency increase (%) =
  (load average - baseline average) / baseline average * 100
```

Report the real average and maximum latency, non-200 responses, maximum
temperature and CPU load, and minimum available memory.

## 6. Experiment 2-4 - Network loss during streaming

Before disconnecting the network, record:

```bash
systemctl show smart-guard-web.service \
  -p MainPID \
  -p NRestarts \
  -p ActiveState
```

Then:

1. Open the live MJPEG stream.
2. Physically disconnect Ethernet, or disable the Wi-Fi access point.
3. Confirm that the remote stream stops.
4. Wait exactly two minutes.
5. Restore the network.
6. Reload the page without manually restarting a Smart Guard service.

After reconnection:

```bash
ping -c 3 "${SG_IP}"
curl -k -i "${BASE}/api/v1/telemetry"
```

On the board:

```bash
systemctl show smart-guard-web.service \
  -p MainPID \
  -p NRestarts \
  -p ActiveState

sudo journalctl -b \
  -u smart-guard-web.service \
  -u smart-guard-vision.service \
  --since "10 minutes ago" \
  --no-pager
```

Local camera processing should continue during network loss. The old TCP
stream ends, and a new browser connection recovers the stream after the network
returns. No manual service restart should be required.

## 7. Restore normal configuration

On the board:

```bash
sudo mv \
  /etc/smart-guard/config.env.section2-backup \
  /etc/smart-guard/config.env

sudo systemctl restart \
  smart-guard-camera.service \
  smart-guard-vision.service \
  smart-guard-watchdog.service \
  smart-guard-notifier.service \
  smart-guard-thermal-manager.service \
  smart-guard-web.service
```

Verify:

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
