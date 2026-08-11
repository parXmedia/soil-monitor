begin;

-- Protocol v4 adds a continuous diagnostic mode. Power fields are nullable so
-- sleep-mode packets and older gateways remain valid during a rolling upgrade.
alter table public.telemetry
  add column current_ma smallint,
  add column power_mw integer,
  add column power_measured boolean,
  add column sampling_mode varchar(16),
  add column sensor_firmware_build bigint,
  add constraint telemetry_current_ma_range
    check (current_ma is null or current_ma between 0 and 20000),
  add constraint telemetry_power_mw_range
    check (power_mw is null or power_mw between 0 and 100000),
  add constraint telemetry_power_pair
    check ((current_ma is null) = (power_mw is null)),
  add constraint telemetry_power_measurement_consistent
    check (power_measured is null or current_ma is not null),
  add constraint telemetry_sampling_mode_supported
    check (sampling_mode is null or sampling_mode in ('live', 'fast', 'low_power')),
  add constraint telemetry_sensor_firmware_build_range
    check (sensor_firmware_build is null or sensor_firmware_build between 1 and 4294967295);

comment on column public.telemetry.power_measured is
  'True for external current-sense hardware; false for the documented firmware estimate.';

commit;
