-- Replaces roll_up_telemetry on projects where the original retention
-- migration was already applied. Editing 20260809120000 in place only helps
-- projects that have not run it yet.
--
-- The version shipped in that migration aggregated the row referenced by
-- device_state.latest_telemetry_id, but its DELETE deliberately spared that
-- row. For a device that has been offline longer than the retention window the
-- same reading was therefore re-aggregated on every hourly run and, because the
-- conflict clause assigned excluded.sample_count directly, replaced the whole
-- hour bucket with that single sample each time.

begin;

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
    from public.telemetry t
    where t.sampled_at < cutoff
      and not exists (
        select 1 from public.device_state s where s.latest_telemetry_id = t.id
      )
    group by device_id, pg_catalog.date_trunc('hour', sampled_at)
  )
  insert into public.telemetry_hourly as target (
    device_id, bucket, sample_count, moisture_avg, moisture_min,
    moisture_max, raw_adc_avg, sensor_mv_avg, rssi_avg, battery_mv_avg
  )
  select * from aggregated
  on conflict (device_id, bucket) do update
  -- A gateway may replay a 7–30 day-old sample after this hour was already
  -- rolled up. Merge the new partial aggregate instead of replacing the
  -- original hour and silently losing its earlier samples.
  set moisture_avg = case
        when target.moisture_avg is null then excluded.moisture_avg
        when excluded.moisture_avg is null then target.moisture_avg
        else pg_catalog.round(
          (target.moisture_avg * target.sample_count +
           excluded.moisture_avg * excluded.sample_count) /
          (target.sample_count + excluded.sample_count), 2)
      end,
      moisture_min = least(target.moisture_min, excluded.moisture_min),
      moisture_max = greatest(target.moisture_max, excluded.moisture_max),
      raw_adc_avg = case
        when target.raw_adc_avg is null then excluded.raw_adc_avg
        when excluded.raw_adc_avg is null then target.raw_adc_avg
        else pg_catalog.round(
          (target.raw_adc_avg::numeric * target.sample_count +
           excluded.raw_adc_avg::numeric * excluded.sample_count) /
          (target.sample_count + excluded.sample_count))::integer
      end,
      sensor_mv_avg = case
        when target.sensor_mv_avg is null then excluded.sensor_mv_avg
        when excluded.sensor_mv_avg is null then target.sensor_mv_avg
        else pg_catalog.round(
          (target.sensor_mv_avg::numeric * target.sample_count +
           excluded.sensor_mv_avg::numeric * excluded.sample_count) /
          (target.sample_count + excluded.sample_count))::integer
      end,
      rssi_avg = case
        when target.rssi_avg is null then excluded.rssi_avg
        when excluded.rssi_avg is null then target.rssi_avg
        else pg_catalog.round(
          (target.rssi_avg::numeric * target.sample_count +
           excluded.rssi_avg::numeric * excluded.sample_count) /
          (target.sample_count + excluded.sample_count))::smallint
      end,
      battery_mv_avg = case
        when target.battery_mv_avg is null then excluded.battery_mv_avg
        when excluded.battery_mv_avg is null then target.battery_mv_avg
        else pg_catalog.round(
          (target.battery_mv_avg::numeric * target.sample_count +
           excluded.battery_mv_avg::numeric * excluded.sample_count) /
          (target.sample_count + excluded.sample_count))::smallint
      end,
      sample_count = target.sample_count + excluded.sample_count;

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

revoke all on function public.roll_up_telemetry(integer, integer) from public, anon, authenticated;
grant execute on function public.roll_up_telemetry(integer, integer) to postgres;

commit;
