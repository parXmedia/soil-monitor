# Supabase soil telemetry backend

This directory contains a database migration and a dependency-free Supabase Edge Function. The
function is the only public data boundary: database RLS is enabled and forced, direct `anon` and
`authenticated` table access is revoked, and no permissive policies are installed.

## API routes

After deployment, the base URL is:

```text
https://PROJECT_REF.supabase.co/functions/v1/soil-api
```

- `POST /v1/ingest` authenticates the indoor gateway with HMAC-SHA256.
- `GET /v1/current` returns the newest reading.
- `GET /v1/history?hours=6|24|168` returns at most 2,500 chronological readings.

Both GET routes require a short-lived Supabase Auth user JWT in the `Authorization` header. The
function validates the JWT with Supabase Auth and requires its user ID to match the device's
`owner_id`. Normal dashboard sign-in uses a passkey; there is no shared read token. Never place the
service-role key or ingest secret in browser code.

## HMAC ingest contract

The gateway sends these headers:

```text
Content-Type: application/json
X-Device-Id: garden-sensor-1
X-Boot-Id: <UUID generated on boot>
X-Sequence: <unsigned 32-bit integer>
X-Timestamp: <current Unix time in seconds>
X-Signature: <unpadded base64url HMAC-SHA256>
```

Hash the exact UTF-8 request bytes, then sign this canonical string (there is no trailing newline):

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
  "current_ma": 96,
  "power_mw": 389,
  "power_measured": true,
  "sampling_mode": "live",
  "sensor_firmware_build": 196609,
  "uptime_seconds": 600,
  "sensor_firmware": "3.0.1",
  "gateway_firmware": "3.0.0"
}
```

Power fields are nullable outside Live mode. `power_measured: false` identifies the configured
estimate used when no current-sense circuit is fitted.

The request timestamp must be within the configured 30–900 second window. A sample can be up to 30
days old, allowing an offline gateway queue to backfill after reconnecting.
`(device_id, boot_id, sequence)` is unique. Repeating the same signed payload is idempotent; reusing
the sequence for different data gets `409 sequence_conflict`.

## Provision and deploy

Prerequisites: a Supabase project, Supabase CLI, and a GitHub Pages origin. Do not paste credentials
into committed commands or files. Run the commands below from the `lafvin_wireless_soil_monitor`
project root (the directory containing this `supabase` directory).

1. Link the local directory and apply the migration:

   ```sh
   supabase link --project-ref YOUR_PROJECT_REF
   supabase db push
   ```

   No firmware reflash is required for this migration. The ingest endpoint accepts the payload
   produced by the already-flashed gateway; newer optional power, mode, and firmware diagnostics
   remain nullable for older boards.

2. In Authentication → Users, create the owner's email account and mark the email confirmed. In the
   SQL editor, provision the logical device and assign it to that Auth user:

   ```sql
   insert into public.devices (device_id, display_name, owner_id)
   select 'garden-sensor-1', 'Garden soil sensor', id
   from auth.users
   where lower(email) = lower('YOU@EXAMPLE.COM');
   ```

   For an existing device, use:

   ```sql
   update public.devices
   set owner_id = (
     select id from auth.users
     where lower(email) = lower('YOU@EXAMPLE.COM')
   )
   where device_id = 'garden-sensor-1';
   ```

3. Generate a 32-byte random ingest value. Store it only on the indoor gateway and in
   `SOIL_INGEST_SECRET`. Set function secrets interactively or from an ignored local environment
   file:

   ```sh
   openssl rand -base64 32
   supabase secrets set --env-file supabase/.env.local
   ```

   `SOIL_ALLOWED_ORIGIN` must be the exact HTTPS origin, such as `https://example.github.io`, with
   no path or trailing slash. The required names are documented in `.env.example`. Supabase supplies
   `SUPABASE_URL` and `SUPABASE_SERVICE_ROLE_KEY` to the function automatically.

4. Deploy the function:

   ```sh
   supabase functions deploy soil-api --no-verify-jwt
   ```

5. Configure Supabase Auth before publishing the dashboard:

   - Authentication → URL Configuration:
     - Site URL: `https://parxmedia.github.io/soil-monitor/`
     - Redirect URL: `https://parxmedia.github.io/soil-monitor/`
   - Authentication → Passkeys:
     - Enable passkeys.
     - RP display name: `Garden Soil Monitor`
     - RP ID: `parxmedia.github.io`
     - RP origins: `https://parxmedia.github.io`

   The RP ID has no scheme or path. Do not change it after enrollment because existing passkeys are
   cryptographically bound to it. Supabase currently labels this feature experimental.

6. Copy the project's public `sb_publishable_...` key from Settings → API into
   `dashboard/config.js`, along with the project and function URLs. A publishable key is safe in
   client code; a secret or service-role key is not.

7. Open the published dashboard on the Mac, request the confirmed owner's email link under
   **First-time setup or recovery**, then open **Passkeys** and choose **Add a passkey**. With
   Passwords & Keychain sync enabled on the same Apple Account, that passkey should be available on
   the iPhone as well. If it is not, repeat the email-link enrollment once on the iPhone to add a
   second passkey.

## Local validation

Run helper unit tests with Deno 2:

```sh
deno fmt --check supabase/functions supabase/tests
deno lint supabase/functions supabase/tests
deno test supabase/tests
```

If Deno is not installed, Node 22 or newer can run the same dependency-free helper tests:

```sh
node --experimental-strip-types supabase/tests/node_runner.mjs
```

For a full local integration test, start Supabase, apply the migration, create a test Auth
user/device row, populate an ignored `.env.local`, and serve the Edge Function with
`supabase functions serve soil-api --env-file supabase/.env.local`.

## Security operations

- Keep the gateway behind the home router; do not add port forwarding or UPnP.
- Keep service-role and HMAC credentials out of Git, Pages, logs, screenshots, firmware source, and
  serial output. Provision the gateway over USB/NVS.
- Use a unique HMAC secret per deployed backend and rotate it after suspected exposure. Increment
  `ingest_key_version` when rotating.
- Set an exact CORS origin. CORS is browser isolation, not authentication.
- Enable GitHub secret scanning, Dependabot, branch protection, and 2FA.
- Configure platform rate limits/alerts and review Edge Function/database logs.
- Back up or export telemetry if it matters; free projects are not a backup.
- The user-provided home Wi-Fi password is not needed by this backend and must never be copied into
  Supabase, GitHub, code, or documentation.
