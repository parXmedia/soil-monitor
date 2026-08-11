-- Replaces evaluate_alerts on projects where the original retention migration
-- was already applied. GREATEST is a PostgreSQL grammar expression and cannot
-- be schema-qualified with the catalog name.

begin;

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
             secs => greatest(d.expected_interval_seconds * 3, 900)
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

revoke all on function public.evaluate_alerts() from public, anon, authenticated;
grant execute on function public.evaluate_alerts() to service_role;

commit;
