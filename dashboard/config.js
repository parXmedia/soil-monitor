/*
 * Public runtime configuration for GitHub Pages.
 *
 * IMPORTANT: Every file published with GitHub Pages is public. Never put a
 * Wi-Fi password, API write key, private token, or database service key here.
 * The configured endpoint must be HTTPS and expose read-only sensor data.
 */
window.SOIL_MONITOR_CONFIG = Object.freeze({
  // Safe to publish: use the sb_publishable_ key from Supabase Settings → API.
  // This key identifies the project; the signed-in user's short-lived JWT
  // authorizes access to garden data.
  supabaseUrl: "https://hezqmueouqjjgjdkavlc.supabase.co",
  publishableKey: "sb_publishable_cdPD9fNRgRefdrJZyzodjA_gh5MBIFW",

  apiBaseUrl: "https://hezqmueouqjjgjdkavlc.supabase.co/functions/v1/soil-api",

  currentPath: "/v1/current",
  historyPath: "/v1/history",
  pollIntervalMs: 2000,
  historyRefreshIntervalMs: 300000,
  requestTimeoutMs: 8000,
  // Sensor reports every 5 minutes; mark stale 1 minute after a missed update.
  staleAfterMs: 360000,

  // Thresholds are percentages and should match the calibrated sensor.
  thresholds: Object.freeze({
    dryBelow: 25,
    healthyBelow: 80
  })
});
