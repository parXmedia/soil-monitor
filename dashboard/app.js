const DEFAULT_CONFIG = Object.freeze({
  apiBaseUrl: "",
  currentPath: "/v1/current",
  historyPath: "/v1/history",
  pollIntervalMs: 15000,
  historyRefreshIntervalMs: 300000,
  requestTimeoutMs: 8000,
  staleAfterMs: 360000,
  thresholds: Object.freeze({ dryBelow: 25, healthyBelow: 80 })
});

const MIN_POLL_INTERVAL_MS = 5000;
const MAX_RETRY_INTERVAL_MS = 5 * 60 * 1000;
const CACHE_KEY = "soil-monitor-cache-v1";
const ACCESS_KEY = "soil-monitor-read-token-v1";

class AuthorizationError extends Error {}

export function clamp(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}

function finiteNumber(value, fallback = null) {
  if (value === null || value === undefined || value === "") return fallback;
  const number = typeof value === "number" ? value : Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function nullablePercent(value) {
  const number = finiteNumber(value);
  return number === null ? null : clamp(number, 0, 100);
}

function parseTimestamp(value) {
  const milliseconds = typeof value === "number" ? value : Date.parse(value);
  return Number.isFinite(milliseconds) ? milliseconds : null;
}

export function normalizeReading(payload) {
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    throw new TypeError("Reading must be a JSON object");
  }

  const moisture = nullablePercent(payload.moisture);
  const timestamp = parseTimestamp(payload.timestamp);
  if (moisture === null || timestamp === null) {
    throw new TypeError("Reading requires valid moisture and timestamp fields");
  }

  const rssi = finiteNumber(payload.rssi);
  const explicitSignal = nullablePercent(payload.signal);

  return Object.freeze({
    deviceId: String(payload.deviceId || "garden-sensor").slice(0, 64),
    timestamp,
    receivedAt: parseTimestamp(payload.receivedAt),
    moisture,
    rawAdc: finiteNumber(payload.rawAdc),
    millivolts: finiteNumber(payload.millivolts),
    rssi,
    signal: explicitSignal ?? signalFromRssi(rssi),
    batteryVoltage: finiteNumber(payload.batteryVoltage),
    batteryPercent: nullablePercent(payload.batteryPercent),
    sequence: finiteNumber(payload.sequence)
  });
}

// States the span the chart actually covers. The API returns time-bucketed
// aggregates, so "7d" can legitimately be backed by hourly averages, and a
// young deployment simply has less history than the button implies. Saying so
// is better than drawing a partial line that looks like the full window.
export function describeCoverage(payload, requestedHours) {
  const readings = Array.isArray(payload) ? payload : payload?.readings;
  if (!Array.isArray(readings) || readings.length === 0) return "";

  const from = Date.parse(payload?.coveredFrom ?? readings[0]?.timestamp);
  const to = Date.parse(payload?.coveredTo ?? readings[readings.length - 1]?.timestamp);
  if (!Number.isFinite(from) || !Number.isFinite(to)) return "";

  const spanHours = (to - from) / 3_600_000;
  const bucketSeconds = Number(payload?.bucketSeconds);
  const parts = [`${readings.length} points`];

  if (Number.isFinite(bucketSeconds) && bucketSeconds >= 60) {
    const minutes = Math.round(bucketSeconds / 60);
    parts.push(minutes >= 60 ? "hourly averages" : `${minutes}-minute averages`);
  }

  // Only flag a shortfall worth noticing, not ordinary bucket-edge rounding.
  if (spanHours < requestedHours * 0.9) {
    const covered = spanHours < 1
      ? `${Math.max(1, Math.round(spanHours * 60))} minutes`
      : `${spanHours.toFixed(1)} hours`;
    parts.push(`covering the last ${covered} — no older data recorded yet`);
  }

  return parts.join(" · ");
}

export function normalizeHistory(payload) {
  const source = Array.isArray(payload) ? payload : payload?.readings;
  if (!Array.isArray(source)) throw new TypeError("History response requires a readings array");

  return source
    .map((entry) => {
      try { return normalizeReading(entry); } catch { return null; }
    })
    .filter(Boolean)
    .sort((a, b) => a.timestamp - b.timestamp);
}

export function signalFromRssi(rssi) {
  if (!Number.isFinite(rssi)) return null;
  if (rssi >= -50) return 100;
  if (rssi <= -100) return 0;
  return Math.round((rssi + 100) * 2);
}

export function classifyMoisture(moisture, thresholds = DEFAULT_CONFIG.thresholds) {
  if (!Number.isFinite(moisture)) return "unknown";
  if (moisture < thresholds.dryBelow) return "dry";
  if (moisture < thresholds.healthyBelow) return "healthy";
  return "wet";
}

export function computeStats(readings) {
  if (!readings.length) return null;
  const values = readings.map((reading) => reading.moisture);
  const average = values.reduce((total, value) => total + value, 0) / values.length;
  const minimum = Math.min(...values);
  const maximum = Math.max(...values);
  const comparison = values[Math.max(0, values.length - Math.min(7, values.length))];
  const delta = values.at(-1) - comparison;
  const trend = delta > 1.5 ? "Rising" : delta < -1.5 ? "Falling" : "Stable";
  return { average, minimum, maximum, delta, trend };
}

export function readingFreshness(timestamp, staleAfterMs, now = Date.now()) {
  if (!Number.isFinite(timestamp)) return { fresh: false, clockError: false, ageMs: Infinity };
  const rawAge = now - timestamp;
  const clockError = rawAge < -120000;
  return {
    fresh: !clockError && rawAge <= staleAfterMs,
    clockError,
    ageMs: Math.max(0, rawAge)
  };
}

export function validateConfig(candidate, locationLike = globalThis.location) {
  const config = {
    ...DEFAULT_CONFIG,
    ...(candidate || {}),
    thresholds: { ...DEFAULT_CONFIG.thresholds, ...(candidate?.thresholds || {}) }
  };

  if (!config.apiBaseUrl || typeof config.apiBaseUrl !== "string") {
    throw new Error("Set apiBaseUrl in config.js before publishing the dashboard.");
  }

  let url;
  try { url = new URL(config.apiBaseUrl); } catch {
    throw new Error("apiBaseUrl in config.js is not a valid URL.");
  }

  const localHosts = new Set(["localhost", "127.0.0.1", "::1"]);
  const pageIsLocal = locationLike && localHosts.has(locationLike.hostname);
  if (url.protocol !== "https:" && !(pageIsLocal && url.protocol === "http:")) {
    throw new Error("apiBaseUrl must use HTTPS (HTTP is allowed only on localhost). ");
  }

  config.apiBaseUrl = url.href.replace(/\/$/, "");
  config.pollIntervalMs = Math.max(MIN_POLL_INTERVAL_MS, finiteNumber(config.pollIntervalMs, DEFAULT_CONFIG.pollIntervalMs));
  config.historyRefreshIntervalMs = Math.max(config.pollIntervalMs, finiteNumber(config.historyRefreshIntervalMs, DEFAULT_CONFIG.historyRefreshIntervalMs));
  config.requestTimeoutMs = clamp(finiteNumber(config.requestTimeoutMs, DEFAULT_CONFIG.requestTimeoutMs), 1000, 30000);
  config.staleAfterMs = Math.max(config.pollIntervalMs * 2, finiteNumber(config.staleAfterMs, DEFAULT_CONFIG.staleAfterMs));
  config.thresholds.dryBelow = clamp(finiteNumber(config.thresholds.dryBelow, 25), 0, 99);
  config.thresholds.healthyBelow = clamp(finiteNumber(config.thresholds.healthyBelow, 80), config.thresholds.dryBelow + 1, 100);
  config.thresholds = Object.freeze(config.thresholds);
  return Object.freeze(config);
}

export function buildApiUrl(baseUrl, path, search = {}) {
  const url = new URL(path.replace(/^\/+/, ""), `${baseUrl}/`);
  for (const [key, value] of Object.entries(search)) url.searchParams.set(key, String(value));
  return url.href;
}

class SoilDashboard {
  constructor(config) {
    this.config = config;
    this.current = null;
    this.history = [];
    this.historyCoverage = "";
    this.historyHours = 24;
    this.failureCount = 0;
    this.lastHistoryFetchedAt = 0;
    this.pollTimer = null;
    this.pollInProgress = false;
    this.resizeFrame = null;
    this.readToken = "";
    this.elements = this.collectElements();
  }

  collectElements() {
    const ids = [
      "connection-pill", "connection-label", "forget-key-button", "access-gate", "access-key",
      "unlock-button", "access-error", "notice", "notice-title", "notice-message",
      "retry-button", "moisture-value", "moisture-number", "condition-icon", "condition-label", "moisture-fill",
      "insight-title", "insight-copy", "last-reading", "signal-bars", "signal-value",
      "rssi-value", "battery-fill", "battery-value", "battery-voltage", "sensor-voltage",
      "raw-value", "sequence-value", "device-value", "history-title", "average-value",
      "minimum-value", "maximum-value", "trend-value", "history-chart", "chart-wrap",
      "chart-empty", "chart-description", "chart-coverage", "refresh-label"
    ];
    return Object.fromEntries(ids.map((id) => [id, document.getElementById(id)]));
  }

  start() {
    this.bindEvents();
    this.elements["refresh-label"].textContent = `Updates every ${Math.round(this.config.pollIntervalMs / 1000)} seconds`;
    try { this.readToken = sessionStorage.getItem(ACCESS_KEY) || ""; } catch { this.readToken = ""; }
    if (!this.readToken) {
      this.showAccessGate();
      return;
    }
    this.hideAccessGate();
    this.restoreCache();
    this.refreshAll();
  }

  bindEvents() {
    this.elements["retry-button"].addEventListener("click", () => this.refreshAll());
    this.elements["unlock-button"].addEventListener("click", () => this.unlock());
    this.elements["access-key"].addEventListener("keydown", (event) => {
      if (event.key === "Enter") this.unlock();
    });
    this.elements["forget-key-button"].addEventListener("click", () => this.forgetKey());
    document.querySelectorAll("[data-hours]").forEach((button) => {
      button.addEventListener("click", () => {
        this.historyHours = Number(button.dataset.hours);
        document.querySelectorAll("[data-hours]").forEach((candidate) => {
          const selected = candidate === button;
          candidate.classList.toggle("selected", selected);
          candidate.setAttribute("aria-pressed", String(selected));
        });
        this.elements["history-title"].textContent = this.historyHours === 168 ? "Last 7 days" : `Last ${this.historyHours} hours`;
        this.fetchHistory().catch(() => {
          this.elements["chart-empty"].textContent = "History is temporarily unavailable. Current readings will continue updating.";
        });
      });
    });

    document.addEventListener("visibilitychange", () => {
      if (!document.hidden) this.refreshAll();
    });
    window.addEventListener("online", () => this.refreshAll());
    window.addEventListener("offline", () => this.showOffline("This device is offline."));
    window.addEventListener("resize", () => {
      cancelAnimationFrame(this.resizeFrame);
      this.resizeFrame = requestAnimationFrame(() => this.drawChart());
    });
  }

  async requestJson(path, search) {
    if (!this.readToken) throw new AuthorizationError("A read-only access key is required.");
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), this.config.requestTimeoutMs);
    try {
      const response = await fetch(buildApiUrl(this.config.apiBaseUrl, path, search), {
        cache: "no-store",
        credentials: "omit",
        headers: {
          Accept: "application/json",
          Authorization: `Bearer ${this.readToken}`
        },
        referrerPolicy: "no-referrer",
        redirect: "error",
        signal: controller.signal
      });
      if (response.status === 401 || response.status === 403) {
        throw new AuthorizationError("The cloud API did not accept this access key.");
      }
      if (!response.ok) throw new Error(`Cloud API returned ${response.status}`);
      const contentType = response.headers.get("content-type") || "";
      if (!contentType.toLowerCase().includes("application/json")) throw new Error("Cloud API did not return JSON");
      return await response.json();
    } finally {
      clearTimeout(timeout);
    }
  }

  async refreshAll() {
    clearTimeout(this.pollTimer);
    if (this.pollInProgress || document.hidden) return;
    this.pollInProgress = true;
    this.setConnection("loading", this.current ? "Refreshing" : "Connecting");

    try {
      await this.fetchCurrent();
      if (!this.history.length || Date.now() - this.lastHistoryFetchedAt >= this.config.historyRefreshIntervalMs) {
        await this.fetchHistory().catch(() => {
          this.elements["chart-empty"].textContent = "History is temporarily unavailable. Current readings will continue updating.";
        });
      }
      this.failureCount = 0;
      this.updateFreshnessNotice();
    } catch (error) {
      if (error instanceof AuthorizationError) {
        this.clearStoredKey();
        this.readToken = "";
        this.showAccessGate(error.message);
        return;
      }
      this.failureCount += 1;
      this.showOffline(error instanceof Error ? error.message : "Could not reach the cloud API.");
    } finally {
      this.pollInProgress = false;
      if (this.readToken) this.scheduleNextPoll();
    }
  }

  scheduleNextPoll() {
    const multiplier = Math.min(2 ** this.failureCount, 16);
    const delay = Math.min(this.config.pollIntervalMs * multiplier, MAX_RETRY_INTERVAL_MS);
    clearTimeout(this.pollTimer);
    this.pollTimer = setTimeout(() => this.refreshAll(), delay);
  }

  async fetchCurrent() {
    const payload = await this.requestJson(this.config.currentPath);
    this.current = normalizeReading(payload);
    this.renderCurrent();
    this.saveCache();
  }

  async fetchHistory() {
    const payload = await this.requestJson(this.config.historyPath, { hours: this.historyHours });
    this.history = normalizeHistory(payload);
    this.historyCoverage = describeCoverage(payload, this.historyHours);
    this.lastHistoryFetchedAt = Date.now();
    this.renderHistory();
    this.saveCache();
  }

  renderCurrent() {
    const reading = this.current;
    if (!reading) return;

    const freshness = readingFreshness(reading.timestamp, this.config.staleAfterMs);
    const condition = classifyMoisture(reading.moisture, this.config.thresholds);
    const roundedMoisture = Math.round(reading.moisture);
    const colors = { dry: "var(--red)", healthy: "var(--green)", wet: "var(--cyan)" };
    const labels = { dry: "Soil is dry", healthy: "Moisture is healthy", wet: "Soil is very wet" };

    this.setConnection(
      freshness.fresh ? "fresh" : "stale",
      freshness.clockError ? "Clock error" : (freshness.fresh ? "Up to date" : "Stale data")
    );
    this.elements["moisture-number"].textContent = String(roundedMoisture);
    this.elements["moisture-value"].style.color = colors[condition];
    this.elements["condition-icon"].style.color = colors[condition];
    this.elements["condition-label"].textContent = labels[condition];
    this.elements["moisture-fill"].style.width = `${reading.moisture}%`;
    this.elements["moisture-fill"].style.background = colors[condition];

    const insight = this.createInsight(condition);
    this.elements["insight-title"].textContent = insight.title;
    this.elements["insight-copy"].textContent = insight.copy;
    const iso = new Date(reading.timestamp).toISOString();
    this.elements["last-reading"].dateTime = iso;
    this.elements["last-reading"].textContent = formatLastUpdate(reading.timestamp);
    this.elements["last-reading"].title = new Date(reading.timestamp).toLocaleString();

    const signal = reading.signal;
    this.elements["signal-value"].textContent = signal === null ? "--%" : `${Math.round(signal)}%`;
    this.elements["rssi-value"].textContent = reading.rssi === null ? "RSSI unavailable" : `${Math.round(reading.rssi)} dBm RSSI`;
    this.elements["signal-bars"].dataset.level = signal === null ? "0" : String(clamp(Math.ceil(signal / 25), 1, 4));

    this.elements["battery-value"].textContent = reading.batteryPercent === null ? "--%" : `${Math.round(reading.batteryPercent)}%`;
    this.elements["battery-fill"].style.width = `${reading.batteryPercent ?? 0}%`;
    this.elements["battery-voltage"].textContent = reading.batteryVoltage === null ? "Not reported" : `${reading.batteryVoltage.toFixed(2)} V battery`;
    this.elements["sensor-voltage"].textContent = reading.millivolts === null ? "-- V" : `${(reading.millivolts / 1000).toFixed(2)} V`;
    this.elements["raw-value"].textContent = reading.rawAdc === null ? "Raw ADC unavailable" : `Raw ADC ${Math.round(reading.rawAdc)}`;
    this.elements["sequence-value"].textContent = reading.sequence === null ? "--" : Math.round(reading.sequence).toLocaleString();
    this.elements["device-value"].textContent = reading.deviceId;
  }

  updateFreshnessNotice() {
    if (!this.current) return;
    const freshness = readingFreshness(this.current.timestamp, this.config.staleAfterMs);
    if (freshness.fresh) {
      this.hideNotice();
      return;
    }
    this.elements["notice"].classList.remove("notice-hidden");
    this.elements["notice-title"].textContent = freshness.clockError
      ? "Gateway clock needs attention"
      : "Sensor update overdue";
    this.elements["notice-message"].textContent = freshness.clockError
      ? "The reported sensor time is ahead of this device. The reading is not marked current."
      : `Last sensor update: ${formatLastUpdate(this.current.timestamp)}.`;
  }

  createInsight(condition) {
    const stats = computeStats(this.history);
    if (condition === "dry") {
      return { title: "Watering recommended", copy: "The calibrated reading is below the dry threshold. Water near the root zone, then confirm that moisture rises on the chart." };
    }
    if (condition === "wet") {
      return { title: "Pause watering", copy: "Moisture is above the healthy range. Allow the soil to drain and check for an upward trend before adding more water." };
    }
    if (stats?.trend === "Falling") {
      return { title: "Healthy, but drying", copy: "Moisture is currently in range and trending downward. Keep watching the history before deciding when to water." };
    }
    return { title: "Moisture is on target", copy: "The latest calibrated reading is in the healthy zone. No watering is indicated from the current data." };
  }

  renderHistory() {
    const stats = computeStats(this.history);
    this.elements["average-value"].textContent = stats ? `${stats.average.toFixed(1)}%` : "--%";
    this.elements["minimum-value"].textContent = stats ? `${stats.minimum.toFixed(1)}%` : "--%";
    this.elements["maximum-value"].textContent = stats ? `${stats.maximum.toFixed(1)}%` : "--%";
    this.elements["trend-value"].textContent = stats?.trend || "--";
    this.elements["chart-wrap"].classList.toggle("has-data", this.history.length > 1);
    this.elements["chart-description"].textContent = stats
      ? `${this.history.length} readings. Average ${stats.average.toFixed(1)} percent, minimum ${stats.minimum.toFixed(1)} percent, maximum ${stats.maximum.toFixed(1)} percent, trend ${stats.trend.toLowerCase()}.`
      : "No historical readings are available.";
    if (this.elements["chart-coverage"]) {
      this.elements["chart-coverage"].textContent = this.historyCoverage || "";
    }
    this.drawChart();
  }

  drawChart() {
    const canvas = this.elements["history-chart"];
    const bounds = canvas.getBoundingClientRect();
    if (bounds.width < 1 || bounds.height < 1) return;
    const ratio = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.round(bounds.width * ratio);
    canvas.height = Math.round(bounds.height * ratio);
    const context = canvas.getContext("2d");
    context.scale(ratio, ratio);
    const width = bounds.width;
    const height = bounds.height;
    const padding = { top: 20, right: 16, bottom: 30, left: 42 };
    const plotWidth = width - padding.left - padding.right;
    const plotHeight = height - padding.top - padding.bottom;

    context.clearRect(0, 0, width, height);
    context.font = "11px system-ui";
    context.lineWidth = 1;
    context.textBaseline = "middle";
    [0, 25, 50, 75, 100].forEach((value) => {
      const y = padding.top + plotHeight - (value / 100) * plotHeight;
      context.strokeStyle = "rgba(141, 198, 162, 0.13)";
      context.beginPath();
      context.moveTo(padding.left, y);
      context.lineTo(width - padding.right, y);
      context.stroke();
      context.fillStyle = "#789180";
      context.fillText(`${value}%`, 6, y);
    });

    if (this.history.length < 2) return;
    const start = this.history[0].timestamp;
    const end = this.history.at(-1).timestamp;
    const span = Math.max(end - start, 1);
    const pointFor = (reading) => ({
      x: padding.left + ((reading.timestamp - start) / span) * plotWidth,
      y: padding.top + plotHeight - (reading.moisture / 100) * plotHeight
    });

    const gradient = context.createLinearGradient(0, padding.top, 0, height - padding.bottom);
    gradient.addColorStop(0, "rgba(85, 229, 141, 0.32)");
    gradient.addColorStop(1, "rgba(85, 229, 141, 0.01)");
    context.beginPath();
    this.history.forEach((reading, index) => {
      const point = pointFor(reading);
      if (index === 0) context.moveTo(point.x, point.y);
      else context.lineTo(point.x, point.y);
    });
    const finalPoint = pointFor(this.history.at(-1));
    const firstPoint = pointFor(this.history[0]);
    context.lineTo(finalPoint.x, height - padding.bottom);
    context.lineTo(firstPoint.x, height - padding.bottom);
    context.closePath();
    context.fillStyle = gradient;
    context.fill();

    context.beginPath();
    this.history.forEach((reading, index) => {
      const point = pointFor(reading);
      if (index === 0) context.moveTo(point.x, point.y);
      else context.lineTo(point.x, point.y);
    });
    context.strokeStyle = "#55e58d";
    context.lineWidth = 2.5;
    context.lineJoin = "round";
    context.lineCap = "round";
    context.stroke();

    context.textBaseline = "alphabetic";
    context.fillStyle = "#789180";
    context.fillText(formatChartTime(start, this.historyHours), padding.left, height - 8);
    const rightLabel = formatChartTime(end, this.historyHours);
    const labelWidth = context.measureText(rightLabel).width;
    context.fillText(rightLabel, width - padding.right - labelWidth, height - 8);
  }

  setConnection(state, label) {
    this.elements["connection-pill"].dataset.state = state;
    this.elements["connection-label"].textContent = label;
  }

  unlock() {
    const token = this.elements["access-key"].value.trim();
    if (!token) {
      this.showAccessGate("Enter the read-only access key supplied with this dashboard.");
      return;
    }
    try {
      sessionStorage.setItem(ACCESS_KEY, token);
    } catch {
      this.showAccessGate("This browser blocked temporary storage. Allow session storage and try again.");
      return;
    }
    this.readToken = token;
    this.elements["access-key"].value = "";
    this.hideAccessGate();
    this.refreshAll();
  }

  showAccessGate(message = "") {
    clearTimeout(this.pollTimer);
    document.getElementById("main").inert = true;
    this.setConnection("locked", "Locked");
    this.elements["access-gate"].hidden = false;
    this.elements["forget-key-button"].hidden = true;
    this.elements["access-error"].hidden = !message;
    this.elements["access-error"].textContent = message;
    this.elements["access-key"].focus();
  }

  hideAccessGate() {
    document.getElementById("main").inert = false;
    this.elements["access-gate"].hidden = true;
    this.elements["forget-key-button"].hidden = false;
    this.elements["access-error"].hidden = true;
    this.elements["access-error"].textContent = "";
  }

  clearStoredKey() {
    try { sessionStorage.removeItem(ACCESS_KEY); } catch { /* Storage is optional. */ }
  }

  forgetKey() {
    this.clearStoredKey();
    try { sessionStorage.removeItem(CACHE_KEY); } catch { /* Storage is optional. */ }
    this.readToken = "";
    window.location.reload();
  }

  showOffline(message) {
    this.setConnection("offline", this.current ? "Cached" : "Offline");
    this.elements["notice"].classList.remove("notice-hidden");
    this.elements["notice-title"].textContent = this.current ? "Cloud connection interrupted" : "Sensor data unavailable";
    this.elements["notice-message"].textContent = this.current ? "Showing the last safely stored reading." : message;
  }

  hideNotice() {
    this.elements["notice"].classList.add("notice-hidden");
  }

  saveCache() {
    try {
      sessionStorage.setItem(CACHE_KEY, JSON.stringify({
        savedAt: Date.now(),
        current: this.current,
        history: this.history.slice(-1000)
      }));
    } catch { /* Storage is optional. */ }
  }

  restoreCache() {
    try {
      const cached = JSON.parse(sessionStorage.getItem(CACHE_KEY));
      if (!cached || Date.now() - cached.savedAt > 7 * 24 * 60 * 60 * 1000) return;
      this.current = cached.current ? normalizeReading(cached.current) : null;
      this.history = cached.history ? normalizeHistory(cached.history) : [];
      this.renderCurrent();
      this.renderHistory();
    } catch { /* Ignore invalid or blocked local storage. */ }
  }
}

function formatAge(timestamp) {
  const seconds = Math.max(0, Math.round((Date.now() - timestamp) / 1000));
  if (seconds < 10) return "Just now";
  if (seconds < 60) return `${seconds}s ago`;
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${minutes}m ago`;
  const hours = Math.round(minutes / 60);
  return hours < 24 ? `${hours}h ago` : `${Math.round(hours / 24)}d ago`;
}

function formatLastUpdate(timestamp) {
  const exact = new Intl.DateTimeFormat(undefined, {
    month: "short",
    day: "numeric",
    hour: "numeric",
    minute: "2-digit",
    second: "2-digit"
  }).format(new Date(timestamp));
  return `${exact} · ${formatAge(timestamp)}`;
}

function formatChartTime(timestamp, hours) {
  return new Intl.DateTimeFormat(undefined, hours >= 168
    ? { month: "short", day: "numeric" }
    : { hour: "numeric", minute: "2-digit" }
  ).format(new Date(timestamp));
}

function showConfigurationError(message) {
  document.getElementById("access-gate").hidden = true;
  const pill = document.getElementById("connection-pill");
  pill.dataset.state = "setup";
  document.getElementById("connection-label").textContent = "Setup needed";
  const notice = document.getElementById("notice");
  notice.classList.remove("notice-hidden");
  document.getElementById("notice-title").textContent = "Cloud API not configured";
  document.getElementById("notice-message").textContent = message;
  document.getElementById("retry-button").hidden = true;
}

if (typeof document !== "undefined") {
  try {
    const config = validateConfig(window.SOIL_MONITOR_CONFIG);
    new SoilDashboard(config).start();
  } catch (error) {
    showConfigurationError(error instanceof Error ? error.message : "Review config.js.");
  }
}
