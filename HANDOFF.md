# Handoff — what to run next

Six commits are on the local `hardening` branch. **Nothing has been pushed** — the
sandbox has no GitHub credentials.

## 1. Push and let CI compile the firmware

The firmware changes are substantial and **were not compiled locally** — the
sandbox can't reach the PlatformIO registry to download the ESP32 toolchain. The
new CI workflow builds both targets, so push the branch first and let it check:

```sh
cd ~/Documents/CS\ Projects/openfarms
git push -u origin hardening
```

Watch <https://github.com/parXmedia/soil-monitor/actions>. When `firmware`,
`backend`, and `dashboard` are all green:

```sh
git checkout main && git merge --ff-only hardening && git push
```

Pushing to `main` redeploys GitHub Pages. If a build fails, send me the log and
I'll fix it rather than you debugging my code.

## 2. Apply the database migration

`supabase/migrations/20260809120000_retention_rollup_alerts.sql` is not applied
yet. Run it via `supabase db push`, or paste it into the SQL editor. It is
transactional and safe to re-run.

Afterwards confirm the hourly job exists:

```sql
select jobname, schedule from cron.job;
```

If `pg_cron` wasn't available the migration prints a notice instead of failing —
in that case schedule `select public.roll_up_telemetry(7, 730);` yourself.

## 3. Turn alerts on

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
where device_id = 'garden-01';
```

## 4. Flash both boards

Gateway first — it's the one that gained the watchdog, the flash spool, and
calibration. `pio run -e display_receiver -t upload`.

Then the sensor: `pio run -e sensor_transmitter -t upload`. This one needs the
BOOT/RESET dance because deep sleep hides its USB port — hold **BOOT**, tap
**RESET**, release **BOOT**, then upload.

After the gateway boots, set an OTA password from the setup portal so future
firmware goes over Wi-Fi instead of USB.

## 5. Calibrate the wet endpoint

Still the largest source of error in the whole system. `WET_RAW = 1300` has
never been measured, so today's percentage is a guess and any alert threshold
inherits that. Now doable from <http://soil-monitor.local> without reflashing:
read **Raw ADC** in dry soil, water it and read again once drained, enter both
in the **Calibration** card.

## Not done — needs hardware or a decision

- **Battery divider.** Firmware support is in and gated behind
  `BATTERY_SENSE_PIN`, but the two resistors aren't fitted, so you still get no
  warning before the garden node dies.
- **MOSFET load switch** for the probe (`SENSOR_POWER_PIN` is still `-1`, so the
  probe draws current through deep sleep).
- **200-foot antenna field test.**
- **Rotate the Wi-Fi password.** It was pasted in plaintext in `chat.txt` and in
  the original chat. `chat.txt` is now git-ignored so it can't be published, but
  the password should be considered exposed. Rotating it means re-provisioning
  the gateway through the setup portal.
- **Multi-device support.** The schema already models it; the edge function and
  firmware still pin one device via `SOIL_DEVICE_ID` and hardcoded MACs. Left
  alone deliberately — changing the auth model would have risked the live
  ingest path for a capability you may never need.
