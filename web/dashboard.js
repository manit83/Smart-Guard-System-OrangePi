"use strict";

const elements = {
  studentName: document.querySelector("#student-name"),
  studentId: document.querySelector("#student-id"),
  temperature: document.querySelector("#temperature"),
  memory: document.querySelector("#memory"),
  cpu: document.querySelector("#cpu"),
  uptime: document.querySelector("#uptime"),
  personCount: document.querySelector("#person-count"),
  visionState: document.querySelector("#vision-state"),
  systemTime: document.querySelector("#system-time"),
  systemDate: document.querySelector("#system-date"),
  lastUpdate: document.querySelector("#last-update"),
  streamStatus: document.querySelector("#stream-status"),
  streamStatusText: document.querySelector("#stream-status-text"),
  cameraFrame: document.querySelector("#camera-frame"),
  waitingState: document.querySelector("#waiting-state"),
  guardState: document.querySelector("#guard-state"),
  guardDescription: document.querySelector("#guard-description"),
  guardToggle: document.querySelector("#guard-toggle"),
  guardMessage: document.querySelector("#guard-message"),
  historyTotal: document.querySelector("#history-total"),
  historyList: document.querySelector("#history-list")
};

let refreshTimer = null;
let intervalMilliseconds = 2000;
let frameWasAvailable = false;
let frameLoading = false;
let streamActive = false;
let guardEnabled = false;
let guardRequestRunning = false;

function numberOrDash(value, fractionDigits = 1) {
  return Number.isFinite(value) ? value.toFixed(fractionDigits) : "--";
}

function formatUptime(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) {
    return "---";
  }

  const totalMinutes = Math.floor(seconds / 60);
  const days = Math.floor(totalMinutes / 1440);
  const hours = Math.floor((totalMinutes % 1440) / 60);
  const minutes = totalMinutes % 60;

  if (days > 0) {
    return `${days}d ${hours}h`;
  }
  return `${hours}h ${minutes}m`;
}

function updateClock(timestamp) {
  const date = new Date(timestamp);

  if (Number.isNaN(date.getTime())) {
    return;
  }

  elements.systemTime.textContent = new Intl.DateTimeFormat("en-GB", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false
  }).format(date);

  elements.systemDate.textContent = new Intl.DateTimeFormat("en-GB", {
    year: "numeric",
    month: "2-digit",
    day: "2-digit"
  }).format(date);

  elements.lastUpdate.textContent = elements.systemTime.textContent;
}

function showWaitingState(message, isError = false) {
  frameWasAvailable = false;
  elements.cameraFrame.hidden = true;
  elements.waitingState.hidden = false;
  elements.streamStatus.classList.toggle("error", isError);
  elements.streamStatus.classList.remove("online");
  elements.streamStatusText.textContent = message;
}

function showLiveState() {
  elements.waitingState.hidden = true;
  elements.cameraFrame.hidden = false;
  elements.streamStatus.classList.remove("error");
  elements.streamStatus.classList.add("online");
  elements.streamStatusText.textContent = "Live feed";
}

function updateGuardDisplay(enabled) {
  guardEnabled = Boolean(enabled);
  elements.guardState.textContent = guardEnabled ? "ENABLED" : "DISABLED";
  elements.guardState.classList.toggle("armed", guardEnabled);
  elements.guardToggle.textContent =
    guardEnabled ? "Disable Guard Mode" : "Enable Guard Mode";
  elements.guardToggle.setAttribute("aria-pressed", String(guardEnabled));
  elements.guardDescription.textContent = guardEnabled
    ? "Person detection sends an email with a photo and an emergency MQTT alarm."
    : "Person email and emergency MQTT alarms are disabled.";
}

async function setGuardMode() {
  if (guardRequestRunning) {
    return;
  }

  guardRequestRunning = true;
  elements.guardToggle.disabled = true;
  elements.guardMessage.textContent = "Updating...";

  try {
    const response = await fetch("/api/v1/guard-mode", {
      method: "POST",
      cache: "no-store",
      headers: {
        Accept: "application/json",
        "Content-Type": "application/json"
      },
      body: JSON.stringify({ enabled: !guardEnabled })
    });
    const result = await response.json();

    if (!response.ok) {
      throw new Error(result.error || `HTTP ${response.status}`);
    }
    updateGuardDisplay(result.enabled);
    elements.guardMessage.textContent =
      result.enabled ? "Guard mode enabled." : "Guard mode disabled.";
  } catch (error) {
    elements.guardMessage.textContent = `Update failed: ${error.message}`;
  } finally {
    guardRequestRunning = false;
    elements.guardToggle.disabled = false;
  }
}

function showHistory(history) {
  elements.historyTotal.textContent =
    Number.isInteger(history.total_detection_events)
      ? String(history.total_detection_events)
      : "0";
  elements.historyList.replaceChildren();

  if (!Array.isArray(history.events) || history.events.length === 0) {
    const item = document.createElement("li");
    item.textContent = "No detections have been recorded.";
    elements.historyList.append(item);
    return;
  }

  for (const event of history.events) {
    const item = document.createElement("li");
    const timestamp = document.createElement("time");
    const count = document.createElement("strong");

    timestamp.textContent = event.timestamp || "Unknown time";
    timestamp.dateTime = event.timestamp || "";
    count.textContent = `${event.persons || 0} people`;
    item.append(timestamp, count);
    elements.historyList.append(item);
  }
}

async function loadHistory() {
  try {
    const response = await fetch("/api/v1/history?limit=5", {
      cache: "no-store",
      headers: { Accept: "application/json" }
    });

    if (response.ok) {
      showHistory(await response.json());
    }
  } catch (error) {
    // The dashboard remains usable while the database service starts.
  }
}

function refreshFrame() {
  if (!frameWasAvailable || frameLoading || streamActive) {
    return;
  }

  frameLoading = true;
  elements.cameraFrame.onload = () => {
    frameLoading = false;
    streamActive = true;
    showLiveState();
  };
  elements.cameraFrame.onerror = () => {
    frameLoading = false;
    streamActive = false;
    showWaitingState("Frame is not available", true);
  };
  elements.cameraFrame.src = `/api/v1/stream?t=${Date.now()}`;
}

function scheduleRefresh() {
  window.clearTimeout(refreshTimer);
  refreshTimer = window.setTimeout(loadStatus, intervalMilliseconds);
}

function updateDashboard(status, persons) {
  elements.studentName.textContent = status.student_name || "Student";
  elements.studentId.textContent = status.student_id || "---";
  document.title =
    `${status.student_name || "Student"} - ${status.student_id || "---"} | Smart Guard`;

  elements.temperature.textContent = numberOrDash(status.temperature_c);
  elements.memory.textContent = numberOrDash(status.memory_available_mb);
  elements.cpu.textContent = numberOrDash(status.cpu_percent);
  elements.uptime.textContent = formatUptime(status.uptime_seconds);
  elements.personCount.textContent =
    Number.isInteger(persons.persons) ? String(persons.persons) : "0";
  updateGuardDisplay(status.guard_enabled);

  elements.visionState.textContent = status.vision_online
    ? "OpenCV face detection is active."
    : "The face detection service is not available.";

  updateClock(status.timestamp);

  const nextInterval = Number(status.telemetry_interval_sec) * 1000;
  if (Number.isFinite(nextInterval) && nextInterval >= 1000) {
    intervalMilliseconds = nextInterval;
  }

  frameWasAvailable = Boolean(status.frame_available);
  if (frameWasAvailable) {
    refreshFrame();
  } else {
    streamActive = false;
    elements.cameraFrame.removeAttribute("src");
    showWaitingState("Waiting for a frame");
  }
}

async function loadStatus() {
  try {
    const [telemetryResponse, personsResponse] = await Promise.all([
      fetch("/api/v1/telemetry", {
        cache: "no-store",
        headers: { Accept: "application/json" }
      }),
      fetch("/api/v1/persons", {
        cache: "no-store",
        headers: { Accept: "application/json" }
      })
    ]);

    if (!telemetryResponse.ok) {
      throw new Error(`Telemetry HTTP ${telemetryResponse.status}`);
    }

    const telemetry = await telemetryResponse.json();
    const persons = personsResponse.ok
      ? await personsResponse.json()
      : { persons: 0 };
    updateDashboard(telemetry, persons);
  } catch (error) {
    elements.lastUpdate.textContent = "Disconnected";
    elements.streamStatus.classList.add("error");
    elements.streamStatus.classList.remove("online");
    elements.streamStatusText.textContent = "Connection error";
  } finally {
    scheduleRefresh();
  }
}

loadStatus();
loadHistory();
window.setInterval(loadHistory, 5000);
elements.guardToggle.addEventListener("click", setGuardMode);
