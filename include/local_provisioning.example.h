#pragma once

// Optional local-only bootstrap. Copy to local_provisioning.h, fill values,
// flash the display gateway, then keep the real file outside version control.
// Leave this at 0 for empty-NVS seeding. Increment it only when you explicitly
// want a new local header to replace previously applied bootstrap values once.
constexpr uint32_t LOCAL_PROVISIONING_VERSION = 0;
constexpr char LOCAL_WIFI_SSID[] = "";
constexpr char LOCAL_WIFI_PASSWORD[] = "";
constexpr char LOCAL_CLOUD_API_URL[] = "";
constexpr char LOCAL_CLOUD_DEVICE_ID[] = "";
constexpr char LOCAL_CLOUD_INGEST_SECRET[] = "";
