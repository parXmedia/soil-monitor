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

The battery-powered garden board does not join the home Wi-Fi network. It sends
short encrypted radio messages to the indoor display board, then returns to deep
sleep. The display board remains powered, shows the reading, hosts the local
dashboard, and performs outbound cloud uploads.

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
       ▲                                                  │
       └──── requested 30-second or 5-minute mode ────────┘
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

Each garden-node wake cycle follows this sequence:

1. Wake from deep sleep.
2. Wait for the moisture probe to settle.
3. Take 17 ADC and millivolt readings.
4. Sort the readings, discard the three highest and three lowest, and average
   the remaining 11. This trimmed mean reduces electrical noise and outliers.
5. Convert the filtered raw ADC value to 0–100% using the dry and wet calibration
   endpoints.
6. Mark obviously invalid ADC values as a likely wiring or sensor problem.
7. Create a versioned 30-byte packet containing moisture, raw ADC, sensor
   voltage, sequence number, status flags, and the next sample interval.
8. Send it to the allowlisted gateway with encrypted ESP-NOW unicast.
9. Wait for an application acknowledgement. Retry the last known channel first,
   then scan Wi-Fi channels 1–11 when necessary.
10. Accept the sampling interval requested in the acknowledgement.
11. Turn off Wi-Fi and enter deep sleep until the next sample.

The gateway validates the sender MAC address, packet size, protocol version,
message type, and value ranges before accepting a reading. It tracks received
and missing sequence numbers, records RSSI, updates the LCD and local dashboard,
acknowledges the packet, and queues a cloud upload when cloud settings exist.

## 5. Sampling modes and freshness

The project currently supports two modes:

| Mode | Interval | Intended use |
|---|---:|---|
| Fast | 30 seconds | Bench testing, calibration, and live diagnosis |
| Low power | 5 minutes | Normal solar/battery garden operation |

The local dashboard's **Sampling mode** switch changes the gateway's requested
mode. ON means 30-second test updates; OFF means five-minute low-power operation.
The sensor receives the choice in the next valid radio acknowledgement, stores
it in RTC memory, and uses it for later wake cycles. A recently requested change
may therefore show as pending until the sensor checks in.

The LCD and local website do not label an old value as continuously connected.
They display the elapsed time since the last sensor update and change from fresh
to stale when the expected next reading plus a bounded radio allowance has
passed. In fast mode this is about 60 seconds; in low-power mode it is about six
minutes.

The cloud dashboard polls its API every 15 seconds. Polling more frequently does
not create new sensor samples; it only checks whether a new uploaded sample has
arrived.

## 6. Moisture calibration

The active endpoints are defined in `src/main.cpp`:

```cpp
constexpr int DRY_RAW = 2513;
constexpr int WET_RAW = 1300;
```

`DRY_RAW=2513` was measured from this sensor in air at 3.3 V. The wet endpoint
is still provisional. A trustworthy soil percentage requires calibration in the
actual soil type:

1. Put the probe in representative dry soil and record the stable `RAW` value
   shown on the LCD or local dashboard.
2. Fully water that soil, allow excess water to drain, and record the stable wet
   value.
3. Replace `DRY_RAW` and `WET_RAW` with those measurements.
4. Rebuild and flash the `sensor_transmitter` environment.

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
- The 30-second / five-minute mode control.

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
6. The viewer supplies a separate read-only access token at runtime. The site
   keeps it only in that browser tab's `sessionStorage`.

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

The ESP32 sensor node deep-sleeps, but the probe is currently connected directly
to `3V3`, and `SENSOR_POWER_PIN=-1`. The probe therefore stays powered while the
ESP32 sleeps. For efficient long-term solar operation, add a suitable 3.3 V load
switch or MOSFET circuit controlled by a spare GPIO, give the control signal a
defined sleep state, and configure that GPIO as `SENSOR_POWER_PIN`. Do not assume
an unknown probe can safely be powered directly from an ESP32 GPIO.

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
| `src/main.cpp` | Both firmware roles, selected at build time |
| `include/board_pins.h` | LAFVIN LCD controller and pin map |
| `include/soil_protocol.h` | Versioned radio packet and acknowledgement formats |
| `include/web_dashboard.h` | Local dashboard and Wi-Fi/cloud setup pages embedded in firmware |
| `include/radio_secrets.example.h` | Safe template for private ESP-NOW/setup credentials |
| `include/local_provisioning.example.h` | Safe template for optional local gateway provisioning |
| `dashboard/` | Dependency-free GitHub Pages dashboard |
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

The sensor USB port normally disappears while it deep-sleeps. Hold BOOT while
resetting or reconnecting the XIAO to enter its bootloader when necessary.

Run the public-dashboard checks with:

```sh
python3 -m unittest discover -s dashboard/tests -v
```

The most recent clean-workspace validation passed all nine dashboard tests and
successfully built both firmware environments. That verifies source integrity;
it does not replace a live radio, sensor, Wi-Fi, and cloud test with the physical
boards.

## 14. Current completion status

Implemented and build-tested:

- Filtered sensor sampling and calibrated percentage conversion.
- Encrypted long-range board-to-board radio with acknowledgement and retries.
- LCD moisture, link strength, freshness, and last-update display.
- Local dashboard with live age, analysis, history, and mode control.
- Fast and low-power sampling commands.
- Signed cloud uploader, Supabase backend, and secured static dashboard.
- GitHub Pages workflow scoped to public files only.

Still requiring physical or deployment verification:

- Complete the wet/soil calibration and flash the final endpoints.
- Confirm both currently connected boards run the latest builds.
- Verify actual 200-foot range at the garden location.
- Add switched probe power for best solar efficiency.
- Add hardware if battery voltage reporting is required.
- Confirm current Supabase, GitHub Pages, access-token, and live-upload status.
- Confirm the external dashboard reports new records rather than cached or stale
  data.

See `README.md` for the complete operational procedures and troubleshooting
table, `dashboard/README.md` for the website API contract, and
`supabase/README.md` for backend deployment and security details.
