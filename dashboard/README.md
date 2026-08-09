# Public dashboard

This directory is a dependency-free static dashboard intended for GitHub Pages.
It reads data from a separate HTTPS cloud API; GitHub Pages cannot receive data
directly from an ESP32 or store time-series readings.

## Configure

Edit `config.js` and set `apiBaseUrl` to the deployed **read-only** cloud API:

```js
apiBaseUrl: "https://soil-api.example.workers.dev"
```

Never place Wi-Fi credentials, API write keys, database passwords, or private
tokens in this directory. GitHub Pages repositories and delivered JavaScript are
public. The API should restrict browser CORS responses to the exact Pages origin,
rate-limit requests, validate all gateway writes, and return only sensor fields.

The dashboard asks the viewer for a revocable **read-only bearer token**. It is
kept in `sessionStorage`, sent only in the HTTPS `Authorization` header, and
discarded when the tab closes or **Forget key** is selected. The read token must
not grant writes, configuration changes, or direct database access. The API must
allow the `Authorization` request header in its CORS preflight response.

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
  "sequence": 12345
}
```

Only `timestamp` and `moisture` are required. Battery fields are optional until
battery-voltage sensing is added to the garden board.

The garden transmitter is expected to send one sample every five minutes. The
dashboard polls the cloud API every 2 seconds for low-latency delivery. A
reading is labeled fresh through the five-minute sampling interval plus one
minute of radio/upload grace; after six minutes it is explicitly stale.

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
computer and returns synthetic readings. Enter `demo-read-token` on the lock
screen. Local HTTP APIs are accepted only when the dashboard itself is running
on localhost; deployed dashboards require HTTPS.

## Publish with GitHub Pages

Publish this directory from a GitHub Actions workflow or copy it into the
repository's selected Pages source directory. Keep the Content Security Policy
in `index.html`; the site loads no third-party scripts, fonts, or analytics.
