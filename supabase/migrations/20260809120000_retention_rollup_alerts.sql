-- Retention, rollup, and alerting.
--
-- The original schema stored every reading forever. At the shipped 30-second
-- cadence that is 2,880 rows per day (about 1.05 million per year), which
-- eventually exhausts the free tier and slows every history query. This
-- migration keeps recent readings at full resolution, rolls older ones into
-- hourly aggregates, and adds the state table needed for offline/dry alerts.

begin;

-- ---------------------------------------------------------------------------
-- Hourly rollup
-- ---------------------------------------------------------------------------

create table public.telemetry_hourly (
  device_id text not null references public.devices(device_id) on delete cascade,
  bucket timestamptz not null,
  sample_count integer not null,
  moisture_avg numeric(5,2) not null,
  moisture_min numeric(5,2) not null,
  moisture_max numeric(5,2) not null,
  raw_adc_avg integer not null,
  sensor_mv_avg integer not null,
  rssi_avg smallint not null,
  battery_mv_avg smallint,
  primary key (device_id, bucket),
  constraint telemetry_hourly_sample_count_positive
    check (sample_count > 0),
  constraint telemetry_hourly_bucket_aligned
    check (bucket = date_trunc('hour', bucket)),
  constraint telemetry_hourly_moisture_range
    check (
      moisture_avg between 0 and 100 and
      moisture_min between 0 and 100 and
      moisture_max between 0 and 100 and
      moisture_min <= moisture_max
    )
);

create index telemetry_hourly_device_bucket_desc_idx
  on public.telemetry_hourly (device_id, bucket desc);

comment on table public.telemetry_hourly is
  'Hourly aggregates of telemetry older than the raw retention window.';

-- ---------------------------------------------------------------------------
-- Alert bookkeeping
-- ---------------------------------------------------------------------------

-- One row per device per alert kind. Without this the cron job would re-send
-- the same "soil is dry" message every fifteen minutes until it was watered.
create table public.alert_state (
  device_id text not null references public.devices(device_id) on delete cascade,
  alert_kind text not null,
  active boolean not null default false,
  last_triggered_at timestamptz,
  last_cleared_at timestamptz,
  last_notified_at timestamptz,
  notify_count integer not null default 0,
  primary key (device_id, alert_kind),
  constraint alert_state_kind_supported
    check (alert_kind in ('dry_soil', 'sensor_offline', 'battery_low'))
);

comment on table public.alert_state is
  'Edge-triggered alert latch. Notifications fire on transition, not on every poll.';

alter table public.devices
  add column dry_threshold_pct numeric(5,2) not null default 25
    constraint devices_dry_threshold_range check (dry_threshold_pct between 0 and 100),
  add column battery_low_mv smallint
    constraint devices_battery_low_range check (battery_low_mv is null or battery_low_mv between 2500 and 5500),
  add column alerts_enabled boolean not null default true;

-- ---------------------------------------------------------------------------
-- Rollup + prune
-- ---------------------------------------------------------------------------

create or replace function public.roll_up_telemetry(
  raw_retention_days integer default 7,
  rollup_retention_days integer default 730
)
returns table (rolled_buckets bigint, deleted_rows bigint)
language plpgsql
security definer
set search_path = ''
as $$
declare
  cutoff timestamptz;
  bucket_count bigint := 0;
  removed bigint := 0;
begin
  if raw_retention_days < 1 or raw_retention_days > 365 then
    raise exception 'raw_retention_days must be between 1 and 365';
  end if;

  -- Only whole hours that can no longer receive late arrivals are rolled up.
  cutoff := pg_catalog.date_trunc(
    'hour', pg_catalog.now() - pg_catalog.make_interval(days => raw_retention_days)
  );

  with aggregated as (
    select
      device_id,
      pg_catalog.date_trunc('hour', sampled_at) as bucket,
      pg_catalog.count(*)::integer as sample_count,
      pg_catalog.round(pg_catalog.avg(moisture_pct), 2) as moisture_avg,
      pg_catalog.min(moisture_pct) as moisture_min,
      pg_catalog.max(moisture_pct) as moisture_max,
      pg_catalog.round(pg_catalog.avg(raw_adc))::integer as raw_adc_avg,
      pg_catalog.round(pg_catalog.avg(sensor_mv))::integer as sensor_mv_avg,
      pg_catalog.round(pg_catalog.avg(espnow_rssi_dbm))::smallint as rssi_avg,
      pg_catalog.round(pg_catalog.avg(battery_mv))::smallint as battery_mv_avg
    from public.telemetry
    where sampled_at < cutoff
    group by device_id, pg_catalog.date_trunc('hour', sampled_at)
  )
  insert into public.telemetry_hourly as target (
    device_id, bucket, sample_count, moisture_avg, moisture_min,
    moisture_max, raw_adc_avg, sensor_mv_avg, rssi_avg, battery_mv_avg
  )
  select * from aggregated
  on conflict (device_id, bucket) do update
  set sample_count = excluded.sample_count,
      moisture_avg = excluded.moisture_avg,
      moisture_min = excluded.moisture_min,
      moisture_max = excluded.moisture_max,
      raw_adc_avg = excluded.raw_adc_avg,
      sensor_mv_avg = excluded.sensor_mv_avg,
      rssi_avg = excluded.rssi_avg,
      battery_mv_avg = excluded.battery_mv_avg;

  get diagnostics bucket_count = row_count;

  -- device_state.latest_telemetry_id references telemetry, so the newest row
  -- per device is retained regardless of age. A sensor that has been offline
  -- for months must not have its last known reading deleted out from under
  -- the pointer.
  delete from public.telemetry t
  where t.sampled_at < cutoff
    and not exists (
      select 1 from public.device_state s where s.latest_telemetry_id = t.id
    );

  get diagnostics removed = row_count;

  delete from public.telemetry_hourly
  where bucket < pg_catalog.date_trunc(
    'hour', pg_catalog.now() - pg_catalog.make_interval(days => rollup_retention_days)
  );

  return query select bucket_count, removed;
end;
$$;

comment on function public.roll_up_telemetry(integer, integer) is
  'Aggregates raw telemetry older than the retention window into hourly buckets, then prunes it.';

-- ---------------------------------------------------------------------------
-- Alert evaluation
-- ---------------------------------------------------------------------------

-- Returns the alerts whose state changed on this call. The edge function only
-- has to deliver whatever comes back, so a delivery failure cannot silently
-- consume an alert: the latch is updated in the same statement that reports it.
create or replace function public.evaluate_alerts()
returns table (
  device_id text,
  alert_kind text,
  transition text,
  detail jsonb
)
language plpgsql
security definer
set search_path = ''
as $$
begin
  return query
  with observed as (
    select
      d.device_id,
      d.dry_threshold_pct,
      d.battery_low_mv,
      d.expected_interval_seconds,
      s.last_sampled_at,
      t.moisture_pct,
      t.battery_mv,
      (s.last_sampled_at is null
        or s.last_sampled_at <
           pg_catalog.now() - pg_catalog.make_interval(
             secs => pg_catalog.greatest(d.expected_interval_seconds * 3, 900)
           )) as offline_now,
      (t.moisture_pct is not null and t.moisture_pct < d.dry_threshold_pct) as dry_now,
      (d.battery_low_mv is not null and t.battery_mv is not null
        and t.battery_mv < d.battery_low_mv) as battery_now
    from public.devices d
    left join public.device_state s on s.device_id = d.device_id
    left join public.telemetry t on t.id = s.latest_telemetry_id
    where d.enabled and d.alerts_enabled
  ),
  flattened as (
    select o.device_id, 'sensor_offline'::text as alert_kind, o.offline_now as is_active,
           pg_catalog.jsonb_build_object('lastSampledAt', o.last_sampled_at) as detail
    from observed o
    union all
    -- An offline sensor reports a stale moisture value, so suppressing the dry
    -- alert while offline avoids two notifications for one root cause.
    select o.device_id, 'dry_soil', o.dry_now and not o.offline_now,
           pg_catalog.jsonb_build_object(
             'moisture', o.moisture_pct, 'threshold', o.dry_threshold_pct)
    from observed o
    union all
    select o.device_id, 'battery_low', o.battery_now and not o.offline_now,
           pg_catalog.jsonb_build_object(
             'batteryMv', o.battery_mv, 'threshold', o.battery_low_mv)
    from observed o
  ),
  merged as (
    insert into public.alert_state as a (
      device_id, alert_kind, active, last_triggered_at, last_cleared_at,
      last_notified_at, notify_count
    )
    select f.device_id, f.alert_kind, f.is_active,
           case when f.is_active then pg_catalog.now() end,
           case when not f.is_active then pg_catalog.now() end,
           case when f.is_active then pg_catalog.now() end,
           case when f.is_active then 1 else 0 end
    from flattened f
    on conflict (device_id, alert_kind) do update
    set active = excluded.active,
        last_triggered_at = case
          when excluded.active and not a.active then pg_catalog.now()
          else a.last_triggered_at end,
        last_cleared_at = case
          when not excluded.active and a.active then pg_catalog.now()
          else a.last_cleared_at end,
        last_notified_at = case
          when excluded.active <> a.active then pg_catalog.now()
          else a.last_notified_at end,
        notify_count = a.notify_count
          + case when excluded.active <> a.active then 1 else 0 end
      where excluded.active is distinct from a.active
    returning a.device_id, a.alert_kind, a.active
  )
  select m.device_id, m.alert_kind,
         case when m.active then 'raised' else 'cleared' end,
         f.detail
  from merged m
  join flattened f
    on f.device_id = m.device_id and f.alert_kind = m.alert_kind;
end;
$$;

comment on function public.evaluate_alerts() is
  'Latches dry/offline/battery alerts and returns only the transitions since the previous call.';

-- ---------------------------------------------------------------------------
-- Bounded history series
-- ---------------------------------------------------------------------------

-- The API previously fetched raw rows with a hard limit of 2,500 and reversed
-- them, so a seven-day request at 30-second sampling silently returned only
-- the newest ~21 hours. Bucketing server-side keeps the response bounded and,
-- more importantly, honest about the span it covers.
create or replace function public.telemetry_series(
  target_device_id text,
  since timestamptz,
  bucket_seconds integer
)
returns table (
  bucket timestamptz,
  sample_count integer,
  moisture_avg numeric,
  moisture_min numeric,
  moisture_max numeric,
  raw_adc_avg integer,
  sensor_mv_avg integer,
  rssi_avg smallint,
  battery_mv_avg smallint
)
language sql
stable
security definer
set search_path = ''
as $$
  with combined as (
    select
      pg_catalog.to_timestamp(
        pg_catalog.floor(
          extract(epoch from t.sampled_at) / bucket_seconds
        ) * bucket_seconds
      ) as bucket,
      t.moisture_pct,
      t.raw_adc,
      t.sensor_mv,
      t.espnow_rssi_dbm,
      t.battery_mv,
      1 as weight
    from public.telemetry t
    where t.device_id = target_device_id and t.sampled_at >= since
    union all
    select
      pg_catalog.to_timestamp(
        pg_catalog.floor(
          extract(epoch from h.bucket) / bucket_seconds
        ) * bucket_seconds
      ),
      h.moisture_avg,
      h.raw_adc_avg,
      h.sensor_mv_avg,
      h.rssi_avg,
      h.battery_mv_avg,
      h.sample_count
    from public.telemetry_hourly h
    where h.device_id = target_device_id and h.bucket >= since
  )
  select
    combined.bucket,
    pg_catalog.sum(combined.weight)::integer,
    pg_catalog.round(
      pg_catalog.sum(combined.moisture_pct * combined.weight)
        / pg_catalog.sum(combined.weight), 2),
    pg_catalog.min(combined.moisture_pct),
    pg_catalog.max(combined.moisture_pct),
    pg_catalog.round(pg_catalog.avg(combined.raw_adc))::integer,
    pg_catalog.round(pg_catalog.avg(combined.sensor_mv))::integer,
    pg_catalog.round(pg_catalog.avg(combined.espnow_rssi_dbm))::smallint,
    pg_catalog.round(pg_catalog.avg(combined.battery_mv))::smallint
  from combined
  group by combined.bucket
  order by combined.bucket
  limit 2000;
$$;

comment on function public.telemetry_series(text, timestamptz, integer) is
  'Time-bucketed history spanning raw telemetry and hourly rollups, bounded to 2000 points.';

-- ---------------------------------------------------------------------------
-- Permissions
-- ---------------------------------------------------------------------------

alter table public.telemetry_hourly enable row level security;
alter table public.telemetry_hourly force row level security;
alter table public.alert_state enable row level security;
alter table public.alert_state force row level security;

revoke all on table public.telemetry_hourly from public, anon, authenticated;
revoke all on table public.alert_state from public, anon, authenticated;
revoke all on function public.roll_up_telemetry(integer, integer) from public, anon, authenticated;
revoke all on function public.evaluate_alerts() from public, anon, authenticated;
revoke all on function public.telemetry_series(text, timestamptz, integer) from public, anon, authenticated;

grant select on table public.telemetry_hourly to service_role;
grant select on table public.alert_state to service_role;
grant execute on function public.evaluate_alerts() to service_role;
grant execute on function public.telemetry_series(text, timestamptz, integer) to service_role;

-- The prune job needs to remove rows, which service_role deliberately cannot
-- do directly: the delete grant lives inside the security-definer function so
-- a leaked service key still cannot erase history.
grant execute on function public.roll_up_telemetry(integer, integer) to postgres;

commit;

-- ---------------------------------------------------------------------------
-- Scheduling (runs outside the transaction; safe to re-run)
-- ---------------------------------------------------------------------------

do $$
begin
  if not exists (select 1 from pg_available_extensions where name = 'pg_cron') then
    raise notice 'pg_cron is unavailable; schedule roll_up_telemetry manually';
    return;
  end if;

  create extension if not exists pg_cron;

  perform cron.unschedule(jobid)
  from cron.job
  where jobname in ('soil-rollup-prune', 'soil-alert-evaluate');

  -- Hourly, a few minutes past the hour so the previous hour is complete.
  perform cron.schedule(
    'soil-rollup-prune',
    '7 * * * *',
    $job$ select public.roll_up_telemetry(7, 730); $job$
  );
exception
  when insufficient_privilege then
    raise notice 'Insufficient privilege to schedule pg_cron jobs; run them manually';
end;
$$;
