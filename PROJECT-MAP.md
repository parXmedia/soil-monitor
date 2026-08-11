# OpenFarms Wireless Soil Monitor — Project Guide

This document explains what the project does, how its parts communicate, where
the important files live, and what remains to be completed before permanent
garden deployment.

Last updated: August 9, 2026.

## 1. Project purpose

OpenFarms uses two ESP32-S3 boards to measure garden soil moisture and make the
result available in three places:

1. The LCD on the indoor LAFVIN board.
2. A local dashboard at `http://soil-monitor.local` while connected to the same
   home network.
3. A secured cloud dashboard hosted with GitHub Pages, backed by Supabase, for
   viewing readings away from home.

The garden board does not join the home Wi-Fi network. It stays awake and sends
short encrypted radio messages to the indoor display board every two seconds.
The display board remains powered, shows the reading, hosts the local dashboard,
and performs outbound cloud uploads.

The LAFVIN board also contains a camera, microphone, and speaker, but this soil
monitor firmware currently uses only its LCD, ESP32-S3 radio, USB serial port,
and Wi-Fi connection.

## 2. System architecture

```text
Garden                                              Home / Internet

Capacitive probe
       │ analog voltage
       ▼
XIAO ESP32-S3  ── encrypted ESP-NOW Long Range ──▶  LAFVIN ESP32-S3
sensor node               reading + ACK              display gateway
                                                          │
                         ┌────────────────────────────────┼──────────────┐
                         ▼                                ▼              ▼
                    LCD screen                 Local web dashboard   HTTPS upload
                                               soil-monitor.local         │
                                                                          ▼
                                                               Supabase API + database
                                                                          │
                                                                          ▼
                                                               GitHub Pages dashboard
```

No router port forwarding or UPnP is needed. The gateway makes an outbound
HTTPS request to Supabase, and the public website reads the stored data through
the Supabase API.

## 3. Hardware roles and wiring

### Garden sensor node

- Board: Seeed Studio XIAO ESP32-S3 with a 2.4 GHz antenna.
- Sensor: Capacitive Soil Moisture Sensor v1.2.
- Intended power: protected battery, solar charger/power-path hardware, and a
  suitable regulated supply.
- Firmware environment: `sensor_transmitter`.

Current wiring:

| Moisture sensor | XIAO ESP32-S3 |
|---|---|
| `GND` | `GND` |
| `VCC` | `3V3` |
| `AOUT` | board pin `D10`, which is `GPIO9` |

The firmware therefore builds with `SENSOR_PIN=9`. The probe must use its analog
output. Do not connect a 5 V analog signal to the ESP32-S3 ADC.

### Indoor display gateway

- Board: LAFVIN ESP32-S3 with 1.69-inch 240 × 280 LCD.
- Power: continuous USB power from the Mac or another reliable USB supply.
- Network: a 2.4 GHz home Wi-Fi network.
- Firmware environment: `display_receiver`.

The display pin assignments are specific to this board and are defined in
`include/board_pins.h`.

## 4. How one measurement works

Each garden-node measurement follows this sequence:

1. Keep the moisture probe powered and wait for it to settle during startup.
2. Take nine ADC and millivolt readings.
3. Use the median values to reduce electrical noise and outliers.
4. Convert the filtered raw ADC value to 0–100% using the dry and wet calibration
   endpoints.
5. Mark obviously invalid ADC values as a likely wiring or sensor problem.
6. Create a versioned 40-byte packet containing moisture, raw ADC, sensor
   voltage, firmware build, sequence, status flags, and fixed fast mode.
7. Send it to the allowlisted gateway with encrypted ESP-NOW unicast.
8. Retry the last working channel first, then scan Wi-Fi channels 1–11 when
   necessary.
9. Print the reading and delivery result over USB serial, remain awake, and
   repeat after the compiled interval.

The gateway validates the sender MAC address, packet size, protocol version,
message type, and value ranges before accepting a reading. It tracks received
and missing sequence numbers, records RSSI, updates the LCD and local dashboard,
acknowledges the packet, and queues a cloud upload when cloud settings exist.

## 5. Sampling and freshness

The sensor stays awake in both supported modes. The local dashboard toggle
selects instant readings every two seconds or five-minute readings. The gateway
persists the selection and pushes it immediately over encrypted ESP-NOW.

The LCD and local website do not label an old value as continuously connected.
They display the elapsed time since the last sensor update and change from fresh
to stale when the expected next reading plus a bounded radio allowance has
passed. The current live packets become stale after seven seconds.

The cloud dashboard polls its API every 2 seconds. Polling more frequently does
not create new sensor samples; it only checks whether a new uploaded sample has
arrived.

## 6. Moisture calibration

The firmware fallback endpoints are defined in `src/sensor_node.cpp`:

```cpp
constexpr int DRY_RAW = 2513;
constexpr int WET_RAW = 1300;
```

`DRY_RAW=2513` was measured from this sensor in air at 3.3 V. The wet endpoint
is still provisional. Use the local dashboard's guided two-point calibration:

1. Turn on Instant readings from the local dashboard.
2. Keep the probe dry and still in open air, then select **Capture air**. The
   page collects three new stable readings and records their median.
3. Immerse only the sensing section in water, keep the electronics dry, wait for
   the live raw value to settle, and select **Capture water**.
4. Save after both points are ready. The gateway stores them in flash; no sensor
   reflash is needed.

The gateway recalculates the LCD, local dashboard, new local history, and future
cloud uploads from raw ADC using the saved endpoints. Local history resets when
the calibration scale changes.

The conversion maps the dry endpoint to 0%, the wet endpoint to 100%, and clamps
values beyond them. A constant 100% reading is not proof that the soil is wet:
first inspect the raw ADC value and verify `AOUT → D10/GPIO9`, common ground, and
3.3 V power.

Do not submerge the probe connector or its electronics. Only place the intended
capacitive sensing section in soil or water during calibration.

## 7. Radio link

The boards use direct ESP-NOW rather than a normal Wi-Fi connection between the
garden and house. The implementation includes:

- ESP32 Long Range radio mode.
- Maximum requested transmit power of 20 dBm.
- Encrypted unicast using separate private PMK and LMK keys.
- Exact peer MAC allowlisting in both directions.
- Application acknowledgements and sequence tracking.
- Channel retry and scan behavior.
- RSSI in dBm and a human-readable signal percentage on the display/dashboard.

Two hundred feet with clear line of sight is a deployment target, not a
guarantee. Test at increasing distance at the real site. Keep the antenna
attached before transmitting, above soil level, away from metal and standing
water, and oriented consistently. Walls, vegetation, antenna quality, router
channel, interference, and enclosures all affect range.

## 8. Display and local dashboard

The LAFVIN LCD shows the latest:

- Moisture percentage and condition.
- Raw ADC value.
- Wireless RSSI and signal percentage.
- Fresh/stale connection state.
- Elapsed time since the last sensor update.

The same-LAN website at `http://soil-monitor.local` adds:

- Average, range, trend, and in-memory history.
- Received and missed packet counts and reliability.
- Raw sensor voltage.
- Gateway address.
- Instant two-second / five-minute sampling toggle.
- Power fields, which are unavailable in the simplified sensor build.
- Legacy sensor firmware staging controls and gateway maintenance controls; the
  simplified sensor ignores staged wireless updates.

Local history is held in RAM and resets when the display gateway restarts. If
the `.local` name does not resolve, use the gateway IP address shown in serial
output or in the router's device list.

## 9. Cloud data path and remote website

GitHub Pages is static hosting and cannot accept ESP32 data directly. The cloud
path therefore works as follows:

1. The gateway converts each accepted sensor packet into JSON.
2. It hashes and signs the exact upload with HMAC-SHA256 using its private ingest
   secret.
3. It sends the request over certificate-validated HTTPS to the Supabase Edge
   Function.
4. The function checks the device identity, signature, timestamp, value ranges,
   and duplicate/replay identifiers before writing to Postgres.
5. The GitHub Pages dashboard calls read-only current/history endpoints.
6. The viewer signs in with a Supabase Auth passkey. The function verifies the
   short-lived user JWT and requires that user to match the device's `owner_id`.

The gateway uploads in a background task so a slow network request does not stop
LCD or radio handling. It uses a small RAM queue and bounded retries for
temporary failures. The queue is not durable across a gateway reboot.

Supabase files are under `supabase/`; the public static site is under
`dashboard/`; its deployment workflow is `.github/workflows/pages.yml`.

## 10. Security design

- Private radio keys, Wi-Fi credentials, cloud secrets, and local environment
  files are ignored by Git.
- The GitHub Pages artifact contains only the `dashboard/` directory.
- The static dashboard contains no write secret or database service-role key.
- The gateway is not exposed through router port forwarding.
- Wi-Fi and cloud provisioning are available only through a time-limited,
  WPA2-protected setup access point.
- Normal LAN requests cannot overwrite the stored network credentials.
- Database Row Level Security is forced, and direct public table access is
  denied.
- Cloud writes are authenticated separately from read-only dashboard access.

Never publish or share these local files:

- `include/radio_secrets.h`
- `include/local_provisioning.h`
- `supabase/.env.local`
- `supabase/.env.functions.local`

Use different random credentials for radio encryption, the setup access point,
cloud ingestion, dashboard reading, and database administration. Because the
home Wi-Fi password was previously entered into a chat, rotating it is
recommended.

## 11. Solar-power limitations

The sensor firmware keeps the ESP32 radio, CPU, and probe awake continuously.
The two-second setting controls transmission cadence, not power state. Size the
battery, regulator, charger, and panel for that continuous load; moving to a
sleeping design would require a separate firmware and hardware power-budget
change.

Battery voltage is also not currently measured. The packet and website support
an unavailable battery value, but real battery telemetry needs a protected,
high-impedance voltage divider connected to an unused ADC pin plus firmware
conversion and calibration.

Use a charger/power-path board designed for the solar panel and battery
chemistry, a protected cell, appropriate regulation, strain relief, and a
weather-resistant enclosure. Never connect a raw panel directly to a lithium
battery or ESP32 power pin.

## 12. Important project files

| Path | Purpose |
|---|---|
| `platformio.ini` | Board targets, USB ports, dependencies, pins, and build options |
| `src/main.cpp` | Validates that exactly one firmware role is selected |
| `src/display_gateway.cpp` | Indoor display, radio receiver, local dashboard, and cloud uploader |
| `src/sensor_node.cpp` | Probe sampling, selectable radio cadence, and serial diagnostics |
| `include/board_pins.h` | LAFVIN LCD controller and pin map |
| `include/soil_protocol.h` | Versioned radio packet and acknowledgement formats |
| `include/web_dashboard.h` | Local dashboard and Wi-Fi/cloud setup pages embedded in firmware |
| `include/radio_secrets.example.h` | Safe template for private ESP-NOW/setup credentials |
| `include/local_provisioning.example.h` | Safe template for optional local gateway provisioning |
| `dashboard/app.js` | Public dashboard browser state, rendering, and charting |
| `dashboard/data.js` | Pure cloud-data validation, normalization, and statistics helpers |
| `supabase/` | Database migration, Edge Function, tests, and deployment instructions |
| `.github/workflows/pages.yml` | Publishes only the public dashboard directory |
| `README.md` | Detailed build, flash, deployment, and troubleshooting instructions |

## 13. Building, testing, and flashing

Open the `openfarms` folder in Visual Studio Code with the PlatformIO IDE
extension installed.

Build both firmware targets:

```sh
pio run -e display_receiver
pio run -e sensor_transmitter
```

Flash the display and sensor respectively:

```sh
pio run -e display_receiver -t upload
pio run -e sensor_transmitter -t upload
```

USB device names can change after reconnecting a board. If an upload target is
missing, use PlatformIO Devices or `pio device list`, then update `upload_port`
and `monitor_port` in `platformio.ini`.

The sensor stays awake, so its USB port should normally remain available. Hold
BOOT while resetting the XIAO only when the normal upload path is unavailable.

Run the public-dashboard checks with:

```sh
python3 -m unittest discover -s dashboard/tests -v
```

The automated checks cover 20 dashboard regressions plus both firmware builds.
That verifies source integrity; it does not replace a live radio, sensor, Wi-Fi,
and cloud test with the physical boards.

## 14. Current completion status

Implemented and build-tested:

- Filtered sensor sampling and calibrated percentage conversion.
- Encrypted long-range board-to-board radio with acknowledgement and retries.
- LCD moisture, link strength, freshness, and last-update display.
- Local dashboard with age, analysis, history, and a two-mode sampling toggle.
- Selectable two-second or five-minute, always-awake sensor sampling.
- Signed cloud uploader, Supabase backend, and secured static dashboard.
- GitHub Pages workflow scoped to public files only.

Still requiring physical or deployment verification:

- Complete and save the wet/soil calibration on the gateway.
- Confirm both currently connected boards run the latest builds.
- Verify actual 200-foot range at the garden location.
- Confirm the continuous-power solar and battery budget is adequate.
- Add hardware if battery voltage reporting is required.
- Confirm current Supabase Auth passkey, GitHub Pages, and live-upload status.
- Confirm the external dashboard reports new records rather than cached or stale
  data.

See `README.md` for the complete operational procedures and troubleshooting
table, `dashboard/README.md` for the website API contract, and
`supabase/README.md` for backend deployment and security details.
