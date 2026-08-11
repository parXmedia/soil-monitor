import {
  ApiError,
  authenticateIngest,
  bearerAccessToken,
  clientKey,
  decodeJsonBody,
  historyBucketSeconds,
  MAX_BODY_BYTES,
  parseHistoryHours,
  RateLimiter,
  sha256Hex,
  toDashboardReading,
  toDashboardSeriesPoint,
  validateTelemetry,
} from "./soil_api.ts";
import type { SeriesPoint, StoredTelemetry } from "./soil_api.ts";

// Generous enough that the dashboard's 2-second poll and the gateway's
// per-sample upload never notice, tight enough to blunt a scripted flood.
const readLimiter = new RateLimiter(120, 60_000);
const ingestLimiter = new RateLimiter(60, 60_000);
const authCache = new Map<string, { userId: string; expiresAt: number }>();
const authEncoder = new TextEncoder();
const AUTH_CACHE_MS = 30_000;

interface RuntimeConfig {
  supabaseUrl: string;
  serviceRoleKey: string;
  deviceId: string;
  ingestSecret: string;
  allowedOrigin: string;
  maxClockSkewSeconds: number;
}

interface PostgrestError {
  code?: string;
  message?: string;
}

function requiredEnvironment(name: string): string {
  const value = Deno.env.get(name)?.trim();
  if (!value) throw new Error(`Missing required environment variable: ${name}`);
  return value;
}

function loadConfig(): RuntimeConfig {
  const supabaseUrl = requiredEnvironment("SUPABASE_URL").replace(/\/$/, "");
  const allowedOriginInput = requiredEnvironment("SOIL_ALLOWED_ORIGIN");
  const allowedUrl = new URL(allowedOriginInput);
  if (allowedUrl.protocol !== "https:" || allowedUrl.origin !== allowedOriginInput) {
    throw new Error("SOIL_ALLOWED_ORIGIN must be one exact HTTPS origin without a trailing slash");
  }

  const maxClockSkewText = Deno.env.get("SOIL_MAX_CLOCK_SKEW_SECONDS") ?? "300";
  const maxClockSkewSeconds = Number(maxClockSkewText);
  if (
    !Number.isInteger(maxClockSkewSeconds) || maxClockSkewSeconds < 30 ||
    maxClockSkewSeconds > 900
  ) {
    throw new Error("SOIL_MAX_CLOCK_SKEW_SECONDS must be an integer from 30 through 900");
  }

  const deviceId = requiredEnvironment("SOIL_DEVICE_ID");
  if (!/^[a-z0-9][a-z0-9_-]{2,63}$/.test(deviceId)) {
    throw new Error("SOIL_DEVICE_ID has an invalid format");
  }

  const ingestSecret = requiredEnvironment("SOIL_INGEST_SECRET");
  if (ingestSecret.length < 32) {
    throw new Error("SOIL_INGEST_SECRET must be at least 32 characters");
  }

  return {
    supabaseUrl,
    serviceRoleKey: requiredEnvironment("SUPABASE_SERVICE_ROLE_KEY"),
    deviceId,
    ingestSecret,
    allowedOrigin: allowedUrl.origin,
    maxClockSkewSeconds,
  };
}

function corsHeaders(requestOrigin: string | null, config: RuntimeConfig): HeadersInit {
  const headers: Record<string, string> = {
    "Access-Control-Allow-Headers": [
      "authorization", "content-type", "x-device-id", "x-boot-id", "x-sequence",
      "x-timestamp", "x-signature",
    ].join(", "),
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Max-Age": "600",
    "Cache-Control": "no-store",
    "Content-Security-Policy": "default-src 'none'; frame-ancestors 'none'",
    "Referrer-Policy": "no-referrer",
    "Vary": "Origin",
    "X-Content-Type-Options": "nosniff",
  };
  if (requestOrigin === config.allowedOrigin) {
    headers["Access-Control-Allow-Origin"] = config.allowedOrigin;
  }
  return headers;
}

function jsonResponse(
  status: number,
  body: Record<string, unknown>,
  requestOrigin: string | null,
  config: RuntimeConfig,
): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      ...corsHeaders(requestOrigin, config),
    },
  });
}

function edgeRoute(pathname: string): string {
  const marker = "/soil-api";
  const markerIndex = pathname.lastIndexOf(marker);
  const route = markerIndex >= 0 ? pathname.slice(markerIndex + marker.length) : pathname;
  return route.replace(/\/$/, "") || "/";
}

function databaseHeaders(config: RuntimeConfig, extra: HeadersInit = {}): Headers {
  const headers = new Headers(extra);
  headers.set("apikey", config.serviceRoleKey);
  headers.set("Authorization", `Bearer ${config.serviceRoleKey}`);
  return headers;
}

async function readBoundedBody(request: Request, maximumBytes: number): Promise<Uint8Array> {
  if (request.body === null) return new Uint8Array();
  const reader = request.body.getReader();
  const chunks: Uint8Array[] = [];
  let totalBytes = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      totalBytes += value.byteLength;
      if (totalBytes > maximumBytes) {
        try {
          await reader.cancel("request body exceeds limit");
        } catch {
          // The body is already rejected; a peer disconnect must not mask 413.
        }
        throw new ApiError(413, "body_too_large", "Request body is too large");
      }
      chunks.push(value);
    }
  } finally {
    reader.releaseLock();
  }

  const body = new Uint8Array(totalBytes);
  let offset = 0;
  for (const chunk of chunks) {
    body.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return body;
}

function restUrl(config: RuntimeConfig, table: string, parameters: Record<string, string>): URL {
  const url = new URL(`/rest/v1/${table}`, config.supabaseUrl);
  for (const [key, value] of Object.entries(parameters)) url.searchParams.set(key, value);
  return url;
}

async function deviceIsEnabled(config: RuntimeConfig): Promise<boolean> {
  const url = restUrl(config, "devices", {
    select: "device_id,enabled",
    device_id: `eq.${config.deviceId}`,
    limit: "1",
  });
  const response = await fetch(url, { headers: databaseHeaders(config) });
  if (!response.ok) throw new Error(`device lookup failed (${response.status})`);
  const rows = await response.json() as Array<{ device_id: string; enabled: boolean }>;
  return rows.length === 1 && rows[0].enabled === true;
}

async function authenticatedUserId(headers: Headers, config: RuntimeConfig): Promise<string> {
  const token = bearerAccessToken(headers);
  const cacheKey = await sha256Hex(authEncoder.encode(token));
  const now = Date.now();
  const cached = authCache.get(cacheKey);
  if (cached && now < cached.expiresAt) return cached.userId;

  const url = new URL("/auth/v1/user", config.supabaseUrl);
  const response = await fetch(url, {
    headers: {
      apikey: config.serviceRoleKey,
      Authorization: `Bearer ${token}`,
    },
  });
  if (!response.ok) throw new ApiError(401, "unauthorized", "Authentication failed");
  const user = await response.json() as { id?: unknown };
  const userIdPattern =
    /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
  if (typeof user.id !== "string" || !userIdPattern.test(user.id)) {
    throw new ApiError(401, "unauthorized", "Authentication failed");
  }

  if (authCache.size > 1000) {
    for (const [key, entry] of authCache) {
      if (now >= entry.expiresAt) authCache.delete(key);
    }
  }
  authCache.set(cacheKey, { userId: user.id, expiresAt: now + AUTH_CACHE_MS });
  return user.id;
}

async function authorizeDashboardRead(headers: Headers, config: RuntimeConfig): Promise<void> {
  const userId = await authenticatedUserId(headers, config);
  const url = restUrl(config, "devices", {
    select: "owner_id,enabled",
    device_id: `eq.${config.deviceId}`,
    limit: "1",
  });
  const response = await fetch(url, { headers: databaseHeaders(config) });
  if (!response.ok) throw new Error(`device authorization lookup failed (${response.status})`);
  const rows = await response.json() as Array<{ owner_id: string | null; enabled: boolean }>;
  if (rows.length !== 1 || rows[0].enabled !== true || rows[0].owner_id !== userId) {
    throw new ApiError(403, "not_device_owner", "This account cannot read the device");
  }
}

async function findDuplicate(
  config: RuntimeConfig,
  bootId: string,
  sequence: number,
): Promise<StoredTelemetry | null> {
  const url = restUrl(config, "telemetry", {
    select: "*",
    device_id: `eq.${config.deviceId}`,
    boot_id: `eq.${bootId}`,
    sequence: `eq.${sequence}`,
    limit: "1",
  });
  const response = await fetch(url, { headers: databaseHeaders(config) });
  if (!response.ok) throw new Error(`duplicate lookup failed (${response.status})`);
  const rows = await response.json() as StoredTelemetry[];
  return rows[0] ?? null;
}

async function insertTelemetry(
  config: RuntimeConfig,
  row: Record<string, unknown>,
): Promise<{ row: StoredTelemetry; duplicate: boolean }> {
  const url = restUrl(config, "telemetry", { select: "*" });
  const response = await fetch(url, {
    method: "POST",
    headers: databaseHeaders(config, {
      "Content-Type": "application/json",
      "Prefer": "return=representation",
    }),
    body: JSON.stringify(row),
  });

  if (response.ok) {
    const rows = await response.json() as StoredTelemetry[];
    if (rows.length !== 1) throw new Error("database returned an unexpected insert result");
    return { row: rows[0], duplicate: false };
  }

  const error = await response.json().catch(() => ({})) as PostgrestError;
  if (response.status !== 409 || error.code !== "23505") {
    throw new Error(`telemetry insert failed (${response.status}, ${error.code ?? "unknown"})`);
  }

  const duplicate = await findDuplicate(config, String(row.boot_id), Number(row.sequence));
  if (!duplicate || duplicate.payload_sha256 !== row.payload_sha256) {
    throw new ApiError(
      409,
      "sequence_conflict",
      "Sequence was already used for a different payload",
    );
  }
  return { row: duplicate, duplicate: true };
}

async function currentTelemetry(config: RuntimeConfig): Promise<StoredTelemetry | null> {
  const stateUrl = restUrl(config, "device_state", {
    select: "latest_telemetry_id",
    device_id: `eq.${config.deviceId}`,
    limit: "1",
  });
  const stateResponse = await fetch(stateUrl, { headers: databaseHeaders(config) });
  if (!stateResponse.ok) throw new Error(`state lookup failed (${stateResponse.status})`);
  const states = await stateResponse.json() as Array<{ latest_telemetry_id: number }>;
  if (states.length === 0) return null;

  const readingUrl = restUrl(config, "telemetry", {
    select: "*",
    id: `eq.${states[0].latest_telemetry_id}`,
    limit: "1",
  });
  const readingResponse = await fetch(readingUrl, { headers: databaseHeaders(config) });
  if (!readingResponse.ok) {
    throw new Error(`current reading lookup failed (${readingResponse.status})`);
  }
  const rows = await readingResponse.json() as StoredTelemetry[];
  return rows[0] ?? null;
}

async function historySeries(
  config: RuntimeConfig,
  hours: number,
): Promise<SeriesPoint[] | null> {
  const bucketSeconds = historyBucketSeconds(hours);
  const since = new Date(Date.now() - hours * 60 * 60 * 1000).toISOString();
  const url = new URL("/rest/v1/rpc/telemetry_series", config.supabaseUrl);
  const response = await fetch(url, {
    method: "POST",
    headers: databaseHeaders(config, { "Content-Type": "application/json" }),
    body: JSON.stringify({
      target_device_id: config.deviceId,
      since,
      bucket_seconds: bucketSeconds,
    }),
  });
  // The current-reading route does not depend on telemetry_series, so it can
  // keep working when the Edge Function is deployed before its companion
  // database migration. Treat PostgREST's missing-function response as a
  // rolling-deploy state and fall back to a bounded raw query below.
  if (response.status === 404) return null;
  if (!response.ok) throw new Error(`history lookup failed (${response.status})`);
  return await response.json() as SeriesPoint[];
}

async function rawHistoryTelemetry(
  config: RuntimeConfig,
  hours: number,
): Promise<StoredTelemetry[]> {
  const since = new Date(Date.now() - hours * 60 * 60 * 1000).toISOString();
  const url = restUrl(config, "telemetry", {
    select: "*",
    device_id: `eq.${config.deviceId}`,
    sampled_at: `gte.${since}`,
    order: "sampled_at.desc,id.desc",
    limit: "2000",
  });
  const response = await fetch(url, { headers: databaseHeaders(config) });
  if (!response.ok) throw new Error(`raw history lookup failed (${response.status})`);
  const newestFirst = await response.json() as StoredTelemetry[];
  return newestFirst.reverse();
}

async function handleRequest(request: Request, config: RuntimeConfig): Promise<Response> {
  const origin = request.headers.get("origin");
  if (origin !== null && origin !== config.allowedOrigin) {
    return jsonResponse(403, { error: "origin_not_allowed" }, origin, config);
  }

  if (request.method === "OPTIONS") {
    if (origin !== config.allowedOrigin) {
      return jsonResponse(403, { error: "origin_not_allowed" }, origin, config);
    }
    return new Response(null, { status: 204, headers: corsHeaders(origin, config) });
  }

  const url = new URL(request.url);
  const route = edgeRoute(url.pathname);
  const caller = clientKey(request.headers);
  const limiter = route === "/v1/ingest" ? ingestLimiter : readLimiter;
  const quota = limiter.check(caller, Date.now());
  if (!quota.allowed) {
    return new Response(JSON.stringify({ error: "rate_limited" }), {
      status: 429,
      headers: {
        "Content-Type": "application/json; charset=utf-8",
        "Retry-After": String(quota.retryAfterSeconds),
        ...corsHeaders(origin, config),
      },
    });
  }

  if (request.method === "POST" && route === "/v1/ingest") {
    const contentType = request.headers.get("content-type")
      ?.split(";", 1)[0].trim().toLowerCase();
    if (contentType !== "application/json") {
      throw new ApiError(415, "unsupported_media_type", "Content-Type must be application/json");
    }
    const declaredLength = Number(request.headers.get("content-length") ?? "0");
    if (Number.isFinite(declaredLength) && declaredLength > MAX_BODY_BYTES) {
      throw new ApiError(413, "body_too_large", "Request body is too large");
    }

    const body = await readBoundedBody(request, MAX_BODY_BYTES);
    const nowMilliseconds = Date.now();
    const envelope = await authenticateIngest(
      request.headers,
      body,
      config.deviceId,
      config.ingestSecret,
      Math.floor(nowMilliseconds / 1000),
      config.maxClockSkewSeconds,
    );
    const telemetry = validateTelemetry(decodeJsonBody(body), nowMilliseconds);
    if (!await deviceIsEnabled(config)) {
      throw new ApiError(403, "device_disabled", "Device is not enabled");
    }

    const { schema, ...measurement } = telemetry;
    const result = await insertTelemetry(config, {
      ...measurement,
      schema_version: schema,
      device_id: envelope.deviceId,
      boot_id: envelope.bootId,
      sequence: envelope.sequence,
      payload_sha256: envelope.payloadHash,
    });
    return jsonResponse(result.duplicate ? 200 : 201, {
      accepted: true,
      duplicate: result.duplicate,
      receivedAt: result.row.received_at,
    }, origin, config);
  }

  if (request.method === "GET" && route === "/v1/current") {
    await authorizeDashboardRead(request.headers, config);
    const row = await currentTelemetry(config);
    if (!row) throw new ApiError(404, "no_readings", "No readings are available");
    return jsonResponse(200, toDashboardReading(row), origin, config);
  }

  if (request.method === "GET" && route === "/v1/history") {
    await authorizeDashboardRead(request.headers, config);
    const hours = parseHistoryHours(url.searchParams.get("hours"));
    const points = await historySeries(config, hours);
    const usingRawFallback = points === null;
    const readings = usingRawFallback
      ? (await rawHistoryTelemetry(config, hours)).map(toDashboardReading)
      : points.map(toDashboardSeriesPoint);
    return jsonResponse(200, {
      hours,
      bucketSeconds: usingRawFallback ? null : historyBucketSeconds(hours),
      // Stated explicitly so the dashboard can show the span it actually has
      // rather than implying the full window was returned.
      coveredFrom: readings.at(0)?.timestamp ?? null,
      coveredTo: readings.at(-1)?.timestamp ?? null,
      readings,
    }, origin, config);
  }

  if (route === "/v1/ingest" || route === "/v1/current" || route === "/v1/history") {
    return jsonResponse(405, { error: "method_not_allowed" }, origin, config);
  }
  return jsonResponse(404, { error: "not_found" }, origin, config);
}

Deno.serve(async (request: Request): Promise<Response> => {
  let config: RuntimeConfig;
  try {
    config = loadConfig();
  } catch (error) {
    console.error(
      "soil-api configuration error",
      error instanceof Error ? error.message : "unknown",
    );
    return new Response(JSON.stringify({ error: "service_unavailable" }), {
      status: 503,
      headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" },
    });
  }

  try {
    return await handleRequest(request, config);
  } catch (error) {
    if (error instanceof ApiError) {
      return jsonResponse(
        error.status,
        { error: error.code, message: error.message },
        request.headers.get("origin"),
        config,
      );
    }
    console.error(
      "soil-api internal error",
      error instanceof Error ? error.message : "unknown",
    );
    return jsonResponse(
      503,
      { error: "service_unavailable" },
      request.headers.get("origin"),
      config,
    );
  }
});
