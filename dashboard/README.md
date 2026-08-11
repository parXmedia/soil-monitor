# Public dashboard

This directory is a build-free static dashboard intended for GitHub Pages.
It reads data from a separate HTTPS cloud API; GitHub Pages cannot receive data
directly from an ESP32 or store time-series readings.

The code is split by responsibility: `data.js` contains pure validation and
statistics helpers, `auth.js` wraps Supabase Auth passkeys, and `app.js` owns
browser state, requests, and rendering. The pinned Supabase browser client is
self-hosted under `vendor/`; the page executes no third-party CDN scripts.

## Configure

Edit `config.js` and set the Supabase project URL, its public publishable key,
and the deployed read-only function URL:

```js
supabaseUrl: "https://PROJECT_REF.supabase.co",
publishableKey: "sb_publishable_...",
apiBaseUrl: "https://PROJECT_REF.supabase.co/functions/v1/soil-api"
```

Supabase publishable keys are designed for public applications and do not grant
garden access by themselves. Never place Wi-Fi credentials, a Supabase secret
key, the service-role key, the ingest secret, database passwords, or user session
tokens in this directory.

Normal sign-in uses a discoverable Supabase Auth passkey and does not ask for an
email, password, or shared access key. Supabase manages a short-lived user JWT;
the Edge Function verifies it and checks that its user ID equals the configured
device's `owner_id`. The confirmed owner email is retained only as first-time
enrollment and recovery access.

Supabase currently labels passkey support experimental and requires
`@supabase/supabase-js` 2.105.0 or newer. The vendored client is pinned to
2.105.0 so an upstream release cannot silently change the deployed site.

## API contract

`GET {apiBaseUrl}/v1/current`

```json
{
  "deviceId": "garden-01",
  "timestamp": "2026-08-09T18:25:43.000Z",
  "moisture": 42.6,
  "rawAdc": 2184,
  "millivolts": 1760,
  "rssi": -72,
  "signal": 56,
  "batteryVoltage": 4.05,
  "batteryPercent": 82,
  "sequence": 12345,
  "currentMilliamps": 96,
  "powerMilliwatts": 389,
  "powerMeasured": true,
  "samplingMode": "live",
  "sensorFirmwareBuild": 196609,
  "sensorFirmware": "3.0.1",
  "gatewayFirmware": "3.0.0"
}
```

Only `timestamp` and `moisture` are required. Battery and power fields are
optional. `powerMeasured: false` means the sensor used its configured current
estimate because current-sense hardware was not fitted.

The garden transmitter supports instant readings every two seconds and a
five-minute cadence selected from the gateway's local dashboard. The public
page remains read-only. It polls the cloud API every 2 seconds and
conservatively labels a reading stale after six minutes so brief internet
interruptions do not immediately hide the most recent valid cloud reading.

`GET {apiBaseUrl}/v1/history?hours=24`

```json
{
  "readings": [
    {
      "deviceId": "garden-01",
      "timestamp": "2026-08-09T18:20:00.000Z",
      "moisture": 41.8,
      "rssi": -73,
      "batteryPercent": 82
    }
  ]
}
```

Supported dashboard ranges are 6, 24, and 168 hours. The API should cap result
size (for example, one aggregated point every 5 minutes and no more than 2,500
points) to protect both the service and the browser.

## Preview locally

From this directory, run the local-only mock API to preview every live state
without editing `config.js` or sending data anywhere:

```sh
python3 tests/mock_server.py --port 8080
```

Then open `http://127.0.0.1:8080`. The mock server binds only to the current
computer and supplies a synthetic signed-in passkey session and readings. Local
HTTP is accepted only for loopback development; deployed passkeys require HTTPS.

## Publish with GitHub Pages

Publish this directory from a GitHub Actions workflow or copy it into the
repository's selected Pages source directory. Keep the Content Security Policy
in `index.html`; the site loads no third-party scripts, fonts, or analytics.
