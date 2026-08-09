# Supabase soil telemetry backend

This directory contains a database migration and a dependency-free Supabase
Edge Function. The function is the only public data boundary: database RLS is
enabled and forced, direct `anon` and `authenticated` table access is revoked,
and no permissive policies are installed.

## API routes

After deployment, the base URL is:

```text
https://PROJECT_REF.supabase.co/functions/v1/soil-api
```

- `POST /v1/ingest` authenticates the indoor gateway with HMAC-SHA256.
- `GET /v1/current` returns the newest reading.
- `GET /v1/history?hours=6|24|168` returns at most 2,500 chronological readings.

Both GET routes require `Authorization: Bearer <SOIL_READ_TOKEN>`. This protects
the endpoint from casual public access, but a token embedded in a static GitHub
Pages application is visible to every visitor. For genuinely private readings,
replace this shared read token with Supabase Auth and owner-scoped RLS before
publishing. Never place the service-role key or ingest secret in browser code.

## HMAC ingest contract

The gateway sends these headers:

```text
Content-Type: application/json
X-Device-Id: garden-01
X-Boot-Id: <UUID generated on boot>
X-Sequence: <unsigned 32-bit integer>
X-Timestamp: <current Unix time in seconds>
X-Signature: <unpadded base64url HMAC-SHA256>
```

Hash the exact UTF-8 request bytes, then sign this canonical string (there is no
trailing newline):

```text
v1
device-id
boot-id
sequence
timestamp
lowercase-hex-sha256-of-exact-body
```

Example JSON body:

```json
{
  "schema": 1,
  "sampled_at": "2026-08-09T18:25:43.000Z",
  "moisture_pct": 42.6,
  "raw_adc": 2184,
  "sensor_mv": 1760,
  "espnow_rssi_dbm": -72,
  "battery_mv": 4050,
  "battery_percent": 82,
  "uptime_seconds": 600,
  "sensor_firmware": "2.0.0",
  "gateway_firmware": "2.0.0"
}
```

The request timestamp must be within the configured 30–900 second window. A
sample can be up to 30 days old, allowing an offline gateway queue to backfill
after reconnecting. `(device_id, boot_id, sequence)` is unique. Repeating the
same signed payload is idempotent; reusing the sequence for different data gets
`409 sequence_conflict`.

## Provision and deploy

Prerequisites: a Supabase project, Supabase CLI, and a GitHub Pages origin. Do
not paste credentials into committed commands or files. Run the commands below
from the `lafvin_wireless_soil_monitor` project root (the directory containing
this `supabase` directory).

1. Link the local directory and apply the migration:

   ```sh
   supabase link --project-ref YOUR_PROJECT_REF
   supabase db push
   ```

2. In the SQL editor, provision the logical device. `owner_id` is optional for
   the shared read-token dashboard; populate it before adding owner-scoped Auth
   policies in a future multi-user deployment:

   ```sql
   insert into public.devices (device_id, display_name)
   values ('garden-01', 'Garden soil sensor');
   ```

3. Generate two different 32-byte random values locally. Store the ingest value
   only on the indoor gateway and in `SOIL_INGEST_SECRET`. Store the read value
   in `SOIL_READ_TOKEN`. Set function secrets interactively or from an ignored
   local environment file:

   ```sh
   openssl rand -base64 32
   openssl rand -base64 32
   supabase secrets set --env-file supabase/.env.local
   ```

   `SOIL_ALLOWED_ORIGIN` must be the exact HTTPS origin, such as
   `https://example.github.io`, with no path or trailing slash. The required
   names are documented in `.env.example`. Supabase supplies `SUPABASE_URL` and
   `SUPABASE_SERVICE_ROLE_KEY` to the function automatically.

4. Deploy the function:

   ```sh
   supabase functions deploy soil-api --no-verify-jwt
   ```

5. Configure the dashboard API base URL with the function URL. If keeping the
   shared read-token design, the dashboard must send it as a Bearer token; treat
   it as a revocable read-only access code, never as a true secret.

## Local validation

Run helper unit tests with Deno 2:

```sh
deno fmt --check supabase/functions supabase/tests
deno lint supabase/functions supabase/tests
deno test supabase/tests
```

If Deno is not installed, Node 22 or newer can run the same dependency-free
helper tests:

```sh
node --experimental-strip-types supabase/tests/node_runner.mjs
```

For a full local integration test, start Supabase, apply the migration, create a
test Auth user/device row, populate an ignored `.env.local`, and serve the Edge
Function with `supabase functions serve soil-api --env-file supabase/.env.local`.

## Security operations

- Keep the gateway behind the home router; do not add port forwarding or UPnP.
- Keep service-role and HMAC credentials out of Git, Pages, logs, screenshots,
  firmware source, and serial output. Provision the gateway over USB/NVS.
- Use a unique HMAC secret per deployed backend and rotate it after suspected
  exposure. Increment `ingest_key_version` when rotating.
- Set an exact CORS origin. CORS is browser isolation, not authentication.
- Enable GitHub secret scanning, Dependabot, branch protection, and 2FA.
- Configure platform rate limits/alerts and review Edge Function/database logs.
- Back up or export telemetry if it matters; free projects are not a backup.
- The user-provided home Wi-Fi password is not needed by this backend and must
  never be copied into Supabase, GitHub, code, or documentation.
