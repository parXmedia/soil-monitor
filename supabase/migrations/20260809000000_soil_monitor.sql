begin;

create table public.devices (
  device_id text primary key,
  owner_id uuid references auth.users(id) on delete set null,
  display_name text not null,
  enabled boolean not null default true,
  expected_interval_seconds integer not null default 300,
  ingest_key_version smallint not null default 1,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  constraint devices_device_id_format
    check (device_id ~ '^[a-z0-9][a-z0-9_-]{2,63}$'),
  constraint devices_display_name_length
    check (char_length(display_name) between 1 and 80),
  constraint devices_expected_interval_range
    check (expected_interval_seconds between 30 and 86400),
  constraint devices_ingest_key_version_positive
    check (ingest_key_version > 0)
);

create table public.telemetry (
  id bigint generated always as identity primary key,
  device_id text not null references public.devices(device_id) on delete cascade,
  boot_id uuid not null,
  sequence bigint not null,
  schema_version smallint not null default 1,
  sampled_at timestamptz not null,
  received_at timestamptz not null default now(),
  moisture_pct numeric(5,2) not null,
  raw_adc smallint not null,
  sensor_mv smallint not null,
  espnow_rssi_dbm smallint not null,
  battery_mv smallint,
  battery_percent numeric(5,2),
  uptime_seconds bigint,
  sensor_firmware varchar(32),
  gateway_firmware varchar(32),
  payload_sha256 char(64) not null,
  constraint telemetry_device_boot_sequence_unique
    unique (device_id, boot_id, sequence),
  constraint telemetry_sequence_range
    check (sequence between 0 and 4294967295),
  constraint telemetry_schema_version_supported
    check (schema_version = 1),
  constraint telemetry_moisture_range
    check (moisture_pct between 0 and 100),
  constraint telemetry_raw_adc_range
    check (raw_adc between 0 and 4095),
  constraint telemetry_sensor_mv_range
    check (sensor_mv between 0 and 5000),
  constraint telemetry_rssi_range
    check (espnow_rssi_dbm between -127 and 0),
  constraint telemetry_battery_mv_range
    check (battery_mv is null or battery_mv between 2500 and 5500),
  constraint telemetry_battery_percent_range
    check (battery_percent is null or battery_percent between 0 and 100),
  constraint telemetry_uptime_range
    check (uptime_seconds is null or uptime_seconds between 0 and 4294967295),
  constraint telemetry_sensor_firmware_format
    check (sensor_firmware is null or sensor_firmware ~ '^[A-Za-z0-9._+-]{1,32}$'),
  constraint telemetry_gateway_firmware_format
    check (gateway_firmware is null or gateway_firmware ~ '^[A-Za-z0-9._+-]{1,32}$'),
  constraint telemetry_payload_hash_format
    check (payload_sha256 ~ '^[0-9a-f]{64}$')
);

create table public.device_state (
  device_id text primary key references public.devices(device_id) on delete cascade,
  latest_telemetry_id bigint not null unique references public.telemetry(id) on delete cascade,
  last_sampled_at timestamptz not null,
  last_received_at timestamptz not null,
  updated_at timestamptz not null default now()
);

create index telemetry_device_sampled_desc_idx
  on public.telemetry (device_id, sampled_at desc);

create index telemetry_device_received_desc_idx
  on public.telemetry (device_id, received_at desc);

create index telemetry_received_brin_idx
  on public.telemetry using brin (received_at);

create or replace function public.update_device_state_from_telemetry()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  insert into public.device_state as current_state (
    device_id,
    latest_telemetry_id,
    last_sampled_at,
    last_received_at,
    updated_at
  ) values (
    new.device_id,
    new.id,
    new.sampled_at,
    new.received_at,
    pg_catalog.now()
  )
  on conflict (device_id) do update
  set latest_telemetry_id = excluded.latest_telemetry_id,
      last_sampled_at = excluded.last_sampled_at,
      last_received_at = excluded.last_received_at,
      updated_at = pg_catalog.now()
  where (excluded.last_sampled_at, excluded.latest_telemetry_id)
      > (current_state.last_sampled_at, current_state.latest_telemetry_id);

  return new;
end;
$$;

create trigger telemetry_update_device_state
after insert on public.telemetry
for each row execute function public.update_device_state_from_telemetry();

alter table public.devices enable row level security;
alter table public.devices force row level security;
alter table public.telemetry enable row level security;
alter table public.telemetry force row level security;
alter table public.device_state enable row level security;
alter table public.device_state force row level security;

-- No RLS policies are intentionally created. Even if a future broad grant is
-- added accidentally, anon/authenticated requests still have no permitted rows.
revoke all on table public.devices from public, anon, authenticated;
revoke all on table public.telemetry from public, anon, authenticated;
revoke all on table public.device_state from public, anon, authenticated;
revoke all on sequence public.telemetry_id_seq from public, anon, authenticated;
revoke all on function public.update_device_state_from_telemetry() from public, anon, authenticated;

grant select on table public.devices to service_role;
grant select, insert on table public.telemetry to service_role;
grant select on table public.device_state to service_role;
grant usage, select on sequence public.telemetry_id_seq to service_role;

comment on table public.devices is
  'Provisioned sensor gateways. Device secrets are stored only as Edge Function secrets, never in this table.';
comment on table public.telemetry is
  'Immutable, range-constrained soil readings accepted by the authenticated Edge Function.';
comment on table public.device_state is
  'Pointer to the newest sample per device for efficient current-value reads.';

commit;
