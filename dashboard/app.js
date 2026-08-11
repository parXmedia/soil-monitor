import {
  DEFAULT_CONFIG,
  buildApiUrl,
  classifyMoisture,
  clamp,
  computeStats,
  describeCoverage,
  normalizeHistory,
  normalizeReading,
  readingFreshness,
  validateConfig
} from "./data.js";
import { createPasskeyAuth } from "./auth.js";

export * from "./data.js";
export * from "./auth.js";

const MAX_RETRY_INTERVAL_MS = 5 * 60 * 1000;
const CACHE_KEY = "soil-monitor-cache-v1";

class AuthorizationError extends Error {}

class SoilDashboard {
  constructor(config, auth = createPasskeyAuth(config)) {
    this.config = config;
    this.auth = auth;
    this.session = null;
    this.cacheRestored = false;
    this.current = null;
    this.history = [];
    this.historyCoverage = "";
    this.historyHours = 24;
    this.failureCount = 0;
    this.lastHistoryFetchedAt = 0;
    this.pollTimer = null;
    this.pollInProgress = false;
    this.resizeFrame = null;
    this.elements = this.collectElements();
  }

  collectElements() {
    const ids = [
      "connection-pill", "connection-label", "manage-passkeys-button", "access-gate",
      "passkey-sign-in-button", "setup-email", "send-link-button", "access-error",
      "account-panel", "account-email", "passkey-list", "account-message", "add-passkey-button",
      "sign-out-button", "notice", "notice-title", "notice-message",
      "retry-button", "moisture-value", "moisture-number", "condition-icon", "condition-label", "moisture-fill",
      "insight-title", "insight-copy", "last-reading", "signal-bars", "signal-value",
      "rssi-value", "battery-fill", "battery-value", "battery-voltage", "sensor-voltage",
      "raw-value", "sequence-value", "device-value", "history-title", "average-value",
      "power-value", "current-value", "power-source", "mode-value", "firmware-value",
      "minimum-value", "maximum-value", "trend-value", "history-chart", "chart-wrap",
      "chart-empty", "chart-description", "chart-coverage", "refresh-label"
    ];
    return Object.fromEntries(ids.map((id) => [id, document.getElementById(id)]));
  }

  async start() {
    this.bindEvents();
    this.elements["refresh-label"].textContent = `Updates every ${Math.round(this.config.pollIntervalMs / 1000)} seconds`;
    this.auth.onAuthStateChange((_event, session) => {
      setTimeout(() => this.applySession(session), 0);
    });
    await this.applySession(await this.auth.getSession());
  }

  bindEvents() {
    this.elements["retry-button"].addEventListener("click", () => this.refreshAll());
    this.elements["passkey-sign-in-button"].addEventListener("click", () => this.signInWithPasskey());
    this.elements["send-link-button"].addEventListener("click", () => this.sendEmailLink());
    this.elements["setup-email"].addEventListener("keydown", (event) => {
      if (event.key === "Enter") this.sendEmailLink();
    });
    this.elements["manage-passkeys-button"].addEventListener("click", () => this.toggleAccountPanel());
    this.elements["add-passkey-button"].addEventListener("click", () => this.addPasskey());
    this.elements["sign-out-button"].addEventListener("click", () => this.signOut());
    document.querySelectorAll("[data-hours]").forEach((button) => {
      button.addEventListener("click", () => {
        this.historyHours = Number(button.dataset.hours);
        document.querySelectorAll("[data-hours]").forEach((candidate) => {
          const selected = candidate === button;
          candidate.classList.toggle("selected", selected);
          candidate.setAttribute("aria-pressed", String(selected));
        });
        this.elements["history-title"].textContent = this.historyHours === 168 ? "Last 7 days" : `Last ${this.historyHours} hours`;
        this.fetchHistory().catch((error) => this.showHistoryError(error));
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
    const session = await this.auth.getSession();
    if (!session?.access_token) throw new AuthorizationError("Sign in with a passkey to view garden data.");
    this.session = session;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), this.config.requestTimeoutMs);
    try {
      const response = await fetch(buildApiUrl(this.config.apiBaseUrl, path, search), {
        cache: "no-store",
        credentials: "omit",
        headers: {
          Accept: "application/json",
          Authorization: `Bearer ${session.access_token}`
        },
        referrerPolicy: "no-referrer",
        redirect: "error",
        signal: controller.signal
      });
      if (response.status === 401 || response.status === 403) {
        const message = response.status === 403
          ? "This passkey account is not assigned to the garden sensor."
          : "Your passkey session expired. Sign in again.";
        throw new AuthorizationError(message);
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
        await this.fetchHistory().catch((error) => this.showHistoryError(error));
      }
      this.failureCount = 0;
      this.updateFreshnessNotice();
    } catch (error) {
      if (error instanceof AuthorizationError) {
        await this.auth.signOut().catch(() => {});
        this.session = null;
        this.showAccessGate(error.message);
        return;
      }
      this.failureCount += 1;
      this.showOffline(error instanceof Error ? error.message : "Could not reach the cloud API.");
    } finally {
      this.pollInProgress = false;
      if (this.session?.access_token) this.scheduleNextPoll();
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
    this.elements["power-value"].textContent = reading.powerMilliwatts === null
      ? "-- mW" : `${Math.round(reading.powerMilliwatts)} mW`;
    this.elements["current-value"].textContent = reading.currentMilliamps === null
      ? "Available in Live mode" : `${Math.round(reading.currentMilliamps)} mA`;
    this.elements["power-source"].textContent = reading.powerMeasured === null
      ? "No power telemetry in this sample"
      : (reading.powerMeasured ? "Measured by current sensor" : "Estimated — current sensor not fitted");
    const modeLabels = { live: "Live · 2s", fast: "30 seconds", low_power: "5 minutes" };
    this.elements["mode-value"].textContent = modeLabels[reading.samplingMode] || "--";
    const build = reading.sensorFirmwareBuild === null ? "" : ` · build ${Math.round(reading.sensorFirmwareBuild)}`;
    this.elements["firmware-value"].textContent = reading.sensorFirmware
      ? `Sensor ${reading.sensorFirmware}${build} · Gateway ${reading.gatewayFirmware || "unknown"}`
      : "Firmware unavailable";
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

  showHistoryError(error) {
    const detail = error instanceof Error ? error.message : "Could not load history";
    this.elements["chart-empty"].textContent =
      `History is temporarily unavailable (${detail}). Current readings will continue updating.`;
    if (this.elements["chart-coverage"]) {
      this.elements["chart-coverage"].textContent = this.history.length
        ? `History refresh failed: ${detail}. Showing the last loaded log.`
        : `History request failed: ${detail}.`;
    }
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

  async applySession(session) {
    if (!session?.access_token) {
      this.session = null;
      this.showAccessGate();
      return;
    }

    const sameSession = this.session?.access_token === session.access_token;
    this.session = session;
    this.elements["account-email"].textContent = session.user?.email || "Confirmed account";
    this.hideAccessGate();
    if (!this.cacheRestored) {
      this.restoreCache();
      this.cacheRestored = true;
    }
    if (!sameSession) this.refreshAll();
    this.renderPasskeys().catch((error) => this.showAccountMessage(authErrorMessage(error), true));
  }

  async signInWithPasskey() {
    if (!("PublicKeyCredential" in window)) {
      this.showAccessGate("This browser does not support passkeys. Use current Safari on your Mac or iPhone.");
      return;
    }
    this.setAuthBusy(true);
    this.showAccessGate();
    try {
      await this.applySession(await this.auth.signInWithPasskey());
    } catch (error) {
      this.showAccessGate(authErrorMessage(error));
    } finally {
      this.setAuthBusy(false);
    }
  }

  async sendEmailLink() {
    const email = this.elements["setup-email"].value.trim();
    if (!/^\S+@\S+\.\S+$/.test(email)) {
      this.showAccessGate("Enter the confirmed owner email address.");
      return;
    }
    this.setAuthBusy(true);
    try {
      const redirectTo = `${window.location.origin}${window.location.pathname}`;
      await this.auth.sendEmailLink(email, redirectTo);
      this.showAccessGate("Check your email and open the sign-in link on this device.", false);
    } catch (error) {
      this.showAccessGate(authErrorMessage(error));
    } finally {
      this.setAuthBusy(false);
    }
  }

  async addPasskey() {
    this.elements["add-passkey-button"].disabled = true;
    this.showAccountMessage("Follow the Face ID or Touch ID prompt…");
    try {
      await this.auth.registerPasskey();
      this.showAccountMessage("Passkey added. It can now sign in without an access key.");
      await this.renderPasskeys();
    } catch (error) {
      this.showAccountMessage(authErrorMessage(error), true);
    } finally {
      this.elements["add-passkey-button"].disabled = false;
    }
  }

  async renderPasskeys() {
    const passkeys = await this.auth.listPasskeys();
    this.elements["passkey-list"].replaceChildren();
    if (passkeys.length === 0) {
      const item = document.createElement("li");
      item.textContent = "No passkey yet — add one before signing out.";
      this.elements["passkey-list"].append(item);
      this.elements["account-panel"].hidden = false;
      return;
    }
    for (const passkey of passkeys) {
      const item = document.createElement("li");
      const name = passkey.friendly_name || passkey.friendlyName || "Passkey";
      const lastUsed = passkey.last_used_at || passkey.lastUsedAt;
      item.textContent = lastUsed
        ? `${name} · last used ${new Date(lastUsed).toLocaleDateString()}`
        : name;
      this.elements["passkey-list"].append(item);
    }
  }

  async signOut() {
    clearTimeout(this.pollTimer);
    try { await this.auth.signOut(); } catch (error) {
      this.showAccountMessage(authErrorMessage(error), true);
      return;
    }
    try { sessionStorage.removeItem(CACHE_KEY); } catch { /* Storage is optional. */ }
    this.session = null;
    this.current = null;
    this.history = [];
    this.showAccessGate();
  }

  setAuthBusy(busy) {
    this.elements["passkey-sign-in-button"].disabled = busy;
    this.elements["send-link-button"].disabled = busy;
  }

  showAccountMessage(message, isError = false) {
    this.elements["account-message"].hidden = !message;
    this.elements["account-message"].textContent = message;
    this.elements["account-message"].style.color = isError ? "var(--red)" : "var(--cyan)";
  }

  toggleAccountPanel() {
    this.elements["account-panel"].hidden = !this.elements["account-panel"].hidden;
  }

  showAccessGate(message = "", isError = true) {
    clearTimeout(this.pollTimer);
    document.getElementById("main").inert = true;
    this.setConnection("locked", "Locked");
    this.elements["access-gate"].hidden = false;
    this.elements["account-panel"].hidden = true;
    this.elements["manage-passkeys-button"].hidden = true;
    this.elements["access-error"].hidden = !message;
    this.elements["access-error"].textContent = message;
    this.elements["access-error"].style.color = isError ? "var(--red)" : "var(--cyan)";
  }

  hideAccessGate() {
    document.getElementById("main").inert = false;
    this.elements["access-gate"].hidden = true;
    this.elements["manage-passkeys-button"].hidden = false;
    this.elements["access-error"].hidden = true;
    this.elements["access-error"].textContent = "";
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

function authErrorMessage(error) {
  if (!(error instanceof Error)) return "Passkey authentication failed. Try again.";
  if (error.name === "NotAllowedError") {
    return "The passkey prompt was cancelled or timed out.";
  }
  return error.message || "Passkey authentication failed. Try again.";
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
    new SoilDashboard(config).start().catch((error) => {
      showConfigurationError(error instanceof Error ? error.message : "Authentication setup failed.");
    });
  } catch (error) {
    showConfigurationError(error instanceof Error ? error.message : "Review config.js.");
  }
}
