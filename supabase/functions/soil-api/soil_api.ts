const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });

export const MAX_BODY_BYTES = 2_048;
export const MAX_SEQUENCE = 4_294_967_295;
export const HISTORY_HOURS = new Set([6, 24, 168]);

// Bucket width per supported range, chosen so every response stays under a few
// hundred points regardless of the sampling cadence. The previous fixed
// 2,500-row limit meant a seven-day request at 30-second sampling returned
// only the newest ~21 hours with nothing to indicate the rest was missing.
export const HISTORY_BUCKET_SECONDS: Record<number, number> = {
  6: 120,
  24: 600,
  168: 3_600,
};

export function historyBucketSeconds(hours: number): number {
  const bucket = HISTORY_BUCKET_SECONDS[hours];
  if (bucket === undefined) {
    throw new ApiError(400, "invalid_range", "hours must be one of 6, 24, or 168");
  }
  return bucket;
}

export interface AuthenticatedEnvelope {
  deviceId: string;
  bootId: string;
  sequence: number;
  requestTimestamp: number;
  payloadHash: string;
}

export interface ValidTelemetry {
  schema: 1;
  sampled_at: string;
  moisture_pct: number;
  raw_adc: number;
  sensor_mv: number;
  espnow_rssi_dbm: number;
  battery_mv: number | null;
  battery_percent: number | null;
  current_ma: number | null;
  power_mw: number | null;
  power_measured: boolean | null;
  sampling_mode: "live" | "fast" | "low_power" | null;
  sensor_firmware_build: number | null;
  uptime_seconds: number | null;
  sensor_firmware: string | null;
  gateway_firmware: string | null;
}

export interface StoredTelemetry extends Omit<ValidTelemetry, "schema"> {
  id: number;
  device_id: string;
  boot_id: string;
  sequence: number;
  schema_version: 1;
  received_at: string;
  payload_sha256: string;
}

export class ApiError extends Error {
  readonly status: number;
  readonly code: string;

  constructor(
    status: number,
    code: string,
    message: string,
  ) {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.code = code;
  }
}

function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

function toBase64Url(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/, "");
}

function fromBase64Url(value: string): Uint8Array | null {
  if (!/^[A-Za-z0-9_-]{43}$/.test(value)) return null;
  try {
    const base64 = value.replaceAll("-", "+").replaceAll("_", "/") + "=";
    const decoded = atob(base64);
    return Uint8Array.from(decoded, (character) => character.charCodeAt(0));
  } catch {
    return null;
  }
}

export function constantTimeEqual(left: Uint8Array, right: Uint8Array): boolean {
  const maximum = Math.max(left.length, right.length);
  let difference = left.length ^ right.length;
  for (let index = 0; index < maximum; index += 1) {
    difference |= (left[index] ?? 0) ^ (right[index] ?? 0);
  }
  return difference === 0;
}

export async function sha256Hex(bytes: Uint8Array): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return toHex(new Uint8Array(digest));
}

export function canonicalIngestMessage(
  deviceId: string,
  bootId: string,
  sequence: number,
  timestamp: number,
  payloadHash: string,
): string {
  return `v1\n${deviceId}\n${bootId}\n${sequence}\n${timestamp}\n${payloadHash}`;
}

export async function hmacSignature(secret: string, canonical: string): Promise<Uint8Array> {
  const key = await crypto.subtle.importKey(
    "raw",
    encoder.encode(secret),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  return new Uint8Array(await crypto.subtle.sign("HMAC", key, encoder.encode(canonical)));
}

export async function hmacSignatureBase64Url(secret: string, canonical: string): Promise<string> {
  return toBase64Url(await hmacSignature(secret, canonical));
}

function requiredHeader(headers: Headers, name: string): string {
  const value = headers.get(name)?.trim();
  if (!value) throw new ApiError(401, "unauthorized", "Authentication failed");
  return value;
}

function parseUnsignedInteger(value: string, maximum: number): number | null {
  if (!/^(0|[1-9][0-9]*)$/.test(value)) return null;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed <= maximum ? parsed : null;
}

export async function authenticateIngest(
  headers: Headers,
  body: Uint8Array,
  expectedDeviceId: string,
  secret: string,
  nowSeconds: number,
  maxClockSkewSeconds: number,
): Promise<AuthenticatedEnvelope> {
  const deviceId = requiredHeader(headers, "x-device-id");
  const bootId = requiredHeader(headers, "x-boot-id").toLowerCase();
  const sequenceText = requiredHeader(headers, "x-sequence");
  const timestampText = requiredHeader(headers, "x-timestamp");
  const suppliedSignature = fromBase64Url(requiredHeader(headers, "x-signature"));

  const sequence = parseUnsignedInteger(sequenceText, MAX_SEQUENCE);
  const requestTimestamp = parseUnsignedInteger(timestampText, 4_102_444_800);
  const bootIdPattern =
    /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
  const bootIdValid = bootIdPattern.test(bootId);
  if (
    deviceId !== expectedDeviceId || !bootIdValid || sequence === null ||
    requestTimestamp === null || suppliedSignature === null || secret.length < 32
  ) {
    throw new ApiError(401, "unauthorized", "Authentication failed");
  }

  const payloadHash = await sha256Hex(body);
  const canonical = canonicalIngestMessage(
    deviceId,
    bootId,
    sequence,
    requestTimestamp,
    payloadHash,
  );
  const expectedSignature = await hmacSignature(secret, canonical);
  if (!constantTimeEqual(suppliedSignature, expectedSignature)) {
    throw new ApiError(401, "unauthorized", "Authentication failed");
  }

  if (Math.abs(nowSeconds - requestTimestamp) > maxClockSkewSeconds) {
    throw new ApiError(401, "unauthorized", "Authentication failed");
  }

  return { deviceId, bootId, sequence, requestTimestamp, payloadHash };
}

function finiteNumber(value: unknown, name: string, minimum: number, maximum: number): number {
  if (typeof value !== "number" || !Number.isFinite(value) || value < minimum || value > maximum) {
    throw new ApiError(422, "invalid_reading", `${name} is outside the accepted range`);
  }
  return value;
}

function integer(value: unknown, name: string, minimum: number, maximum: number): number {
  const parsed = finiteNumber(value, name, minimum, maximum);
  if (!Number.isInteger(parsed)) {
    throw new ApiError(422, "invalid_reading", `${name} must be an integer`);
  }
  return parsed;
}

function optionalInteger(
  value: unknown,
  name: string,
  minimum: number,
  maximum: number,
): number | null {
  return value === undefined || value === null ? null : integer(value, name, minimum, maximum);
}

function optionalNumber(
  value: unknown,
  name: string,
  minimum: number,
  maximum: number,
): number | null {
  return value === undefined || value === null ? null : finiteNumber(value, name, minimum, maximum);
}

function optionalVersion(value: unknown, name: string): string | null {
  if (value === undefined || value === null) return null;
  if (typeof value !== "string" || !/^[A-Za-z0-9._+-]{1,32}$/.test(value)) {
    throw new ApiError(422, "invalid_reading", `${name} has an invalid format`);
  }
  return value;
}

function optionalBoolean(value: unknown, name: string): boolean | null {
  if (value === undefined || value === null) return null;
  if (typeof value !== "boolean") {
    throw new ApiError(422, "invalid_reading", `${name} must be a boolean`);
  }
  return value;
}

function optionalSamplingMode(value: unknown): "live" | "fast" | "low_power" | null {
  if (value === undefined || value === null) return null;
  if (value !== "live" && value !== "fast" && value !== "low_power") {
    throw new ApiError(422, "invalid_reading", "sampling_mode is not supported");
  }
  return value;
}

export function decodeJsonBody(body: Uint8Array): unknown {
  if (body.byteLength === 0) throw new ApiError(400, "empty_body", "JSON body is required");
  if (body.byteLength > MAX_BODY_BYTES) {
    throw new ApiError(413, "body_too_large", "Request body is too large");
  }
  try {
    return JSON.parse(decoder.decode(body));
  } catch {
    throw new ApiError(400, "invalid_json", "Request body must be valid UTF-8 JSON");
  }
}

export function validateTelemetry(value: unknown, nowMilliseconds: number): ValidTelemetry {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    throw new ApiError(422, "invalid_reading", "Telemetry must be a JSON object");
  }
  const input = value as Record<string, unknown>;
  const allowedKeys = new Set([
    "schema", "sampled_at", "moisture_pct", "raw_adc", "sensor_mv",
    "espnow_rssi_dbm", "battery_mv", "battery_percent", "uptime_seconds",
    "sensor_firmware", "gateway_firmware", "current_ma", "power_mw",
    "power_measured", "sampling_mode", "sensor_firmware_build",
  ]);
  if (Object.keys(input).some((key) => !allowedKeys.has(key))) {
    throw new ApiError(422, "invalid_reading", "Telemetry contains an unknown field");
  }
  if (input.schema !== 1) throw new ApiError(422, "invalid_reading", "schema must be 1");
  if (typeof input.sampled_at !== "string") {
    throw new ApiError(422, "invalid_reading", "sampled_at must be an ISO-8601 timestamp");
  }
  const timestampPattern =
    /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?Z$/;
  const timestampMatch = timestampPattern.exec(input.sampled_at);
  if (timestampMatch === null) {
    throw new ApiError(422, "invalid_reading", "sampled_at must be a UTC ISO-8601 timestamp");
  }
  const [, year, month, day, hour, minute, second, fraction = "0"] = timestampMatch;
  const sampledMilliseconds = Date.UTC(
    Number(year), Number(month) - 1, Number(day), Number(hour), Number(minute),
    Number(second), Number(fraction.padEnd(3, "0")),
  );
  const canonicalSampledAt = new Date(sampledMilliseconds).toISOString();
  const expectedCanonical =
    `${year}-${month}-${day}T${hour}:${minute}:${second}.${fraction.padEnd(3, "0")}Z`;
  const oldestAccepted = nowMilliseconds - 30 * 24 * 60 * 60 * 1000;
  const newestAccepted = nowMilliseconds + 5 * 60 * 1000;
  if (
    canonicalSampledAt !== expectedCanonical || sampledMilliseconds < oldestAccepted ||
    sampledMilliseconds > newestAccepted
  ) {
    throw new ApiError(422, "invalid_reading", "sampled_at is outside the accepted range");
  }

  const currentMa = optionalInteger(input.current_ma, "current_ma", 0, 20_000);
  const powerMw = optionalInteger(input.power_mw, "power_mw", 0, 100_000);
  const powerMeasured = optionalBoolean(input.power_measured, "power_measured");
  if ((currentMa === null) !== (powerMw === null) ||
      (powerMeasured !== null && currentMa === null)) {
    throw new ApiError(422, "invalid_reading", "power telemetry fields are inconsistent");
  }

  return {
    schema: 1,
    sampled_at: canonicalSampledAt,
    moisture_pct: finiteNumber(input.moisture_pct, "moisture_pct", 0, 100),
    raw_adc: integer(input.raw_adc, "raw_adc", 0, 4095),
    sensor_mv: integer(input.sensor_mv, "sensor_mv", 0, 5000),
    espnow_rssi_dbm: integer(input.espnow_rssi_dbm, "espnow_rssi_dbm", -127, 0),
    battery_mv: optionalInteger(input.battery_mv, "battery_mv", 2500, 5500),
    battery_percent: optionalNumber(input.battery_percent, "battery_percent", 0, 100),
    current_ma: currentMa,
    power_mw: powerMw,
    power_measured: powerMeasured,
    sampling_mode: optionalSamplingMode(input.sampling_mode),
    sensor_firmware_build: optionalInteger(
      input.sensor_firmware_build, "sensor_firmware_build", 1, MAX_SEQUENCE,
    ),
    uptime_seconds: optionalInteger(input.uptime_seconds, "uptime_seconds", 0, MAX_SEQUENCE),
    sensor_firmware: optionalVersion(input.sensor_firmware, "sensor_firmware"),
    gateway_firmware: optionalVersion(input.gateway_firmware, "gateway_firmware"),
  };
}

export function parseHistoryHours(value: string | null): number {
  if (value === null || value === "") return 24;
  const hours = Number(value);
  if (!Number.isInteger(hours) || !HISTORY_HOURS.has(hours)) {
    throw new ApiError(400, "invalid_range", "hours must be one of 6, 24, or 168");
  }
  return hours;
}

export function bearerAccessToken(headers: Headers): string {
  const authorization = headers.get("authorization") ?? "";
  const match = /^Bearer ([A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+)$/.exec(authorization);
  if (match === null || match[1].length > 4096) {
    throw new ApiError(401, "unauthorized", "Authentication failed");
  }
  return match[1];
}

export interface SeriesPoint {
  bucket: string;
  sample_count: number;
  moisture_avg: number | string;
  moisture_min: number | string | null;
  moisture_max: number | string | null;
  raw_adc_avg: number | null;
  sensor_mv_avg: number | null;
  rssi_avg: number | null;
  battery_mv_avg: number | null;
}

export function toDashboardSeriesPoint(point: SeriesPoint): Record<string, unknown> {
  const rssi = point.rssi_avg ?? -127;
  return {
    timestamp: point.bucket,
    moisture: Number(point.moisture_avg),
    moistureMin: point.moisture_min === null ? null : Number(point.moisture_min),
    moistureMax: point.moisture_max === null ? null : Number(point.moisture_max),
    rawAdc: point.raw_adc_avg,
    millivolts: point.sensor_mv_avg,
    rssi,
    signal: signalFromRssi(rssi),
    batteryVoltage: point.battery_mv_avg === null ? null : point.battery_mv_avg / 1000,
    batteryPercent: null,
    sampleCount: point.sample_count,
  };
}

// Fixed-window counter keyed by client address. Authentication already runs
// before any database work, so this exists to cap Supabase invocation spend
// during a flood rather than to protect the data itself.
export class RateLimiter {
  // Declared as plain fields rather than constructor parameter properties so
  // the suite also runs under Node's strip-only TypeScript loader.
  readonly limit: number;
  readonly windowMs: number;
  private readonly hits = new Map<string, { count: number; resetAt: number }>();

  constructor(limit: number, windowMs: number) {
    this.limit = limit;
    this.windowMs = windowMs;
  }

  check(key: string, nowMs: number): { allowed: boolean; retryAfterSeconds: number } {
    const existing = this.hits.get(key);
    if (existing === undefined || nowMs >= existing.resetAt) {
      // Opportunistic sweep keeps the map from growing without bound on an
      // isolate that stays warm for a long time.
      if (this.hits.size > 1_000) {
        for (const [entryKey, entry] of this.hits) {
          if (nowMs >= entry.resetAt) this.hits.delete(entryKey);
        }
      }
      this.hits.set(key, { count: 1, resetAt: nowMs + this.windowMs });
      return { allowed: true, retryAfterSeconds: 0 };
    }

    existing.count += 1;
    if (existing.count > this.limit) {
      return {
        allowed: false,
        retryAfterSeconds: Math.max(1, Math.ceil((existing.resetAt - nowMs) / 1000)),
      };
    }
    return { allowed: true, retryAfterSeconds: 0 };
  }
}

export function clientKey(headers: Headers): string {
  // Supabase Edge runs behind Cloudflare, which supplies this header from the
  // connection rather than trusting the value sent by the caller.
  const cloudflare = headers.get("cf-connecting-ip")?.trim();
  if (cloudflare && cloudflare.length <= 64) return cloudflare;

  // Proxies append their observed address to X-Forwarded-For. The left-most
  // value can be supplied by the caller, so rate-limit on the last value.
  const forwarded = headers.get("x-forwarded-for");
  if (forwarded !== null && forwarded.length > 0) {
    const last = forwarded.split(",").at(-1)?.trim();
    if (last && last.length <= 64) return last;
  }
  return "unknown";
}

export function signalFromRssi(rssi: number): number {
  if (rssi >= -50) return 100;
  if (rssi <= -100) return 0;
  return Math.round((rssi + 100) * 2);
}

export function toDashboardReading(row: StoredTelemetry): Record<string, unknown> {
  return {
    deviceId: row.device_id,
    timestamp: row.sampled_at,
    receivedAt: row.received_at,
    moisture: Number(row.moisture_pct),
    rawAdc: row.raw_adc,
    millivolts: row.sensor_mv,
    rssi: row.espnow_rssi_dbm,
    signal: signalFromRssi(row.espnow_rssi_dbm),
    batteryVoltage: row.battery_mv === null ? null : row.battery_mv / 1000,
    batteryPercent: row.battery_percent === null ? null : Number(row.battery_percent),
    currentMilliamps: row.current_ma,
    powerMilliwatts: row.power_mw,
    powerMeasured: row.power_measured,
    samplingMode: row.sampling_mode,
    sensorFirmwareBuild: row.sensor_firmware_build,
    sensorFirmware: row.sensor_firmware,
    gatewayFirmware: row.gateway_firmware,
    sequence: row.sequence,
  };
}
