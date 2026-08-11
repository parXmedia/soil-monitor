export const DEFAULT_CONFIG = Object.freeze({
  supabaseUrl: "",
  publishableKey: "",
  apiBaseUrl: "",
  currentPath: "/v1/current",
  historyPath: "/v1/history",
  pollIntervalMs: 15000,
  historyRefreshIntervalMs: 300000,
  requestTimeoutMs: 8000,
  staleAfterMs: 360000,
  thresholds: Object.freeze({ dryBelow: 25, healthyBelow: 80 })
});

// The deployed dashboard intentionally refreshes at the live two-second
// cadence. Keep the floor aligned with the API's 120-requests/minute limiter.
const MIN_POLL_INTERVAL_MS = 2000;
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
    sequence: finiteNumber(payload.sequence),
    currentMilliamps: finiteNumber(payload.currentMilliamps),
    powerMilliwatts: finiteNumber(payload.powerMilliwatts),
    powerMeasured: typeof payload.powerMeasured === "boolean" ? payload.powerMeasured : null,
    samplingMode: ["live", "fast", "low_power"].includes(payload.samplingMode) ? payload.samplingMode : null,
    sensorFirmwareBuild: finiteNumber(payload.sensorFirmwareBuild),
    sensorFirmware: payload.sensorFirmware ? String(payload.sensorFirmware).slice(0, 32) : null,
    gatewayFirmware: payload.gatewayFirmware ? String(payload.gatewayFirmware).slice(0, 32) : null
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

  let supabaseUrl;
  try { supabaseUrl = new URL(config.supabaseUrl); } catch {
    throw new Error("supabaseUrl in config.js is not a valid URL.");
  }
  if (
    (supabaseUrl.protocol !== "https:" && !(pageIsLocal && supabaseUrl.protocol === "http:")) ||
    supabaseUrl.pathname !== "/"
  ) {
    throw new Error("supabaseUrl must be the HTTPS project origin without a path.");
  }
  if (url.origin !== supabaseUrl.origin) {
    throw new Error("supabaseUrl and apiBaseUrl must use the same Supabase project.");
  }
  if (
    typeof config.publishableKey !== "string" ||
    config.publishableKey === "PASTE_SUPABASE_PUBLISHABLE_KEY_HERE" ||
    !/^(sb_publishable_[A-Za-z0-9_-]{20,}|eyJ[A-Za-z0-9._-]{80,})$/.test(config.publishableKey)
  ) {
    throw new Error("Set publishableKey in config.js from Supabase Settings → API.");
  }

  config.supabaseUrl = supabaseUrl.origin;
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
