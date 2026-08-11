# Handoff — remaining physical setup

The cloud history schema, live-power columns, device ownership, Auth URLs, and
passkey relying-party settings are applied to the hosted Supabase project. The
production history RPC returns data for `garden-sensor-1`, and both firmware
targets compile locally.

## 1. Finish owner enrollment

An owner invitation was sent to the email on the Supabase account. Open that
message on the Mac, follow its link to the published dashboard, and choose
**Add a passkey**. Approve the Touch ID prompt. With Passwords & Keychain sync
enabled for the same Apple Account, the passkey should appear automatically on
the iPhone. If it does not, open the recovery email flow on the iPhone and add a
second passkey there.

This Touch ID/Face ID ceremony intentionally cannot be completed by automation.
After at least one passkey is registered, sign out and confirm **Continue with
passkey** restores both the live reading and history chart.

## 2. Turn alerts on

Alerts are the reason the project becomes useful day to day, and they are
**inert until you configure a delivery channel**. Set these as Edge Function
secrets (see `supabase/.env.example`):

- `SOIL_CRON_SECRET` — 32+ random chars
- Either `RESEND_API_KEY` + `SOIL_ALERT_FROM` + `SOIL_ALERT_TO`, or
  `SOIL_ALERT_WEBHOOK_URL` (ntfy, Pushover bridge, Slack — anything taking a
  JSON POST)
- `SOIL_DASHBOARD_URL` so notifications link back

Deploy `supabase functions deploy alert-monitor`, then schedule it (the exact
`cron.schedule` call is in the README's *Unattended operation* section).

Tune per device if you want:

```sql
update public.devices
set dry_threshold_pct = 25, battery_low_mv = null, alerts_enabled = true
where device_id = 'garden-sensor-1';
```

## 3. Flash both boards

Gateway first — it's the one that gained the watchdog, the flash spool, and
calibration. `pio run -e display_receiver -t upload`.

Then the sensor: `pio run -e sensor_transmitter -t upload`. The simplified
sensor stays awake, so its USB port should remain available. Use the BOOT/RESET
dance only if the normal upload port is unavailable.

After the gateway boots, set an OTA password from the setup portal so future
firmware goes over Wi-Fi instead of USB.

## 4. Calibrate the wet endpoint

Still the largest source of error in the whole system. `WET_RAW = 1300` has
never been measured, so today's percentage is a guess and any alert threshold
inherits that. Now doable from <http://soil-monitor.local> without reflashing:
read **Raw ADC** in dry soil, water it and read again once drained, enter both
in the **Calibration** card.

## Not done — needs hardware or a decision

- **Battery monitoring.** The simplified sensor reports battery voltage as
  unavailable; add both the measurement hardware and firmware support if it is
  needed.
- **Continuous-power sizing.** The sensor and probe remain awake, so size the
  battery/solar system for continuous load rather than deep-sleep duty cycling.
- **200-foot antenna field test.**
- **Rotate the Wi-Fi password.** It was pasted in plaintext in `chat.txt` and in
  the original chat. `chat.txt` is now git-ignored so it can't be published, but
  the password should be considered exposed. Rotating it means re-provisioning
  the gateway through the setup portal.
- **Multi-device support.** The schema already models it; the edge function and
  firmware still pin one device via `SOIL_DEVICE_ID` and hardcoded MACs. Left
  alone deliberately — changing the auth model would have risked the live
  ingest path for a capability you may never need.
