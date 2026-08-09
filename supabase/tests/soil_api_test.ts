import {
  ApiError,
  authenticateIngest,
  authorizeRead,
  canonicalIngestMessage,
  decodeJsonBody,
  hmacSignatureBase64Url,
  parseHistoryHours,
  sha256Hex,
  signalFromRssi,
  toDashboardReading,
  validateTelemetry,
} from "../functions/soil-api/soil_api.ts";

function assert(condition: unknown, message = "assertion failed"): asserts condition {
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
  const result = await authenticateIngest(headers, body, "garden-01", secret, now, 300);
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
    () => authenticateIngest(
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
    uptime_seconds: 600,
    sensor_firmware: "2.0.0",
    gateway_firmware: "2.0.0",
  }, now);
  assert(reading.sampled_at === "2026-08-09T18:25:43.000Z");
  assert(reading.moisture_pct === 42.6);
  assert(reading.battery_mv === 4050);

  await assertRejectsCode(
    () => validateTelemetry({ ...reading, moisture_pct: 101 }, now),
    "invalid_reading",
  );
  await assertRejectsCode(
    () => validateTelemetry({ ...reading, unexpected: true }, now),
    "invalid_reading",
  );
  await assertRejectsCode(
    () => validateTelemetry({ ...reading, sampled_at: "2026-02-30T12:00:00Z" }, now),
    "invalid_reading",
  );
});

Deno.test("read token, history range, and RSSI mapping are bounded", async () => {
  const token = "read-token-test-only-0123456789-abcdefghijklmnopqrstuvwxyz";
  await authorizeRead(new Headers({ Authorization: `Bearer ${token}` }), token);
  await assertRejectsCode(
    () => authorizeRead(
      new Headers({ Authorization: "Bearer wrong-token-that-is-still-long-enough-123456" }),
      token,
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
  const decoded = decodeJsonBody(new TextEncoder().encode('{"ok":true}')) as { ok: boolean };
  assert(decoded.ok === true);
  await assertRejectsCode(() => decodeJsonBody(new Uint8Array(2049)), "body_too_large");

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
    uptime_seconds: 600,
    sensor_firmware: "2.0.0",
    gateway_firmware: "2.0.0",
    payload_sha256: "0".repeat(64),
  });
  assert(reading.deviceId === "garden-01");
  assert(reading.moisture === 42.6);
  assert(reading.millivolts === 1760);
  assert(reading.batteryVoltage === 4.05);
});
