import {
  ApiError,
  authenticateIngest,
  bearerAccessToken,
  canonicalIngestMessage,
  clientKey,
  decodeJsonBody,
  historyBucketSeconds,
  hmacSignatureBase64Url,
  parseHistoryHours,
  RateLimiter,
  sha256Hex,
  signalFromRssi,
  toDashboardReading,
  toDashboardSeriesPoint,
  validateTelemetry,
} from "../functions/soil-api/soil_api.ts";

function assert(
  condition: unknown,
  message = "assertion failed",
): asserts condition {
  if (!condition) throw new Error(message);
}

async function assertRejectsCode(
  operation: () => unknown | Promise<unknown>,
  code: string,
): Promise<void> {
  try {
    await operation();
  } catch (error) {
    assert(error instanceof ApiError, "expected ApiError");
    assert(error.code === code, `expected ${code}, received ${error.code}`);
    return;
  }
  throw new Error(`expected ${code} rejection`);
}

Deno.test("canonical HMAC has a stable external test vector", async () => {
  const body = new TextEncoder().encode('{"schema":1}');
  const payloadHash = await sha256Hex(body);
  const canonical = canonicalIngestMessage(
    "garden-01",
    "018f47b8-68d8-7c8e-9c33-1b5d62f58b61",
    42,
    1_754_761_943,
    payloadHash,
  );
  const signature = await hmacSignatureBase64Url(
    "test-only-secret-0123456789-abcdefghijklmnopqrstuvwxyz",
    canonical,
  );
  assert(
    signature === "Psif0CqsToRc6ztt6HdC744_vRxI3DSxmY5J-jDxNos",
    `unexpected signature: ${signature}`,
  );
});

Deno.test("ingest authentication accepts signed fresh request", async () => {
  const now = 1_754_761_943;
  const secret = "test-only-secret-0123456789-abcdefghijklmnopqrstuvwxyz";
  const body = new TextEncoder().encode('{"schema":1}');
  const payloadHash = await sha256Hex(body);
  const canonical = canonicalIngestMessage(
    "garden-01",
    "018f47b8-68d8-7c8e-9c33-1b5d62f58b61",
    42,
    now,
    payloadHash,
  );
  const headers = new Headers({
    "x-device-id": "garden-01",
    "x-boot-id": "018f47b8-68d8-7c8e-9c33-1b5d62f58b61",
    "x-sequence": "42",
    "x-timestamp": String(now),
    "x-signature": await hmacSignatureBase64Url(secret, canonical),
  });
  const result = await authenticateIngest(
    headers,
    body,
    "garden-01",
    secret,
    now,
    300,
  );
  assert(result.sequence === 42);
  assert(result.payloadHash === payloadHash);
});

Deno.test("ingest authentication rejects stale and modified requests", async () => {
  const now = 1_754_761_943;
  const timestamp = now - 301;
  const secret = "test-only-secret-0123456789-abcdefghijklmnopqrstuvwxyz";
  const body = new TextEncoder().encode('{"schema":1}');
  const payloadHash = await sha256Hex(body);
  const canonical = canonicalIngestMessage(
    "garden-01",
    "018f47b8-68d8-7c8e-9c33-1b5d62f58b61",
    7,
    timestamp,
    payloadHash,
  );
  const headers = new Headers({
    "x-device-id": "garden-01",
    "x-boot-id": "018f47b8-68d8-7c8e-9c33-1b5d62f58b61",
    "x-sequence": "7",
    "x-timestamp": String(timestamp),
    "x-signature": await hmacSignatureBase64Url(secret, canonical),
  });
  await assertRejectsCode(
    () => authenticateIngest(headers, body, "garden-01", secret, now, 300),
    "unauthorized",
  );
  await assertRejectsCode(
    () =>
      authenticateIngest(
        headers,
        new TextEncoder().encode('{"schema":2}'),
        "garden-01",
        secret,
        timestamp,
        300,
      ),
    "unauthorized",
  );
});

Deno.test("telemetry validator enforces ranges and canonicalizes time", async () => {
  const now = Date.parse("2026-08-09T18:30:00.000Z");
  const reading = validateTelemetry({
    schema: 1,
    sampled_at: "2026-08-09T18:25:43Z",
    moisture_pct: 42.6,
    raw_adc: 2184,
    sensor_mv: 1760,
    espnow_rssi_dbm: -72,
    battery_mv: 4050,
    battery_percent: 82,
    current_ma: 96,
    power_mw: 389,
    power_measured: true,
    sampling_mode: "live",
    sensor_firmware_build: 196609,
    uptime_seconds: 600,
    sensor_firmware: "2.0.0",
    gateway_firmware: "2.0.0",
  }, now);
  assert(reading.sampled_at === "2026-08-09T18:25:43.000Z");
  assert(reading.moisture_pct === 42.6);
  assert(reading.battery_mv === 4050);
  assert(reading.power_mw === 389);
  assert(reading.sampling_mode === "live");

  const sleeping = validateTelemetry({
    ...reading,
    current_ma: null,
    power_mw: null,
    power_measured: null,
    sampling_mode: "low_power",
  }, now);
  assert(sleeping.current_ma === null);
  assert(sleeping.power_measured === null);

  await assertRejectsCode(
    () => validateTelemetry({ ...reading, moisture_pct: 101 }, now),
    "invalid_reading",
  );
  await assertRejectsCode(
    () => validateTelemetry({ ...reading, power_mw: null }, now),
    "invalid_reading",
  );
  await assertRejectsCode(
    () => validateTelemetry({ ...reading, unexpected: true }, now),
    "invalid_reading",
  );
  await assertRejectsCode(
    () =>
      validateTelemetry(
        { ...reading, sampled_at: "2026-02-30T12:00:00Z" },
        now,
      ),
    "invalid_reading",
  );
});

Deno.test("user JWT, history range, and RSSI mapping are bounded", async () => {
  const token = "header.payload.signature";
  assert(
    bearerAccessToken(new Headers({ Authorization: `Bearer ${token}` })) ===
      token,
  );
  await assertRejectsCode(
    () =>
      Promise.resolve(
        bearerAccessToken(new Headers({ Authorization: "Bearer shared-key" })),
      ),
    "unauthorized",
  );
  assert(parseHistoryHours(null) === 24);
  assert(parseHistoryHours("168") === 168);
  await assertRejectsCode(() => parseHistoryHours("1000"), "invalid_range");
  assert(signalFromRssi(-50) === 100);
  assert(signalFromRssi(-75) === 50);
  assert(signalFromRssi(-100) === 0);
});

Deno.test("JSON size limit and dashboard response contract stay stable", async () => {
  const decoded = decodeJsonBody(new TextEncoder().encode('{"ok":true}')) as {
    ok: boolean;
  };
  assert(decoded.ok === true);
  await assertRejectsCode(
    () => decodeJsonBody(new Uint8Array(2049)),
    "body_too_large",
  );

  const reading = toDashboardReading({
    id: 1,
    device_id: "garden-01",
    boot_id: "018f47b8-68d8-7c8e-9c33-1b5d62f58b61",
    sequence: 42,
    schema_version: 1,
    sampled_at: "2026-08-09T18:25:43.000Z",
    received_at: "2026-08-09T18:25:44.000Z",
    moisture_pct: 42.6,
    raw_adc: 2184,
    sensor_mv: 1760,
    espnow_rssi_dbm: -72,
    battery_mv: 4050,
    battery_percent: 82,
    current_ma: 96,
    power_mw: 389,
    power_measured: true,
    sampling_mode: "live",
    sensor_firmware_build: 196609,
    uptime_seconds: 600,
    sensor_firmware: "2.0.0",
    gateway_firmware: "2.0.0",
    payload_sha256: "0".repeat(64),
  });
  assert(reading.deviceId === "garden-01");
  assert(reading.moisture === 42.6);
  assert(reading.millivolts === 1760);
  assert(reading.batteryVoltage === 4.05);
  assert(reading.powerMilliwatts === 389);
  assert(reading.powerMeasured === true);
});

Deno.test("history buckets keep every supported range bounded", () => {
  // The regression this guards: a seven-day window at 30-second sampling is
  // 20,160 raw readings. The old fixed 2,500-row cap returned the newest ~21
  // hours and said nothing about the missing six days.
  for (const hours of [6, 24, 168]) {
    const bucketSeconds = historyBucketSeconds(hours);
    const points = (hours * 3600) / bucketSeconds;
    assert(points <= 400, `${hours}h yields ${points} points`);
    assert(points >= 100, `${hours}h yields only ${points} points`);
  }

  let rejected = false;
  try {
    historyBucketSeconds(72);
  } catch (error) {
    rejected = error instanceof ApiError && error.code === "invalid_range";
  }
  assert(
    rejected,
    "unsupported ranges must be rejected, not silently bucketed",
  );
});

Deno.test("series points expose aggregate span and survive null columns", () => {
  const point = toDashboardSeriesPoint({
    bucket: "2026-08-09T18:00:00.000Z",
    sample_count: 12,
    moisture_avg: "42.55",
    moisture_min: "40.10",
    moisture_max: "45.00",
    raw_adc_avg: 2184,
    sensor_mv_avg: 1760,
    rssi_avg: -72,
    battery_mv_avg: null,
  });
  assert(point.moisture === 42.55);
  assert(point.moistureMin === 40.1);
  assert(point.moistureMax === 45);
  assert(point.sampleCount === 12);
  assert(point.batteryVoltage === null);
  assert(point.signal === signalFromRssi(-72));

  // A bucket with no RSSI must not be reported as a perfect link.
  const missing = toDashboardSeriesPoint({
    bucket: "2026-08-09T19:00:00.000Z",
    sample_count: 1,
    moisture_avg: 10,
    moisture_min: null,
    moisture_max: null,
    raw_adc_avg: null,
    sensor_mv_avg: null,
    rssi_avg: null,
    battery_mv_avg: null,
  });
  assert(missing.rssi === -127);
  assert(missing.signal === 0);
});

Deno.test("rate limiter caps a flood without blocking normal polling", () => {
  const limiter = new RateLimiter(3, 60_000);
  const start = 1_000_000;
  assert(limiter.check("1.2.3.4", start).allowed);
  assert(limiter.check("1.2.3.4", start + 10).allowed);
  assert(limiter.check("1.2.3.4", start + 20).allowed);

  const blocked = limiter.check("1.2.3.4", start + 30);
  assert(!blocked.allowed, "fourth request in the window must be rejected");
  assert(blocked.retryAfterSeconds >= 1 && blocked.retryAfterSeconds <= 60);

  // A different caller has its own budget.
  assert(limiter.check("5.6.7.8", start + 30).allowed);
  // The window rolls over.
  assert(limiter.check("1.2.3.4", start + 60_001).allowed);
});

Deno.test("client key rejects spoofable forwarded prefixes and is bounded", () => {
  const forwarded = new Headers({ "x-forwarded-for": "203.0.113.7, 10.0.0.1" });
  assert(clientKey(forwarded) === "10.0.0.1");

  const direct = new Headers({ "cf-connecting-ip": "198.51.100.9" });
  assert(clientKey(direct) === "198.51.100.9");

  const cloudflareWins = new Headers({
    "cf-connecting-ip": "198.51.100.10",
    "x-forwarded-for": "203.0.113.8, 10.0.0.2",
  });
  assert(clientKey(cloudflareWins) === "198.51.100.10");

  assert(clientKey(new Headers()) === "unknown");

  const oversized = new Headers({ "x-forwarded-for": "a".repeat(200) });
  assert(clientKey(oversized).length <= 64);
});
