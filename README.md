# LAFVIN wireless soil monitor

This project turns two ESP32-S3 boards into a long-range garden soil monitor:

- A Seeed XIAO ESP32-S3 stays awake, reads a Capacitive Soil Moisture Sensor
  v1.2, and sends a filtered sample every two seconds.
- A LAFVIN ESP32-S3 1.69-inch LCD board receives the sample, acknowledges it,
  keeps the moisture and wireless-strength screen active, and serves a local
  dashboard at `http://soil-monitor.local`.
- When cloud settings are provisioned, the display gateway also sends signed
  HTTPS telemetry to a Supabase Edge Function. A static GitHub Pages dashboard
  then makes the stored history available from outside the home network.

The local LCD and LAN dashboard do not depend on Supabase or GitHub Pages. Do
not expose the gateway's HTTP server with router port forwarding or UPnP.

> This firmware uses the LAFVIN board's LCD and radio for the soil-monitor
> application. Its camera, microphone, and speaker are not exercised by this
> build.

## Architecture

```mermaid
flowchart LR
    S["Garden XIAO ESP32-S3<br/>always-awake probe transmitter"]
    G["Indoor LAFVIN gateway<br/>LCD + local dashboard"]
    E["Supabase Edge Function<br/>HMAC validation"]
    D["Supabase Postgres<br/>RLS-protected telemetry"]
    P["GitHub Pages dashboard<br/>read-only access"]

    S -- "encrypted ESP-NOW LR<br/>reading + application ACK" --> G
    G -- "outbound HTTPS<br/>signed ingest" --> E
    E --> D
    P -- "HTTPS current/history reads" --> E
```

The radio link is encrypted ESP-NOW unicast with a 16-byte PMK and LMK, exact
peer-MAC allowlists, protocol/length validation, and sequence tracking. The
gateway still sends an application acknowledgement for accepted readings, but
the simplified sensor does not wait for commands in it. Both radios
request ESP32 Long Range mode and 20 dBm transmit power. The sensor first tries
the last successful channel, then scans 2.4 GHz channels 1 through 11 until it
gets a successful ESP-NOW delivery result. The gateway follows the channel used
by the home 2.4 GHz Wi-Fi network.

Long Range mode and direct sight improve the link budget, but 200 feet is a
deployment target rather than a guaranteed distance. Test at the actual site.
Keep the 2.4 GHz antenna firmly connected, above the soil, away from metal and
standing water, and in the same orientation at both ends where possible.

## Hardware and exact sensor wiring

Current sensor-board wiring:

| Capacitive Soil Moisture Sensor v1.2 | Seeed XIAO ESP32-S3 |
|---|---|
| `VCC` | `3V3` |
| `GND` | `GND` |
| `AOUT` | `D10`, which is `GPIO9` on this board |

Use the probe's analog `AOUT` pin, not a digital threshold output. This project
builds the sensor with `SENSOR_PIN=9`. Do not feed a 5 V analog signal into the
ESP32-S3 ADC. Powering this probe from 3.3 V keeps its output in the appropriate
range.

The display pin map is board-specific and lives in
[`include/board_pins.h`](include/board_pins.h). Do not use the display build on
a different LCD board until its controller and pin map have been verified.

## Power and deployment

The transmitter never enters deep sleep: its CPU, radio, and probe remain on.
`MEASUREMENT_INTERVAL_SECONDS=2` in [`platformio.ini`](platformio.ini) sets the
startup cadence. The local dashboard toggle switches between instant readings
every two seconds and five-minute readings; the gateway saves that choice.

This is useful for bench diagnosis and reliable USB serial access, but it uses
substantially more power than sleeping firmware. The current build also reports
battery voltage and current/power as unavailable because no measurement circuit
is fitted. Size any battery, regulator, charger, and solar panel for continuous
operation rather than for a sleeping load.

Use a solar charger/power-path board intended for the panel and battery
chemistry, a protected cell, and a regulated output compatible with the XIAO.
Never connect a raw solar panel directly to a Li-ion/LiPo cell or directly to an
ESP32 supply pin. Follow the charger, cell, and board manufacturers' voltage,
temperature, and enclosure requirements. Add strain relief and weather
protection while keeping the capacitive sensing area in the soil and the
electronics, connectors, battery, and charger dry. Do not seal a charging cell
in an enclosure that cannot safely manage heat or pressure.

## Prepare private radio settings

The build intentionally fails if the private radio header is absent.

1. Copy `include/radio_secrets.example.h` to
   `include/radio_secrets.h`.
2. Replace the example PMK and LMK with two different, cryptographically random
   16-byte values. Replace the setup access-point password with a strong random
   WPA2 password of 8 to 63 characters, then set
   `RADIO_SECRETS_CONFIGURED` to `1`.
3. Flash both boards from the same private header so their PMK and LMK match.

Never deploy the example values. `include/radio_secrets.h` is ignored by Git and
must remain uncommitted. The peer MAC addresses are compiled into
[`src/display_gateway.cpp`](src/display_gateway.cpp) and
[`src/sensor_node.cpp`](src/sensor_node.cpp); if either ESP32 board is replaced,
update both `SENSOR_MAC` and `GATEWAY_MAC`, then reflash both boards.

## Build and flash with VS Code and PlatformIO

Install VS Code and the recommended **PlatformIO IDE** extension, then open this
project directory. Connect both boards with data-capable USB cables.

Serial ports are auto-detected. They were previously pinned to one laptop's
`/dev/cu.usbmodem…` paths, which broke on any other machine and whenever a cable
moved. If auto-detection picks the wrong board because both are connected, pass
the port explicitly: `pio run -e display_receiver -t upload --upload-port
/dev/cu.usbmodemXXXX`. Use `pio device list` to see the candidates.

In VS Code, choose **Terminal > Run Task** and run:

1. `Upload LAFVIN Display Receiver`
2. `Upload Soil Sensor Transmitter`

The equivalent PlatformIO Core commands are:

```sh
pio run -e display_receiver
pio run -e sensor_transmitter
pio run -e display_receiver -t upload
pio run -e sensor_transmitter -t upload
```

Serial monitoring runs at 115200 baud. The sensor prints its MAC address, radio
startup result, raw ADC value, converted voltage, channel, and delivery result
for every reading. Because it stays awake, its USB serial port remains present.

## Provision the display gateway

### Secured setup access point

1. Hold the LAFVIN board's **BOOT** button while resetting or powering it, then
   release BOOT. Setup also starts automatically when no saved home network is
   available.
2. Join `SoilMonitor-Setup` with the private WPA2 password stored in your local
   `include/radio_secrets.h`.
3. Open `http://192.168.4.1` within ten minutes.
4. Enter the home network's **2.4 GHz** SSID and password. An ESP32-S3 cannot
   join a 5 GHz-only network.
5. If Supabase has already been deployed, optionally enter the complete HTTPS
   ingest URL, device ID, and ingest secret. All three cloud fields must be
   supplied together; otherwise leave all three blank.
6. Save, let the board restart, rejoin the home network, and open
   `http://soil-monitor.local`.

The setup portal closes after ten minutes. Wi-Fi and cloud changes are rejected
from normal LAN mode; hold BOOT during reset to reopen setup. Settings are kept
in the ESP32's NVS flash. The private ingest secret is never sent to GitHub
Pages and is not printed to serial output.

### Optional USB/bootstrap header

If joining the setup access point is impractical, copy
[`include/local_provisioning.example.h`](include/local_provisioning.example.h)
to the Git-ignored `include/local_provisioning.h`, fill the local values, and
flash only the display environment. Cloud values are optional, but if any cloud
value is present, these three must all be valid:

- `LOCAL_CLOUD_API_URL`: the full HTTPS endpoint ending in
  `/functions/v1/soil-api/v1/ingest`
- `LOCAL_CLOUD_DEVICE_ID`: 3–64 lowercase letters, digits, `_`, or `-`
- `LOCAL_CLOUD_INGEST_SECRET`: a random value at least 32 characters long

With `LOCAL_PROVISIONING_VERSION` left at `0`, values seed only an empty Wi-Fi
or cloud NVS namespace. Once a namespace has settings, later boots leave it
alone, so setup-portal changes are not silently overwritten. To intentionally
apply a revised local header once, increment the version; the gateway records
that version in each namespace and will not reapply it on later boots. After
successful seeding, blank or remove the private header and reflash the display
to keep credentials out of later firmware artifacts. Never commit or share the
header.

## Local display and dashboard

The LCD continues to show the latest moisture percentage, raw ADC value, link
state, signal percentage, RSSI, and elapsed time since the last sensor update
between sensor updates. Its header also reports the home Wi-Fi and cloud API
links separately: `WIFI OK` confirms the gateway is associated with the router,
while `SERVER OK` confirms a successful cloud upload. `SERVER WAIT` means Wi-Fi
is connected but the cloud uploader has not yet succeeded. The onboard RGB LED
briefly flashes cyan whenever the gateway accepts a new sensor packet. Important
header and footer text stays inside a 20-pixel safe margin for the LCD's rounded
corners. The current live packets become stale after seven seconds without a new
reading.

On the same home LAN, open:

```text
http://soil-monitor.local
```

The **Sampling mode** card has an Instant toggle. Turn it on for a reading every
two seconds or off for a reading every five minutes. Because the sensor radio
stays awake, either change is pushed immediately and survives gateway restarts.
The control endpoint accepts only a same-origin JavaScript request with a
non-simple control header, which prevents a normal cross-site form from changing
the mode.

The local dashboard reports moisture, RSSI, packet reliability, raw ADC,
sensor voltage, live current/power, battery, firmware/protocol versions, age,
trend, and in-memory history. It also provides calibration reset, local-history
clear, refresh, gateway restart, and legacy sensor-update controls. The
simplified sensor does not act on staged wireless updates. History resets when
the display gateway restarts. If `.local` name resolution is unavailable, use
the IP address printed in the display serial log or shown by the router.

## Calibrate the moisture percentage

**Calibration is set from the local dashboard and takes effect immediately. No
reflash is required.** The gateway derives the percentage from each packet's raw
ADC value and stores the endpoints in its own flash.

The shipped defaults are `dry = 2513`, `wet = 1300`. `DRY_RAW` was measured from
this specific probe in air at 3.3 V on 2026-08-09 (stable at 2511-2515 ADC).
**`WET_RAW` is still a provisional placeholder, not a real measurement**, so
until you complete step 2 below the percentage is an educated guess and any
alerting built on it inherits that error.

1. Open <http://soil-monitor.local> and turn on **Instant** readings.
2. Remove the probe from soil, keep it dry and still, and press **Capture air**.
   The page waits for three new readings and uses their median only after they
   are stable.
3. Immerse only the sensing section in water, keep the connector/electronics
   dry, wait for the displayed raw value to settle, and press **Capture water**.
4. When both points are ready, press **Save two-point calibration**. The open-air
   value must exceed the water value by at least 200 ADC counts.

The display, local dashboard, new local history samples, and future cloud
uploads all switch to the new scale. Local history is cleared when the scale
changes so old and new percentages are not mixed.

Do not immerse the connector or electronics above the probe's safe sensing
area. The calculation maps the dry endpoint to 0% and wet endpoint to 100%, then
clamps out-of-range readings. If the display is stuck at 100%, inspect the raw
value first: a value at or below the configured wet endpoint will correctly
clamp to 100%, which usually means the endpoints need calibration or `AOUT` is
miswired. Confirm `AOUT -> D10/GPIO9`, common ground, and 3.3 V power before
changing code.

## Unattended operation

The gateway is designed to run for months without anyone looking at it.

**Self-supervision.** A 60-second task watchdog covers the main loop and the
cloud uploader, so a wedged TLS or Wi-Fi stack reboots instead of quietly
freezing the display on a stale reading. Separately, six hours with no sensor
packet triggers a restart on the assumption that the receiver, not the sensor,
is the broken half.

**Upload buffering.** Readings are spooled to a CRC-checked LittleFS file
before upload, so an internet outage or a power cut does not punch a hole in the
history. The buffer holds roughly two hours at two-second sampling; past that
the oldest reading is dropped. The dashboard's **Cloud upload** card shows the
current depth and any drops. If the flash filesystem cannot be mounted, the
gateway falls back to a small RAM queue and says so in the serial log.

**Wireless updates.** The display gateway uses password-protected Arduino OTA.
Set a 12+ character password during Wi-Fi setup, then run
`pio run -e display_receiver -t upload --upload-port soil-monitor.local`.

The simplified garden-sensor firmware deliberately omits wireless self-update
logic. Build and upload the `sensor_transmitter` environment over USB when a
future sensor update is wanted; never upload the display image to the sensor.
The gateway's legacy sensor-update form may still stage a file, but this sensor
will ignore that command.

**Database growth.** `roll_up_telemetry()` aggregates raw readings older than
seven days into hourly buckets and prunes them, keeping the newest row per
device so `device_state` stays valid. It is scheduled hourly by pg_cron in the
retention migration. Without it, two-second sampling produces about 15.8 million
rows a year.

**Alerts.** `evaluate_alerts()` latches dry-soil, sensor-offline, and low-battery
conditions in the database and returns only transitions, so the `alert-monitor`
function delivers each raise and clear exactly once no matter how often it is
polled. Schedule it every 15 minutes and configure at least one delivery
channel in the function's environment, or alerts are latched but never sent:

```sql
select cron.schedule(
  'soil-alert-evaluate', '*/15 * * * *',
  $$ select net.http_post(
       url := 'https://YOUR-PROJECT.supabase.co/functions/v1/alert-monitor',
       headers := '{"Authorization":"Bearer YOUR_SOIL_CRON_SECRET"}'::jsonb
     ); $$
);
```

**Trust boundary on the LAN.** `soil-monitor.local` serves the local dashboard
and its `/api/*` routes over plain HTTP with no authentication. State-changing
routes require a custom header and a matching `Origin`, which blocks drive-by
requests from a malicious website, but anyone already on your Wi-Fi can read
readings and change the sampling mode. Treat the local dashboard as trusted-LAN
only; the Supabase API and the public dashboard are the authenticated path.

## Deploy the Supabase backend

GitHub Pages is static hosting; it cannot receive ESP32 uploads or store sensor
history. The included Supabase Edge Function is the public API boundary. It
validates strict telemetry ranges, HMAC-SHA256 signatures, request time, device
identity, and `(device, boot, sequence)` uniqueness. Postgres RLS is forced and
the public roles have no direct table access.

Detailed API and security instructions are in
[`supabase/README.md`](supabase/README.md). The deployment outline is:

1. Create a Supabase project and install/authenticate the Supabase CLI.
2. From the project root, link the project and apply the migration:

   ```sh
   supabase link --project-ref YOUR_PROJECT_REF
   supabase db push
   ```

3. In Supabase Authentication, create and confirm the owner's email user. Insert
   one `devices` row whose `device_id` exactly matches the gateway setting and
   set `owner_id` to that Auth user's UUID. Reads are denied when `owner_id` is
   empty or belongs to another user.
4. Copy `supabase/.env.example` to the ignored `supabase/.env.local`. Generate
   a random value for `SOIL_INGEST_SECRET`; do not reuse the ESP-NOW keys,
   Wi-Fi password, database password, or service-role key. Set
   `SOIL_ALLOWED_ORIGIN` to the exact Pages origin, for example
   `https://YOUR-GITHUB-USER.github.io` with no repository path or trailing
   slash.
5. Upload the function secrets and deploy:

   ```sh
   supabase secrets set --env-file supabase/.env.local
   supabase functions deploy soil-api --no-verify-jwt
   ```

   Supabase's gateway JWT check is intentionally disabled because one function
   accepts both HMAC-authenticated gateway writes and user-JWT dashboard reads.
   The handler verifies each credential and device ownership itself. Database
   RLS remains enforced.
6. Provision the display with this full ingest URL:

   ```text
   https://PROJECT_REF.supabase.co/functions/v1/soil-api/v1/ingest
   ```

The gateway validates the server certificate, signs each exact JSON body, and
uploads on a separate task so the LCD and ESP-NOW receiver remain responsive.
Network failures, HTTP 408/429, and server failures use bounded exponential
retry; permanent client errors are dropped so one bad record cannot block every
newer sample. A small in-RAM queue absorbs shorter outages. It is not durable
across a gateway reboot and is not a substitute for offline storage.

## Publish the GitHub Pages dashboard

The public dashboard is in [`dashboard/`](dashboard/). Set its public Supabase
project values in [`dashboard/config.js`](dashboard/config.js):

```js
supabaseUrl: "https://PROJECT_REF.supabase.co",
publishableKey: "sb_publishable_...",
apiBaseUrl: "https://PROJECT_REF.supabase.co/functions/v1/soil-api"
```

The publishable key is safe in a public client; never put a secret/service-role
key or ingest secret there. Publish `dashboard/` with GitHub's official static
Pages workflow and set the uploaded artifact path to `./dashboard`.

Before enrollment, enable Supabase Auth passkeys with RP ID
`parxmedia.github.io` and RP origin `https://parxmedia.github.io`, and add
`https://parxmedia.github.io/soil-monitor/` as an allowed redirect URL. Open the
Pages site, expand **First-time setup or recovery**, request the owner email
link, then choose **Passkeys → Add a passkey**. Later visits use Face ID or Touch
ID and do not require an access key.

When the Mac and iPhone use the same Apple Account with Passwords & Keychain
sync enabled, the passkey normally appears on both. If it does not, use the
owner email link once on the second device and add another passkey there. Keep
the email account recoverable and preferably enroll a second passkey before
depending on passwordless access exclusively.

More dashboard details and the JSON contract are in
[`dashboard/README.md`](dashboard/README.md).

## Tests and validation

Run these checks before publishing or deploying hardware:

```sh
# Firmware: warning-enabled release builds for both boards
pio run -e display_receiver
pio run -e sensor_transmitter

# Static dashboard structure and security checks
python3 -m unittest discover -s dashboard/tests -v

# Edge Function formatting, lint, and helper/security tests
deno fmt --check supabase/functions supabase/tests
deno lint supabase/functions supabase/tests
deno test supabase/tests
```

To preview the Pages dashboard with synthetic data:

```sh
python3 dashboard/tests/mock_server.py --port 8080
```

Open `http://127.0.0.1:8080`. The mock server supplies a synthetic passkey
session, binds locally, and does not contact the garden device or Supabase.

For an end-to-end hardware check:

1. Test both boards close together and confirm the gateway log reports an
   ESP-NOW receiver ready on a 2.4 GHz channel.
2. Confirm the next packet is acknowledged, the LCD shows a believable raw
   reading and RSSI, and the packet counter advances about every two seconds.
3. Confirm `http://soil-monitor.local` works from another device on the home
   LAN while the LCD continues updating.
4. With cloud settings installed, confirm the gateway status changes to cloud
   `ONLINE` with HTTP `200` or `201`, then verify current and history responses
   in the Pages dashboard.
5. Only after the bench test passes, repeat at increasing distance in direct
   sight and monitor RSSI and missed-packet reliability.

## Troubleshooting

| Symptom | Checks |
|---|---|
| LCD is blank but a blue LED is on | An LED proves power, not that the correct firmware is running. Verify the `display_receiver` environment, data-capable USB cable, selected port, and serial output. Check `include/board_pins.h` only against this exact LAFVIN board revision. |
| Moisture always reads 100% | Check the live raw ADC value. Verify `AOUT -> D10/GPIO9`, `VCC -> 3V3`, common `GND`, and that a digital output is not connected. Recalibrate `DRY_RAW`/`WET_RAW`; do not hide a wiring fault by changing only the percentage. |
| LCD says `WAITING` or `LOST` | Allow several seconds at the current two-second cadence. Confirm matching PMK/LMK, correct peer MAC constants, attached antenna, and an unobstructed 2.4 GHz path. |
| Sensor USB port vanishes | The new firmware does not intentionally sleep. Check power, the data cable, USB port, and whether the board reset or crashed. Hold BOOT while tapping RESET only if you need the ROM bootloader. |
| `SoilMonitor-Setup` is missing | Hold BOOT during display reset, release it after boot, use the private WPA2 password, and connect within the ten-minute setup window. Power-cycle and retry if the window expired. |
| `soil-monitor.local` does not open | Confirm the laptop and gateway are on the same non-isolated LAN. Try the IP address from serial or the router because some networks block mDNS between VLANs or clients. Never solve this with internet port forwarding. |
| Sensor cannot find the gateway after a router change | The gateway must use 2.4 GHz. The sensor scans channels 1–11 on its next reading. Configure the router to a channel in that range and avoid client isolation. |
| Cloud stays `WAITING` | Verify internet and NTP access, an HTTPS ingest URL, matching device ID and ingest secret, a deployed `soil-api` function, and a provisioned `devices` row. Inspect Supabase Function logs without printing secrets. Clock-skew rejection is expected when gateway time is not synchronized. |
| Pages says configuration or authorization failed | Confirm `supabaseUrl` and `apiBaseUrl` use the same project, `publishableKey` is the public `sb_publishable_...` value, `SOIL_ALLOWED_ORIGIN` is the exact Pages origin, the passkey RP ID/origin are correct, and the signed-in Auth user matches `devices.owner_id`. Never use an ingest, secret, or service-role key in the page. |
| Battery is unavailable | This is the expected current state. Add a safe external divider and implement ADC-to-battery conversion before expecting voltage or percentage fields. |

## Security checklist

- Keep `include/radio_secrets.h`, `include/local_provisioning.h`,
  `supabase/.env.local`, and all credentials out of Git.
- Use different random values for ESP-NOW PMK, ESP-NOW LMK, setup WPA2,
  cloud ingest HMAC, and database credentials.
- Keep the display behind the home router; no inbound port forwarding is
  required for Supabase uploads or GitHub Pages reads.
- Never put a service-role key, ingest secret, Wi-Fi credential, or database
  password in the static dashboard.
- Restrict CORS to the exact Pages origin, but do not treat CORS as
  authentication.
- Revoke lost passkeys in Supabase Auth. Rotate radio and cloud credentials
  after a device or firmware image is lost or exposed, and reflash both
  ESP-NOW peers after radio-key rotation.
- Enable 2FA, secret scanning, protected branches, and dependency/update alerts
  on the hosting accounts and repository.
