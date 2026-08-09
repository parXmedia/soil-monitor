#include <Arduino.h>
#include <WiFi.h>
#include <algorithm>
#include <esp_now.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include "soil_protocol.h"
#if __has_include("radio_secrets.h")
#include "radio_secrets.h"
#else
#error "Copy include/radio_secrets.example.h to include/radio_secrets.h and replace the example keys"
#endif
#if !defined(RADIO_SECRETS_CONFIGURED) || !RADIO_SECRETS_CONFIGURED
#error "Replace the example radio credentials and set RADIO_SECRETS_CONFIGURED to 1"
#endif

#if defined(DEVICE_ROLE_DISPLAY)

#include <ArduinoOTA.h>
#include <Arduino_GFX_Library.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include "board_pins.h"
#if __has_include("local_provisioning.h")
#include "local_provisioning.h"
#define HAS_LOCAL_PROVISIONING 1
#else
#define HAS_LOCAL_PROVISIONING 0
#endif
#include "tls_roots.h"
#include "web_dashboard.h"

namespace {

constexpr uint8_t SENSOR_MAC[] = {0xA4, 0xCB, 0x8F, 0xD2, 0x6E, 0x34};
constexpr char GATEWAY_FIRMWARE_VERSION[] = "2.1.0";
constexpr char SENSOR_FIRMWARE_VERSION[] = "2.1.0";

constexpr uint16_t C_BLACK = 0x0000;
constexpr uint16_t C_WHITE = 0xFFFF;
constexpr uint16_t C_RED = 0xF800;
constexpr uint16_t C_GREEN = 0x07E0;
constexpr uint16_t C_YELLOW = 0xFFE0;
constexpr uint16_t C_CYAN = 0x07FF;
constexpr uint16_t C_DARK_BLUE = 0x018C;
constexpr uint16_t C_DARK_GREY = 0x4208;
constexpr uint16_t FAST_SAMPLE_SECONDS = 30;
constexpr uint16_t LOW_POWER_SAMPLE_SECONDS = 300;
constexpr uint16_t LEGACY_ACK_PACKET_SIZE = 16;
// Version 2 makes five-minute low-power sampling the shipped default. Thirty
// second sampling is a bench-test cadence: it burns sensor battery and, at
// 2,880 rows per day, fills the database roughly ten times faster.
constexpr uint16_t MODE_SETTINGS_VERSION = 2;

// The gateway supervises itself. Wi-Fi association, a TLS handshake, and an
// mDNS responder can each wedge the network stack; without a watchdog the
// display keeps showing a stale reading and nobody notices for days.
constexpr uint32_t WATCHDOG_TIMEOUT_SECONDS = 60;
// Independent of the watchdog: if the radio has been silent this long the
// gateway reboots on the assumption that its own receiver, not the sensor,
// is the broken half. Deliberately longer than any supported sample interval.
constexpr uint32_t SILENT_RADIO_REBOOT_MS = 6UL * 60UL * 60UL * 1000UL;

// Moisture is derived on the gateway from the raw ADC value so the endpoints
// can be recalibrated from the dashboard without waking and reflashing a
// battery-powered sensor that is buried in a garden bed.
constexpr uint16_t DEFAULT_CALIBRATION_DRY_RAW = 2513;
constexpr uint16_t DEFAULT_CALIBRATION_WET_RAW = 1300;
constexpr uint16_t CALIBRATION_MIN_SEPARATION = 200;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
Arduino_GFX *display = new Arduino_ST7789(
    bus, LCD_RST, 0, true, LCD_WIDTH, LCD_HEIGHT, 0, 20, 0, 0);

portMUX_TYPE packetMux = portMUX_INITIALIZER_UNLOCKED;
SoilPacket latestPacket = {};
volatile bool packetReceived = false;
volatile uint32_t lastPacketAt = 0;
volatile int8_t latestRssi = -127;
uint32_t lastRenderedSequence = UINT32_MAX;
bool lastLinkState = false;
uint32_t lastFooterRefreshAt = 0;

constexpr size_t HISTORY_CAPACITY = 288; // 4.8 hours at one-minute trend samples.
struct HistorySample {
  uint32_t capturedAt;
  uint8_t moisture;
};
HistorySample historySamples[HISTORY_CAPACITY] = {};
size_t historyHead = 0;
size_t historyCount = 0;
uint32_t lastHistoryAt = 0;

volatile uint32_t receivedPacketCount = 0;
volatile uint32_t missedPacketCount = 0;
volatile uint32_t lastReceivedSequence = 0;
volatile uint32_t lastReceivedBootId = 0;

WebServer webServer(80);
DNSServer dnsServer;
Preferences preferences;
bool setupPortalActive = false;
bool webServerStarted = false;
bool otaInProgress = false;
uint32_t scheduledRestartAt = 0;
uint8_t gatewayChannel = 0;
uint32_t lastStatusLogAt = 0;
uint32_t setupPortalStartedAt = 0;
uint32_t lastWifiReconnectAt = 0;
constexpr uint32_t SETUP_PORTAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;

volatile bool acknowledgementPending = false;
SoilAckPacket pendingAcknowledgement = {};
volatile uint16_t pendingAcknowledgementLength = sizeof(SoilAckPacket);
volatile uint16_t requestedSampleSeconds = FAST_SAMPLE_SECONDS;

struct CloudUploadRecord {
  SoilPacket packet;
  int8_t rssi;
  time_t sampledAt;
};

enum class CloudUploadResult : uint8_t {
  Succeeded,
  RetryableFailure,
  PermanentFailure,
};

constexpr uint8_t MAX_CLOUD_UPLOAD_ATTEMPTS = 8;

QueueHandle_t cloudUploadQueue = nullptr;
String cloudApiUrl;
String cloudDeviceId;
String cloudIngestSecret;
volatile bool cloudUploadOnline = false;
volatile int cloudLastHttpStatus = 0;

uint16_t calibrationDryRaw = DEFAULT_CALIBRATION_DRY_RAW;
uint16_t calibrationWetRaw = DEFAULT_CALIBRATION_WET_RAW;
bool calibrationFromUser = false;

// Flash-backed spool for cloud uploads. The previous twelve-slot RAM queue
// lost every buffered reading on reboot and silently evicted the oldest entry
// while the uploader was busy retrying, so an internet outage longer than a
// few minutes punched a permanent hole in the history.
constexpr char SPOOL_PATH[] = "/spool.bin";
constexpr char SPOOL_CURSOR_PATH[] = "/spool.pos";
constexpr uint32_t SPOOL_RECORD_MAGIC = 0x53504C31; // "SPL1"
constexpr size_t MAX_SPOOL_BYTES = 192 * 1024;
constexpr size_t SPOOL_COMPACT_THRESHOLD = 32 * 1024;

struct __attribute__((packed)) SpoolRecord {
  uint32_t magic;
  SoilPacket packet;
  int8_t rssi;
  int64_t sampledAt;
  uint32_t crc;
};

bool spoolReady = false;
SemaphoreHandle_t spoolMutex = nullptr;
uint32_t spoolCursor = 0;
volatile uint32_t spoolDroppedCount = 0;

// Defined with the rest of the spool implementation below, but reported by the
// dashboard API which is declared earlier in this file.
size_t spoolPendingCount();

void watchdogFeed() { esp_task_wdt_reset(); }

// Splits a long wait into watchdog-sized slices so the uploader can back off
// for minutes without the supervisor mistaking patience for a hang.
void watchdogDelay(uint32_t milliseconds) {
  constexpr uint32_t slice = 2000;
  while (milliseconds > 0) {
    const uint32_t step = milliseconds < slice ? milliseconds : slice;
    vTaskDelay(pdMS_TO_TICKS(step));
    watchdogFeed();
    milliseconds -= step;
  }
}

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t length) {
  crc = ~crc;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (-static_cast<int32_t>(crc & 1)));
    }
  }
  return ~crc;
}

uint32_t spoolRecordCrc(const SpoolRecord &record) {
  return crc32Update(0, reinterpret_cast<const uint8_t *>(&record),
                     sizeof(SpoolRecord) - sizeof(uint32_t));
}

uint8_t moistureFromRaw(uint16_t rawAdc) {
  if (calibrationDryRaw <= calibrationWetRaw + CALIBRATION_MIN_SEPARATION) {
    return 0;
  }
  const long percent = map(rawAdc, calibrationDryRaw, calibrationWetRaw, 0, 100);
  return static_cast<uint8_t>(constrain(percent, 0L, 100L));
}

// Prefers the gateway's own calibration and falls back to whatever the sensor
// computed if the stored endpoints are unusable.
uint8_t moistureFor(const SoilPacket &packet) {
  if (calibrationDryRaw <= calibrationWetRaw + CALIBRATION_MIN_SEPARATION) {
    return packet.moisturePercent;
  }
  return moistureFromRaw(packet.rawAdc);
}

bool validCalibration(uint16_t dryRaw, uint16_t wetRaw) {
  return dryRaw <= 4095 && wetRaw <= 4095 &&
         dryRaw > wetRaw + CALIBRATION_MIN_SEPARATION;
}

void loadCalibration() {
  preferences.begin("soilcal", true);
  const uint16_t dryRaw = preferences.getUShort("dry", 0);
  const uint16_t wetRaw = preferences.getUShort("wet", 0);
  preferences.end();
  if (validCalibration(dryRaw, wetRaw)) {
    calibrationDryRaw = dryRaw;
    calibrationWetRaw = wetRaw;
    calibrationFromUser = true;
  }
  Serial.printf("Calibration: dry=%u wet=%u (%s)\n", calibrationDryRaw,
                calibrationWetRaw,
                calibrationFromUser ? "user" : "firmware default");
}

bool saveCalibration(uint16_t dryRaw, uint16_t wetRaw) {
  if (!validCalibration(dryRaw, wetRaw)) return false;
  preferences.begin("soilcal", false);
  const size_t dryWritten = preferences.putUShort("dry", dryRaw);
  const size_t wetWritten = preferences.putUShort("wet", wetRaw);
  preferences.end();
  if (dryWritten != sizeof(uint16_t) || wetWritten != sizeof(uint16_t)) {
    return false;
  }
  calibrationDryRaw = dryRaw;
  calibrationWetRaw = wetRaw;
  calibrationFromUser = true;
  return true;
}

void textAt(int16_t x, int16_t y, const String &text, uint16_t color,
            uint8_t size = 1) {
  display->setTextSize(size);
  display->setTextColor(color);
  display->setCursor(x, y);
  display->print(text);
}

uint16_t moistureColor(uint8_t percent) {
  if (percent < 25) return C_RED;
  if (percent < 45) return C_YELLOW;
  return C_GREEN;
}

const char *moistureLabel(uint8_t percent) {
  if (percent < 25) return "DRY - WATER SOON";
  if (percent < 45) return "A LITTLE DRY";
  if (percent < 80) return "MOISTURE GOOD";
  return "VERY WET";
}

uint8_t rssiToPercent(int8_t rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -45) return 100;
  return static_cast<uint8_t>(map(rssi, -100, -45, 0, 100));
}

uint16_t linkColor(uint8_t percent) {
  if (percent < 30) return C_RED;
  if (percent < 60) return C_YELLOW;
  return C_GREEN;
}

uint32_t freshnessTimeoutMs(const SoilPacket &packet) {
  const uint32_t intervalSeconds = packet.nextSampleSeconds < 10
                                       ? 10U
                                       : packet.nextSampleSeconds;
  // A periodic sensor is fresh through its next expected sample plus a small
  // radio/jitter allowance. Do not imply a continuous connection for several
  // missed samples.
  const uint32_t graceSeconds = constrain(intervalSeconds / 5U, 30U, 120U);
  return (intervalSeconds + graceSeconds) * 1000UL;
}

bool validSampleInterval(uint16_t seconds) {
  return seconds == FAST_SAMPLE_SECONDS ||
         seconds == LOW_POWER_SAMPLE_SECONDS;
}

void loadRequestedSampleInterval() {
  preferences.begin("soilmode", false);
  const uint16_t storedVersion = preferences.getUShort("version", 0);
  uint16_t stored =
      preferences.getUShort("interval", LOW_POWER_SAMPLE_SECONDS);
  // Version 2 moves the default back to five-minute low-power sampling. The
  // 30-second cadence remains available from the dashboard toggle, but it is
  // a bench-test setting and should not survive a firmware update silently.
  if (storedVersion < MODE_SETTINGS_VERSION) {
    stored = LOW_POWER_SAMPLE_SECONDS;
    preferences.putUShort("interval", stored);
    preferences.putUShort("version", MODE_SETTINGS_VERSION);
  }
  preferences.end();
  requestedSampleSeconds =
      validSampleInterval(stored) ? stored : LOW_POWER_SAMPLE_SECONDS;
}

bool saveRequestedSampleInterval(uint16_t seconds) {
  if (!validSampleInterval(seconds)) return false;
  preferences.begin("soilmode", false);
  const size_t intervalWritten = preferences.putUShort("interval", seconds);
  const size_t versionWritten =
      preferences.putUShort("version", MODE_SETTINGS_VERSION);
  preferences.end();
  if (intervalWritten != sizeof(uint16_t) ||
      versionWritten != sizeof(uint16_t)) {
    return false;
  }
  portENTER_CRITICAL(&packetMux);
  requestedSampleSeconds = seconds;
  portEXIT_CRITICAL(&packetMux);
  return true;
}

String formatSensorAge(uint32_t ageMs) {
  const uint32_t seconds = ageMs / 1000UL;
  if (seconds < 60U) return String(seconds) + "s AGO";
  const uint32_t minutes = seconds / 60U;
  if (minutes < 60U) {
    return String(minutes) + "m " + String(seconds % 60U) + "s AGO";
  }
  const uint32_t hours = minutes / 60U;
  return String(hours) + "h " + String(minutes % 60U) + "m AGO";
}

void drawLastUpdate(bool havePacket, uint32_t ageMs, bool fresh) {
  display->fillRect(8, 264, 224, 16, C_BLACK);
  if (!havePacket) {
    textAt(12, 267, "LAST UPDATE: NEVER", C_YELLOW, 1);
    return;
  }
  textAt(12, 267, "LAST UPDATE: " + formatSensorAge(ageMs),
         fresh ? C_CYAN : C_RED, 1);
}

void drawStaticScreen() {
  display->fillScreen(C_BLACK);
  display->fillRect(0, 0, LCD_WIDTH, 37, C_DARK_BLUE);
  textAt(11, 8, "SOIL MONITOR", C_WHITE, 2);
  textAt(11, 46, "WIRELESS LINK", C_CYAN, 1);
  display->drawRoundRect(9, 76, 222, 113, 8, C_DARK_GREY);
  textAt(21, 198, "MOISTURE", C_WHITE, 1);
  display->drawRoundRect(19, 214, 202, 25, 5, C_WHITE);
  textAt(12, 250, "RAW", C_DARK_GREY, 1);
  textAt(112, 250, "SIGNAL", C_DARK_GREY, 1);
}

void drawWaiting() {
  display->fillRect(8, 58, 224, 16, C_BLACK);
  textAt(103, 46, "WAITING", C_YELLOW, 1);
  display->fillRect(10, 78, 220, 109, C_BLACK);
  textAt(43, 111, "Waiting for", C_WHITE, 2);
  textAt(35, 137, "sensor node...", C_YELLOW, 2);
  display->fillRect(20, 215, 200, 23, C_BLACK);
  display->fillRect(41, 247, 60, 13, C_BLACK);
  display->fillRect(153, 247, 80, 13, C_BLACK);
  textAt(153, 250, "OFFLINE", C_RED, 1);
  drawLastUpdate(false, 0, false);
}

void drawReading(const SoilPacket &packet, bool fresh, int8_t rssi,
                 uint32_t ageMs) {
  const bool sensorSignalValid =
      (packet.statusFlags & SOIL_STATUS_SENSOR_VALID) != 0 &&
      packet.rawAdc >= 200 && packet.rawAdc <= 4000;
  const uint8_t moisture = moistureFor(packet);
  const uint16_t color =
      sensorSignalValid ? moistureColor(moisture) : C_RED;

  display->fillRect(103, 45, 129, 15, C_BLACK);
  textAt(103, 46, fresh ? "FRESH" : "STALE", fresh ? C_GREEN : C_RED, 1);

  display->fillRect(10, 78, 220, 109, C_BLACK);
  const String percent =
      sensorSignalValid ? String(moisture) + "%" : "--";
  const int16_t percentX = 120 - static_cast<int16_t>(percent.length() * 18);
  textAt(percentX < 18 ? 18 : percentX, 91, percent, color, 6);
  const String label =
      sensorSignalValid ? moistureLabel(moisture) : "CHECK SENSOR WIRE";
  const int16_t centeredLabelX = (LCD_WIDTH - label.length() * 12) / 2;
  const int16_t labelX = centeredLabelX < 14 ? 14 : centeredLabelX;
  textAt(labelX, 165, label, color, 2);

  const int barWidth =
      sensorSignalValid ? map(moisture, 0, 100, 0, 196) : 0;
  display->fillRoundRect(22, 217, 196, 19, 3, C_BLACK);
  if (barWidth > 0) {
    display->fillRoundRect(22, 217, barWidth, 19, 3, color);
  }

  display->fillRect(41, 247, 67, 13, C_BLACK);
  textAt(41, 250, String(packet.rawAdc), C_WHITE, 1);
  display->fillRect(153, 247, 80, 13, C_BLACK);
  if (!fresh) {
    textAt(153, 250, "STALE", C_RED, 1);
  } else if (rssi <= -127) {
    textAt(153, 250, "LINKING", C_YELLOW, 1);
  } else {
    const uint8_t strength = rssiToPercent(rssi);
    const String signal = String(strength) + "% " + String(rssi);
    textAt(153, 250, signal, linkColor(strength), 1);
  }
  drawLastUpdate(true, ageMs, fresh);
}

void onPromiscuousPacket(void *buffer, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;

  const wifi_promiscuous_pkt_t *packet =
      static_cast<const wifi_promiscuous_pkt_t *>(buffer);
  if (packet->rx_ctrl.sig_len < 16) return;

  // ESP-NOW uses vendor-specific 802.11 action frames. Address 2 at byte 10
  // is the transmitter MAC address.
  //
  // Promiscuous capture sits below the ESP-NOW crypto layer, so this MAC match
  // is unauthenticated and anyone in radio range can spoof it. The value is
  // used only to draw a signal bar; never let it reach the data path.
  if (memcmp(packet->payload + 10, SENSOR_MAC, sizeof(SENSOR_MAC)) == 0) {
    portENTER_CRITICAL(&packetMux);
    latestRssi = packet->rx_ctrl.rssi;
    portEXIT_CRITICAL(&packetMux);
  }
}

void onDataReceived(const uint8_t *mac, const uint8_t *data, int length) {
  if (memcmp(mac, SENSOR_MAC, sizeof(SENSOR_MAC)) != 0 ||
      length != sizeof(SoilPacket)) {
    return;
  }

  SoilPacket incoming;
  memcpy(&incoming, data, sizeof(incoming));
  if (incoming.magic != SOIL_PACKET_MAGIC ||
      (incoming.version != SOIL_PACKET_LEGACY_VERSION &&
       incoming.version != SOIL_PACKET_VERSION) ||
      incoming.type != SoilMessageType::Reading ||
      incoming.length != sizeof(SoilPacket) ||
      incoming.moisturePercent > 100) {
    return;
  }

  portENTER_CRITICAL(&packetMux);
  if (receivedPacketCount > 0 && incoming.bootId == lastReceivedBootId &&
      incoming.sequence > lastReceivedSequence + 1) {
    missedPacketCount += incoming.sequence - lastReceivedSequence - 1;
  }
  lastReceivedBootId = incoming.bootId;
  lastReceivedSequence = incoming.sequence;
  ++receivedPacketCount;
  latestPacket = incoming;
  packetReceived = true;
  lastPacketAt = millis();
  const uint16_t acknowledgementLength =
      incoming.version == SOIL_PACKET_LEGACY_VERSION
          ? LEGACY_ACK_PACKET_SIZE
          : sizeof(SoilAckPacket);
  pendingAcknowledgement = {
      SOIL_PACKET_MAGIC,
      incoming.version,
      SoilMessageType::Acknowledgement,
      acknowledgementLength,
      incoming.bootId,
      incoming.sequence,
      requestedSampleSeconds,
  };
  pendingAcknowledgementLength = acknowledgementLength;
  acknowledgementPending = true;
  portEXIT_CRITICAL(&packetMux);
}

bool startEspNowReceiver() {
  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) return false;
  const uint8_t protocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                            WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR;
  if (esp_wifi_set_protocol(WIFI_IF_STA, protocols) != ESP_OK) {
    return false;
  }
  // Units are 0.25 dBm; 80 requests the ESP32-S3 maximum of 20 dBm.
  if (esp_wifi_set_max_tx_power(80) != ESP_OK) return false;
  wifi_second_chan_t secondary;
  if (esp_wifi_get_channel(&gatewayChannel, &secondary) != ESP_OK) return false;
  if (esp_now_init() != ESP_OK) return false;
  if (esp_now_set_pmk(ESPNOW_PMK) != ESP_OK) return false;

  esp_now_peer_info_t sensorPeer = {};
  memcpy(sensorPeer.peer_addr, SENSOR_MAC, sizeof(SENSOR_MAC));
  memcpy(sensorPeer.lmk, ESPNOW_LMK, ESP_NOW_KEY_LEN);
  sensorPeer.channel = 0;
  sensorPeer.ifidx = WIFI_IF_STA;
  sensorPeer.encrypt = true;
  if (esp_now_add_peer(&sensorPeer) != ESP_OK) return false;
  if (esp_now_register_recv_cb(onDataReceived) != ESP_OK) return false;

  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK) return false;
  if (esp_wifi_set_promiscuous_rx_cb(onPromiscuousPacket) != ESP_OK) return false;
  return esp_wifi_set_promiscuous(true) == ESP_OK;
}

const HistorySample &historyAt(size_t logicalIndex) {
  const size_t first = (historyHead + HISTORY_CAPACITY - historyCount) %
                       HISTORY_CAPACITY;
  return historySamples[(first + logicalIndex) % HISTORY_CAPACITY];
}

void addHistorySample(uint8_t moisture) {
  historySamples[historyHead] = {millis(), moisture};
  historyHead = (historyHead + 1) % HISTORY_CAPACITY;
  if (historyCount < HISTORY_CAPACITY) ++historyCount;
}

void calculateHistory(float &average, uint8_t &minimum, uint8_t &maximum,
                      float &delta, String &trend) {
  average = 0;
  minimum = 0;
  maximum = 0;
  delta = 0;
  trend = "--";
  if (historyCount == 0) return;

  uint32_t total = 0;
  minimum = 100;
  for (size_t i = 0; i < historyCount; ++i) {
    const uint8_t value = historyAt(i).moisture;
    total += value;
    minimum = min(minimum, value);
    maximum = max(maximum, value);
  }
  average = static_cast<float>(total) / historyCount;
  const size_t comparisonIndex = historyCount > 30 ? historyCount - 31 : 0;
  delta = static_cast<float>(historyAt(historyCount - 1).moisture) -
          historyAt(comparisonIndex).moisture;
  trend = delta > 2.0f ? "Rising" : (delta < -2.0f ? "Falling" : "Stable");
}

void handleApiData() {
  SoilPacket packet;
  bool havePacket;
  uint32_t receivedAt;
  int8_t rssi;
  uint32_t received;
  uint32_t missed;
  uint16_t requestedInterval;
  portENTER_CRITICAL(&packetMux);
  packet = latestPacket;
  havePacket = packetReceived;
  receivedAt = lastPacketAt;
  rssi = latestRssi;
  received = receivedPacketCount;
  missed = missedPacketCount;
  requestedInterval = requestedSampleSeconds;
  portEXIT_CRITICAL(&packetMux);

  const uint32_t age = havePacket ? millis() - receivedAt : UINT32_MAX;
  const bool fresh = havePacket && age < freshnessTimeoutMs(packet);
  const bool valid = havePacket &&
                     (packet.statusFlags & SOIL_STATUS_SENSOR_VALID) != 0 &&
                     packet.rawAdc >= 200 && packet.rawAdc <= 4000;
  const uint32_t totalPackets = received + missed;
  const float reliability = totalPackets > 0
                                ? received * 100.0f / totalPackets
                                : 0.0f;
  float average;
  float delta;
  uint8_t minimum;
  uint8_t maximum;
  String trend;
  calculateHistory(average, minimum, maximum, delta, trend);

  const uint8_t moisture = moistureFor(packet);

  String json;
  json.reserve(820);
  json += "{\"linked\":" + String(fresh ? "true" : "false");
  json += ",\"fresh\":" + String(fresh ? "true" : "false");
  json += ",\"valid\":" + String(valid ? "true" : "false");
  json += ",\"moisture\":" + String(moisture);
  json += ",\"condition\":\"" + String(moistureLabel(moisture)) + "\"";
  json += ",\"raw\":" + String(packet.rawAdc);
  json += ",\"millivolts\":" + String(packet.millivolts);
  json += ",\"rssi\":" + String(rssi);
  json += ",\"signal\":" + String(rssiToPercent(rssi));
  json += ",\"age_ms\":" + String(age);
  json += ",\"sequence\":" + String(packet.sequence);
  json += ",\"boot_id\":" + String(packet.bootId);
  json += ",\"next_sample_seconds\":" + String(packet.nextSampleSeconds);
  json += ",\"requested_sample_seconds\":" + String(requestedInterval);
  json += ",\"sampling_mode\":\"" +
          String(requestedInterval == LOW_POWER_SAMPLE_SECONDS
                     ? "low_power"
                     : "fast") + "\"";
  json += ",\"battery_millivolts\":" + String(packet.batteryMillivolts);
  json += ",\"received\":" + String(received);
  json += ",\"missed\":" + String(missed);
  json += ",\"reliability\":" + String(reliability, 1);
  json += ",\"samples\":" + String(historyCount);
  json += ",\"average\":" + String(average, 1);
  json += ",\"minimum\":" + String(minimum);
  json += ",\"maximum\":" + String(maximum);
  json += ",\"delta\":" + String(delta, 1);
  json += ",\"trend\":\"" + trend + "\"";
  json += ",\"calibration_dry\":" + String(calibrationDryRaw);
  json += ",\"calibration_wet\":" + String(calibrationWetRaw);
  json += ",\"calibrated\":" + String(calibrationFromUser ? "true" : "false");
  json += ",\"cloud_pending\":" +
          String(static_cast<unsigned long>(spoolPendingCount()));
  json += ",\"cloud_dropped\":" +
          String(static_cast<unsigned long>(spoolDroppedCount));
  json += ",\"cloud_buffer\":\"" + String(spoolReady ? "flash" : "ram") + "\"";
  json += ",\"cloud_online\":" +
          String(cloudUploadOnline ? "true" : "false");
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", json);
}

void handleApiHistory() {
  String json;
  json.reserve(64 + historyCount * 5);
  json = "{\"moisture\":[";
  for (size_t i = 0; i < historyCount; ++i) {
    if (i) json += ',';
    json += String(historyAt(i).moisture);
  }
  json += "]}";
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", json);
}

bool validCloudDeviceId(const String &value) {
  if (value.length() < 3 || value.length() > 64) return false;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (!((character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '_' ||
          character == '-')) {
      return false;
    }
  }
  return true;
}

bool cloudConfigured() {
  return cloudApiUrl.startsWith("https://") && cloudApiUrl.length() <= 220 &&
         validCloudDeviceId(cloudDeviceId) &&
         cloudIngestSecret.length() >= 32 &&
         cloudIngestSecret.length() <= 256;
}

bool isProvisioningVersionUpgrade(uint32_t desiredVersion,
                                  uint32_t appliedVersion) {
  return desiredVersion != 0 && appliedVersion < desiredVersion;
}

void seedLocalProvisioning() {
#if HAS_LOCAL_PROVISIONING
  const String localSsid = LOCAL_WIFI_SSID;
  if (!localSsid.isEmpty()) {
    const String localPassword = LOCAL_WIFI_PASSWORD;
    preferences.begin("soilwifi", false);
    const bool empty = preferences.getString("ssid", "").isEmpty();
    const uint32_t appliedVersion = preferences.getUInt("seed_ver", 0);
    const uint32_t desiredVersion = LOCAL_PROVISIONING_VERSION;
    const bool versionUpgrade =
        isProvisioningVersionUpgrade(desiredVersion, appliedVersion);
    if (empty || versionUpgrade) {
      preferences.putString("ssid", localSsid);
      preferences.putString("password", localPassword);
      if (desiredVersion != 0) {
        preferences.putUInt("seed_ver", desiredVersion);
      }
    }
    preferences.end();
    Serial.printf("Local Wi-Fi bootstrap: %s\n",
                  versionUpgrade ? "MIGRATED"
                                 : (empty ? "SEEDED" : "ALREADY SET"));
  }

  const String localApiUrl = LOCAL_CLOUD_API_URL;
  const String localDeviceId = LOCAL_CLOUD_DEVICE_ID;
  const String localIngestSecret = LOCAL_CLOUD_INGEST_SECRET;
  const bool anyCloudValue = !localApiUrl.isEmpty() ||
                             !localDeviceId.isEmpty() ||
                             !localIngestSecret.isEmpty();
  const bool validCloudValues = localApiUrl.startsWith("https://") &&
                                localApiUrl.length() <= 220 &&
                                validCloudDeviceId(localDeviceId) &&
                                localIngestSecret.length() >= 32 &&
                                localIngestSecret.length() <= 256;
  if (validCloudValues) {
    preferences.begin("soilcloud", false);
    const bool empty = preferences.getString("api", "").isEmpty() ||
                       preferences.getString("device", "").isEmpty() ||
                       preferences.getString("secret", "").isEmpty();
    const uint32_t appliedVersion = preferences.getUInt("seed_ver", 0);
    const uint32_t desiredVersion = LOCAL_PROVISIONING_VERSION;
    const bool versionUpgrade =
        isProvisioningVersionUpgrade(desiredVersion, appliedVersion);
    if (empty || versionUpgrade) {
      preferences.putString("api", localApiUrl);
      preferences.putString("device", localDeviceId);
      preferences.putString("secret", localIngestSecret);
      if (desiredVersion != 0) {
        preferences.putUInt("seed_ver", desiredVersion);
      }
    }
    preferences.end();
    Serial.printf("Local cloud bootstrap: %s\n",
                  versionUpgrade ? "MIGRATED"
                                 : (empty ? "SEEDED" : "ALREADY SET"));
  } else if (anyCloudValue) {
    Serial.println("Local cloud bootstrap is incomplete or invalid");
  }
#endif
}

void loadCloudConfig() {
  preferences.begin("soilcloud", true);
  cloudApiUrl = preferences.getString("api", "");
  cloudDeviceId = preferences.getString("device", "");
  cloudIngestSecret = preferences.getString("secret", "");
  preferences.end();
  Serial.printf("Cloud uploader configuration: %s\n",
                cloudConfigured() ? "READY" : "NOT SET");
}

void bytesToHex(const uint8_t *bytes, size_t length, char *output) {
  constexpr char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < length; ++index) {
    output[index * 2] = digits[bytes[index] >> 4];
    output[index * 2 + 1] = digits[bytes[index] & 0x0F];
  }
  output[length * 2] = '\0';
}

bool createCloudSignature(const String &canonical, String &signature) {
  const mbedtls_md_info_t *algorithm =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!algorithm) return false;

  uint8_t digest[32];
  if (mbedtls_md_hmac(
          algorithm,
          reinterpret_cast<const uint8_t *>(cloudIngestSecret.c_str()),
          cloudIngestSecret.length(),
          reinterpret_cast<const uint8_t *>(canonical.c_str()),
          canonical.length(), digest) != 0) {
    return false;
  }

  uint8_t encoded[48];
  size_t encodedLength = 0;
  if (mbedtls_base64_encode(encoded, sizeof(encoded), &encodedLength, digest,
                            sizeof(digest)) != 0) {
    return false;
  }
  signature = "";
  signature.reserve(encodedLength);
  for (size_t index = 0; index < encodedLength; ++index) {
    const char character = static_cast<char>(encoded[index]);
    if (character == '=') break;
    signature += character == '+' ? '-' : (character == '/' ? '_' : character);
  }
  return signature.length() == 43;
}

bool formatUtc(time_t timestamp, char (&output)[21]) {
  if (timestamp < 1700000000) return false;
  struct tm utcTime;
  if (!gmtime_r(&timestamp, &utcTime)) return false;
  return strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utcTime) ==
         20;
}

CloudUploadResult uploadToCloud(const CloudUploadRecord &record) {
  if (!cloudConfigured()) return CloudUploadResult::PermanentFailure;
  if (WiFi.status() != WL_CONNECTED) {
    return CloudUploadResult::RetryableFailure;
  }

  const time_t requestTime = time(nullptr);
  if (requestTime < 1700000000) {
    return CloudUploadResult::RetryableFailure;
  }
  const time_t sampleTime = record.sampledAt >= 1700000000
                                ? record.sampledAt
                                : requestTime;
  char sampledAt[21];
  if (!formatUtc(sampleTime, sampledAt)) {
    return CloudUploadResult::PermanentFailure;
  }

  char bootUuid[37];
  snprintf(bootUuid, sizeof(bootUuid),
           "00000000-0000-4000-8000-%012lx",
           static_cast<unsigned long>(record.packet.bootId));

  char batteryValue[12];
  if (record.packet.batteryMillivolts == 0) {
    snprintf(batteryValue, sizeof(batteryValue), "null");
  } else {
    snprintf(batteryValue, sizeof(batteryValue), "%u",
             record.packet.batteryMillivolts);
  }

  char body[560];
  const int bodyLength = snprintf(
      body, sizeof(body),
      "{\"schema\":1,\"sampled_at\":\"%s\",\"moisture_pct\":%u,"
      "\"raw_adc\":%u,\"sensor_mv\":%u,\"espnow_rssi_dbm\":%d,"
      "\"battery_mv\":%s,\"battery_percent\":null,"
      "\"uptime_seconds\":%lu,\"sensor_firmware\":\"%s\","
      "\"gateway_firmware\":\"%s\"}",
      sampledAt, moistureFor(record.packet), record.packet.rawAdc,
      record.packet.millivolts, record.rssi,
      batteryValue,
      static_cast<unsigned long>(record.packet.uptimeSeconds),
      SENSOR_FIRMWARE_VERSION, GATEWAY_FIRMWARE_VERSION);
  if (bodyLength <= 0 || bodyLength >= static_cast<int>(sizeof(body))) {
    return CloudUploadResult::PermanentFailure;
  }

  uint8_t bodyHash[32];
  if (mbedtls_sha256_ret(reinterpret_cast<const uint8_t *>(body), bodyLength,
                         bodyHash, 0) != 0) {
    return CloudUploadResult::RetryableFailure;
  }
  char bodyHashHex[65];
  bytesToHex(bodyHash, sizeof(bodyHash), bodyHashHex);

  const uint32_t requestTimestamp = static_cast<uint32_t>(requestTime);
  String canonical;
  canonical.reserve(180);
  canonical = "v1\n" + cloudDeviceId + "\n" + bootUuid + "\n" +
              String(record.packet.sequence) + "\n" +
              String(requestTimestamp) + "\n" + bodyHashHex;
  String signature;
  if (!createCloudSignature(canonical, signature)) {
    return CloudUploadResult::RetryableFailure;
  }

  WiFiClientSecure client;
  client.setCACert(TLS_ROOT_CA_BUNDLE);
  client.setHandshakeTimeout(8);
  HTTPClient request;
  request.setConnectTimeout(8000);
  request.setTimeout(8000);
  if (!request.begin(client, cloudApiUrl)) {
    return CloudUploadResult::RetryableFailure;
  }
  request.addHeader("Content-Type", "application/json");
  request.addHeader("X-Device-Id", cloudDeviceId);
  request.addHeader("X-Boot-Id", bootUuid);
  request.addHeader("X-Sequence", String(record.packet.sequence));
  request.addHeader("X-Timestamp", String(requestTimestamp));
  request.addHeader("X-Signature", signature);
  request.addHeader("User-Agent", "soil-gateway/2.0");
  const int status = request.POST(
      reinterpret_cast<uint8_t *>(body), bodyLength);
  request.end();
  cloudLastHttpStatus = status;
  if (status == HTTP_CODE_OK || status == HTTP_CODE_CREATED) {
    return CloudUploadResult::Succeeded;
  }
  if (status <= 0 || status == HTTP_CODE_REQUEST_TIMEOUT || status == 425 ||
      status == 429 || status >= 500) {
    return CloudUploadResult::RetryableFailure;
  }
  return CloudUploadResult::PermanentFailure;
}

void spoolPersistCursor() {
  File cursorFile = LittleFS.open(SPOOL_CURSOR_PATH, "w");
  if (!cursorFile) return;
  cursorFile.write(reinterpret_cast<const uint8_t *>(&spoolCursor),
                   sizeof(spoolCursor));
  cursorFile.close();
}

void spoolReset() {
  LittleFS.remove(SPOOL_PATH);
  spoolCursor = 0;
  spoolPersistCursor();
}

// Reclaims the already-uploaded prefix of the spool file. Writing to a
// temporary path first means a power cut during compaction leaves the original
// spool intact rather than a half-copied one.
void spoolCompactLocked() {
  File source = LittleFS.open(SPOOL_PATH, "r");
  if (!source) {
    spoolCursor = 0;
    spoolPersistCursor();
    return;
  }
  const size_t total = source.size();
  if (spoolCursor >= total) {
    source.close();
    spoolReset();
    return;
  }

  source.seek(spoolCursor);
  File destination = LittleFS.open("/spool.tmp", "w");
  if (!destination) {
    source.close();
    return;
  }
  uint8_t buffer[512];
  while (source.available()) {
    const size_t read = source.read(buffer, sizeof(buffer));
    if (read == 0) break;
    destination.write(buffer, read);
  }
  source.close();
  destination.close();
  LittleFS.remove(SPOOL_PATH);
  LittleFS.rename("/spool.tmp", SPOOL_PATH);
  spoolCursor = 0;
  spoolPersistCursor();
}

bool spoolBegin() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed; cloud buffer is RAM-only");
    return false;
  }
  spoolMutex = xSemaphoreCreateMutex();
  if (!spoolMutex) return false;

  File cursorFile = LittleFS.open(SPOOL_CURSOR_PATH, "r");
  if (cursorFile && cursorFile.size() == sizeof(spoolCursor)) {
    cursorFile.read(reinterpret_cast<uint8_t *>(&spoolCursor),
                    sizeof(spoolCursor));
  }
  if (cursorFile) cursorFile.close();

  File spoolFile = LittleFS.open(SPOOL_PATH, "r");
  const size_t total = spoolFile ? spoolFile.size() : 0;
  if (spoolFile) spoolFile.close();
  if (spoolCursor > total || (total % sizeof(SpoolRecord)) != 0) {
    Serial.println("Spool file inconsistent; starting a fresh buffer");
    spoolReset();
  }
  const size_t pending = (total - spoolCursor) / sizeof(SpoolRecord);
  Serial.printf("Cloud spool ready: %u reading(s) awaiting upload\n",
                static_cast<unsigned>(pending));
  return true;
}

bool spoolAppend(const CloudUploadRecord &record) {
  if (!spoolReady) return false;
  if (xSemaphoreTake(spoolMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

  SpoolRecord entry = {};
  entry.magic = SPOOL_RECORD_MAGIC;
  entry.packet = record.packet;
  entry.rssi = record.rssi;
  entry.sampledAt = static_cast<int64_t>(record.sampledAt);
  entry.crc = spoolRecordCrc(entry);

  File spoolFile = LittleFS.open(SPOOL_PATH, "a");
  if (!spoolFile) {
    xSemaphoreGive(spoolMutex);
    return false;
  }
  if (spoolFile.size() >= MAX_SPOOL_BYTES) {
    // The buffer is full, which means the network has been down for a very
    // long time. Drop the oldest reading rather than the newest: recent soil
    // moisture is what the dashboard and alerts actually need.
    spoolFile.close();
    spoolCursor += sizeof(SpoolRecord);
    ++spoolDroppedCount;
    spoolCompactLocked();
    spoolFile = LittleFS.open(SPOOL_PATH, "a");
    if (!spoolFile) {
      xSemaphoreGive(spoolMutex);
      return false;
    }
  }
  const size_t written =
      spoolFile.write(reinterpret_cast<const uint8_t *>(&entry),
                      sizeof(entry));
  spoolFile.close();
  xSemaphoreGive(spoolMutex);
  return written == sizeof(entry);
}

bool spoolPeek(CloudUploadRecord &record) {
  if (!spoolReady) return false;
  if (xSemaphoreTake(spoolMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

  File spoolFile = LittleFS.open(SPOOL_PATH, "r");
  if (!spoolFile) {
    xSemaphoreGive(spoolMutex);
    return false;
  }
  if (spoolCursor + sizeof(SpoolRecord) > spoolFile.size()) {
    spoolFile.close();
    xSemaphoreGive(spoolMutex);
    return false;
  }
  SpoolRecord entry = {};
  spoolFile.seek(spoolCursor);
  const size_t read =
      spoolFile.read(reinterpret_cast<uint8_t *>(&entry), sizeof(entry));
  spoolFile.close();

  if (read != sizeof(entry) || entry.magic != SPOOL_RECORD_MAGIC ||
      entry.crc != spoolRecordCrc(entry)) {
    Serial.println("Corrupt spool record discarded");
    spoolReset();
    xSemaphoreGive(spoolMutex);
    return false;
  }

  record.packet = entry.packet;
  record.rssi = entry.rssi;
  record.sampledAt = static_cast<time_t>(entry.sampledAt);
  xSemaphoreGive(spoolMutex);
  return true;
}

void spoolAdvance() {
  if (!spoolReady) return;
  if (xSemaphoreTake(spoolMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
  spoolCursor += sizeof(SpoolRecord);
  File spoolFile = LittleFS.open(SPOOL_PATH, "r");
  const size_t total = spoolFile ? spoolFile.size() : 0;
  if (spoolFile) spoolFile.close();
  if (spoolCursor >= total) {
    spoolReset();
  } else if (spoolCursor >= SPOOL_COMPACT_THRESHOLD) {
    spoolCompactLocked();
  } else {
    spoolPersistCursor();
  }
  xSemaphoreGive(spoolMutex);
}

size_t spoolPendingCount() {
  if (!spoolReady) {
    return cloudUploadQueue ? uxQueueMessagesWaiting(cloudUploadQueue) : 0;
  }
  File spoolFile = LittleFS.open(SPOOL_PATH, "r");
  const size_t total = spoolFile ? spoolFile.size() : 0;
  if (spoolFile) spoolFile.close();
  if (total <= spoolCursor) return 0;
  return (total - spoolCursor) / sizeof(SpoolRecord);
}

bool pendingStoreTake(CloudUploadRecord &record) {
  if (spoolReady) return spoolPeek(record);
  if (!cloudUploadQueue) return false;
  return xQueueReceive(cloudUploadQueue, &record, 0) == pdTRUE;
}

void pendingStoreRelease() {
  // The RAM fallback already removed the item when it was taken.
  if (spoolReady) spoolAdvance();
}

void cloudUploaderTask(void *) {
  esp_task_wdt_add(nullptr);
  CloudUploadRecord record;
  uint32_t retryDelayMs = 5000;

  for (;;) {
    watchdogFeed();
    if (!pendingStoreTake(record)) {
      retryDelayMs = 5000;
      watchdogDelay(2000);
      continue;
    }

    const CloudUploadResult result = uploadToCloud(record);
    if (result == CloudUploadResult::Succeeded) {
      pendingStoreRelease();
      cloudUploadOnline = true;
      retryDelayMs = 5000;
      continue;
    }

    if (result == CloudUploadResult::PermanentFailure) {
      // The server will never accept this row, so retrying forever would
      // block every reading behind it. Drop exactly this one and move on.
      Serial.printf(
          "Cloud rejected sequence %lu permanently (HTTP %d); discarding\n",
          static_cast<unsigned long>(record.packet.sequence),
          cloudLastHttpStatus);
      pendingStoreRelease();
      retryDelayMs = 5000;
      continue;
    }

    // Retryable: leave the record at the head of the spool and wait. Nothing
    // is lost, and readings taken during the outage queue up behind it.
    cloudUploadOnline = false;
    watchdogDelay(retryDelayMs);
    retryDelayMs = retryDelayMs >= 150000UL ? 300000UL : retryDelayMs * 2;
  }
}

void queueCloudUpload(const SoilPacket &packet, int8_t rssi) {
  const CloudUploadRecord record = {packet, rssi, time(nullptr)};
  if (spoolAppend(record)) return;
  if (!cloudUploadQueue) return;
  if (xQueueSend(cloudUploadQueue, &record, 0) == pdTRUE) return;

  CloudUploadRecord discarded;
  xQueueReceive(cloudUploadQueue, &discarded, 0);
  ++spoolDroppedCount;
  xQueueSend(cloudUploadQueue, &record, 0);
}

void startCloudUploader() {
  if (!cloudConfigured()) return;
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");
  spoolReady = spoolBegin();
  if (!spoolReady) {
    cloudUploadQueue = xQueueCreate(12, sizeof(CloudUploadRecord));
    if (!cloudUploadQueue) {
      Serial.println("Cloud upload queue allocation failed");
      return;
    }
  }
  if (xTaskCreatePinnedToCore(cloudUploaderTask, "soil-cloud", 8192, nullptr,
                              1, nullptr, 0) != pdPASS) {
    if (cloudUploadQueue) {
      vQueueDelete(cloudUploadQueue);
      cloudUploadQueue = nullptr;
    }
    Serial.println("Cloud uploader task creation failed");
  }
}

// State-changing local routes are same-origin only. A custom header cannot be
// set by a plain cross-site form post, and any browser that does set one must
// first pass a CORS preflight that this server never answers. That closes the
// drive-by case; anything already on the LAN is inside the trust boundary,
// which the README now states explicitly rather than leaving implied.
bool localControlRequestAllowed() {
  if (webServer.header("X-Soil-Control") != "local-dashboard") return false;
  if (!webServer.header("Content-Type").startsWith("application/json")) {
    return false;
  }
  const String origin = webServer.header("Origin");
  if (origin.length() == 0) return true;
  const String expectedIp = "http://" + WiFi.localIP().toString();
  return origin == "http://soil-monitor.local" || origin == expectedIp;
}

void configureWebRoutes() {
  webServer.on("/", HTTP_GET, []() {
    webServer.send_P(200, "text/html", setupPortalActive
                                           ? WIFI_SETUP_HTML
                                           : DASHBOARD_HTML);
  });
  webServer.on("/wifi", HTTP_GET, []() {
    if (!setupPortalActive) {
      webServer.send(403, "text/plain",
                     "Wi-Fi changes require setup mode. Hold BOOT while "
                     "resetting the display gateway.");
      return;
    }
    webServer.send_P(200, "text/html", WIFI_SETUP_HTML);
  });
  webServer.on("/save-wifi", HTTP_POST, []() {
    if (!setupPortalActive) {
      webServer.send(403, "text/plain", "Setup mode is not active");
      return;
    }
    const String ssid = webServer.arg("ssid");
    const String password = webServer.arg("password");
    const String apiUrl = webServer.arg("cloud_api_url");
    const String deviceId = webServer.arg("cloud_device_id");
    const String ingestSecret = webServer.arg("cloud_ingest_secret");
    if (ssid.isEmpty()) {
      webServer.send(400, "text/plain", "Wi-Fi name is required");
      return;
    }
    const bool anyCloudValue = !apiUrl.isEmpty() || !deviceId.isEmpty() ||
                               !ingestSecret.isEmpty();
    if (anyCloudValue &&
        (!apiUrl.startsWith("https://") || apiUrl.length() > 220 ||
         !validCloudDeviceId(deviceId) || ingestSecret.length() < 32 ||
         ingestSecret.length() > 256)) {
      webServer.send(400, "text/plain",
                     "Cloud settings are incomplete or invalid");
      return;
    }
    const String requestedOtaPassword = webServer.arg("ota_password");
    if (!requestedOtaPassword.isEmpty() &&
        (requestedOtaPassword.length() < 12 ||
         requestedOtaPassword.length() > 64)) {
      webServer.send(400, "text/plain",
                     "The OTA password must be 12 to 64 characters, or blank");
      return;
    }
    preferences.begin("soilwifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();
    if (anyCloudValue) {
      preferences.begin("soilcloud", false);
      preferences.putString("api", apiUrl);
      preferences.putString("device", deviceId);
      preferences.putString("secret", ingestSecret);
      preferences.end();
    }
    const String otaPassword = webServer.arg("ota_password");
    if (otaPassword.length() >= 12 && otaPassword.length() <= 64) {
      preferences.begin("soilota", false);
      preferences.putString("pass", otaPassword);
      preferences.end();
    }
    webServer.send(200, "text/html",
                   "<h2>Saved</h2><p>The Soil Monitor is restarting. Rejoin "
                   "your home Wi-Fi, then open <b>http://soil-monitor.local</b>.</p>");
    scheduledRestartAt = millis() + 1200;
  });
  webServer.on("/api/data", HTTP_GET, handleApiData);
  webServer.on("/api/history", HTTP_GET, handleApiHistory);
  webServer.on("/api/calibration", HTTP_POST, []() {
    if (setupPortalActive) {
      webServer.send(403, "application/json", "{\"error\":\"setup_mode\"}");
      return;
    }
    if (!localControlRequestAllowed()) {
      webServer.send(403, "application/json",
                     "{\"error\":\"invalid_control_request\"}");
      return;
    }
    // WebServer only exposes a JSON body as the "plain" argument, so the two
    // integers are pulled out directly rather than through arg("dry").
    const String body = webServer.arg("plain");
    unsigned int dryRaw = 0;
    unsigned int wetRaw = 0;
    if (body.length() > 64 ||
        sscanf(body.c_str(), "{\"dry\":%u,\"wet\":%u}", &dryRaw, &wetRaw) != 2 ||
        dryRaw > 4095 || wetRaw > 4095 ||
        !validCalibration(static_cast<uint16_t>(dryRaw),
                          static_cast<uint16_t>(wetRaw))) {
      webServer.send(400, "application/json",
                     "{\"error\":\"invalid_calibration\"}");
      return;
    }
    if (!saveCalibration(static_cast<uint16_t>(dryRaw),
                         static_cast<uint16_t>(wetRaw))) {
      webServer.send(500, "application/json", "{\"error\":\"save_failed\"}");
      return;
    }
    lastRenderedSequence = UINT32_MAX; // Force a redraw with the new scale.
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json",
                   "{\"ok\":true,\"dry\":" + String(calibrationDryRaw) +
                       ",\"wet\":" + String(calibrationWetRaw) + "}");
  });
  webServer.on("/api/mode", HTTP_POST, []() {
    if (setupPortalActive) {
      webServer.send(403, "application/json",
                     "{\"error\":\"setup_mode\"}");
      return;
    }
    if (!localControlRequestAllowed()) {
      webServer.send(403, "application/json",
                     "{\"error\":\"invalid_control_request\"}");
      return;
    }
    const String body = webServer.arg("plain");
    uint16_t interval = 0;
    if (body == "{\"mode\":\"fast\"}") {
      interval = FAST_SAMPLE_SECONDS;
    } else if (body == "{\"mode\":\"low_power\"}") {
      interval = LOW_POWER_SAMPLE_SECONDS;
    } else {
      webServer.send(400, "application/json",
                     "{\"error\":\"invalid_mode\"}");
      return;
    }
    if (!saveRequestedSampleInterval(interval)) {
      webServer.send(500, "application/json",
                     "{\"error\":\"save_failed\"}");
      return;
    }
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json",
                   "{\"ok\":true,\"requested_sample_seconds\":" +
                       String(interval) + "}");
  });
  webServer.onNotFound([]() {
    if (setupPortalActive) {
      webServer.send_P(200, "text/html", WIFI_SETUP_HTML);
    } else {
      webServer.send(404, "text/plain", "Not found");
    }
  });
}

bool connectToHomeWifi() {
  preferences.begin("soilwifi", true);
  const String ssid = preferences.getString("ssid", "");
  const String password = preferences.getString("password", "");
  preferences.end();
  if (ssid.isEmpty()) return false;

  Serial.printf("Connecting gateway to Wi-Fi: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("soil-monitor");
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 20000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

void startSetupPortal() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP("SoilMonitor-Setup", SETUP_AP_PASSWORD, 1)) {
    Serial.println("Unable to start secured setup access point");
    return;
  }
  setupPortalActive = true;
  setupPortalStartedAt = millis();
  dnsServer.start(53, "*", WiFi.softAPIP());
  configureWebRoutes();
  webServer.begin();
  webServerStarted = true;
  drawStaticScreen();
  drawWaiting();
  Serial.printf("Setup AP ready at http://%s\n",
                WiFi.softAPIP().toString().c_str());
}

// Opt-in network firmware updates. The gateway lives on a shelf indoors, but
// pulling it off Wi-Fi and onto a USB cable for every change is the reason
// small fixes never get deployed. Disabled unless a password is provisioned:
// an unauthenticated OTA listener would be a remote code execution hole.
void startOverTheAirUpdates() {
  preferences.begin("soilota", true);
  const String otaPassword = preferences.getString("pass", "");
  preferences.end();
  if (otaPassword.length() < 12) {
    Serial.println("OTA disabled (set a 12+ character password to enable)");
    return;
  }

  ArduinoOTA.setHostname("soil-monitor");
  ArduinoOTA.setPassword(otaPassword.c_str());
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    display->fillScreen(C_BLACK);
    textAt(28, 130, "UPDATING", C_CYAN, 3);
    Serial.println("OTA update started");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    // Flashing takes far longer than the watchdog period, and the transfer is
    // a legitimate long-running operation rather than a hang.
    watchdogFeed();
    if (total == 0) return;
    const int percent = static_cast<int>((done * 100ULL) / total);
    display->fillRect(28, 170, 190, 20, C_BLACK);
    textAt(28, 170, String(percent) + "%", C_WHITE, 2);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA update complete; restarting");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("OTA failed with error %u\n", error);
    drawStaticScreen();
    lastRenderedSequence = UINT32_MAX;
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready at soil-monitor.local");
}

void startDashboardServer() {
  setupPortalActive = false;
  configureWebRoutes();
  webServer.begin();
  webServerStarted = true;
  if (MDNS.begin("soil-monitor")) {
    MDNS.addService("http", "tcp", 80);
  }
  Serial.printf("Dashboard: http://soil-monitor.local (%s)\n",
                WiFi.localIP().toString().c_str());
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\nLAFVIN wireless soil display");

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  if (!display->begin()) {
    Serial.println("Display initialization failed");
    return;
  }
  drawStaticScreen();
  drawWaiting();

  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  seedLocalProvisioning();
  loadRequestedSampleInterval();
  loadCalibration();
  const char *controlHeaders[] = {"X-Soil-Control", "Content-Type", "Origin"};
  webServer.collectHeaders(controlHeaders, 3);
  const bool forceSetup = digitalRead(BOOT_BUTTON) == LOW;
  const bool homeWifiConnected = !forceSetup && connectToHomeWifi();
  loadCloudConfig();
  if (!homeWifiConnected) {
    startSetupPortal();
  } else {
    Serial.println("Home Wi-Fi connected");
  }

  const bool radioReady = startEspNowReceiver();
  Serial.printf("ESP-NOW receiver: %s, channel %u\n",
                radioReady ? "READY" : "FAILED", gatewayChannel);
  Serial.printf("Display MAC: %s\n", WiFi.macAddress().c_str());
  if (homeWifiConnected) {
    startDashboardServer();
    startCloudUploader();
    startOverTheAirUpdates();
  }
  if (!radioReady) {
    display->fillRect(8, 58, 224, 16, C_BLACK);
    textAt(8, 60, "RADIO START FAILED", C_RED, 2);
  }

  // Armed last so the long blocking steps above — a 20 second Wi-Fi join, an
  // NTP sync, a LittleFS format on first boot — cannot trip it during startup.
  esp_task_wdt_config_t watchdogConfig = {
      .timeout_ms = WATCHDOG_TIMEOUT_SECONDS * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&watchdogConfig) != ESP_OK) {
    esp_task_wdt_init(&watchdogConfig);
  }
  esp_task_wdt_add(nullptr);
  Serial.printf("Watchdog armed at %lu seconds\n",
                static_cast<unsigned long>(WATCHDOG_TIMEOUT_SECONDS));
}

void loop() {
  watchdogFeed();
  ArduinoOTA.handle();
  if (otaInProgress) {
    // Nothing else may touch the display, the radio, or flash while an image
    // is being written.
    delay(10);
    return;
  }
  if (webServerStarted) webServer.handleClient();
  if (setupPortalActive) dnsServer.processNextRequest();
  if (setupPortalActive &&
      millis() - setupPortalStartedAt >= SETUP_PORTAL_TIMEOUT_MS) {
    dnsServer.stop();
    webServer.stop();
    WiFi.softAPdisconnect(true);
    setupPortalActive = false;
    webServerStarted = false;
    drawStaticScreen();
    Serial.println("Setup portal timed out; reboot to open it again");
  }
  // Unsigned subtraction so the comparison stays correct across the 49.7-day
  // millis() rollover, matching every other timing check in this file.
  if (scheduledRestartAt &&
      static_cast<int32_t>(millis() - scheduledRestartAt) >= 0) {
    ESP.restart();
  }

  if (!setupPortalActive && WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiReconnectAt >= 30000) {
    lastWifiReconnectAt = millis();
    WiFi.reconnect();
  }

  // Backstop for a wedged receiver: the watchdog only catches a stalled task,
  // not an ESP-NOW stack that has quietly stopped delivering callbacks.
  if (!setupPortalActive && millis() > SILENT_RADIO_REBOOT_MS) {
    const uint32_t silentFor =
        packetReceived ? millis() - lastPacketAt : millis();
    if (silentFor >= SILENT_RADIO_REBOOT_MS) {
      Serial.println("No sensor packet in six hours; restarting gateway");
      Serial.flush();
      ESP.restart();
    }
  }

  bool sendAcknowledgement = false;
  SoilAckPacket acknowledgement;
  uint16_t acknowledgementLength = sizeof(SoilAckPacket);
  portENTER_CRITICAL(&packetMux);
  if (acknowledgementPending) {
    acknowledgement = pendingAcknowledgement;
    acknowledgementLength = pendingAcknowledgementLength;
    acknowledgementPending = false;
    sendAcknowledgement = true;
  }
  portEXIT_CRITICAL(&packetMux);
  if (sendAcknowledgement) {
    esp_now_send(SENSOR_MAC,
                 reinterpret_cast<const uint8_t *>(&acknowledgement),
                 acknowledgementLength);
  }

  SoilPacket packet;
  bool havePacket;
  uint32_t receivedAt;
  int8_t rssi;
  portENTER_CRITICAL(&packetMux);
  packet = latestPacket;
  havePacket = packetReceived;
  receivedAt = lastPacketAt;
  rssi = latestRssi;
  portEXIT_CRITICAL(&packetMux);

  const uint32_t age = havePacket ? millis() - receivedAt : UINT32_MAX;
  const bool fresh = havePacket && age < freshnessTimeoutMs(packet);
  if (havePacket &&
      (packet.sequence != lastRenderedSequence || fresh != lastLinkState)) {
    const bool isNewReading = packet.sequence != lastRenderedSequence;
    drawReading(packet, fresh, rssi, age);
    if (isNewReading) queueCloudUpload(packet, rssi);
    lastRenderedSequence = packet.sequence;
    lastLinkState = fresh;
    Serial.printf("Moisture %u%% | raw %u | %u mV | RSSI %d dBm | packet %lu\n",
                  packet.moisturePercent, packet.rawAdc, packet.millivolts,
                  rssi,
                  static_cast<unsigned long>(packet.sequence));
    if ((packet.statusFlags & SOIL_STATUS_SENSOR_VALID) != 0 &&
        packet.rawAdc >= 200 && packet.rawAdc <= 4000 &&
        (historyCount == 0 || millis() - lastHistoryAt >= 60000)) {
      addHistorySample(moistureFor(packet));
      lastHistoryAt = millis();
    }
  }
  if (millis() - lastFooterRefreshAt >= 1000) {
    lastFooterRefreshAt = millis();
    drawLastUpdate(havePacket, age, fresh);
  }
  if (millis() - lastStatusLogAt >= 5000) {
    lastStatusLogAt = millis();
    Serial.printf("Gateway mode=%s channel=%u link=%s packets=%lu requested=%us cloud=%s http=%d queued=%u dropped=%lu address=%s\n",
                  setupPortalActive ? "SETUP" : "HOME", gatewayChannel,
                  fresh ? "FRESH" : "STALE",
                  static_cast<unsigned long>(receivedPacketCount),
                  requestedSampleSeconds,
                  !cloudConfigured()
                      ? "NOT_SET"
                      : (cloudUploadOnline ? "ONLINE" : "WAITING"),
                  cloudLastHttpStatus,
                  static_cast<unsigned>(spoolPendingCount()),
                  static_cast<unsigned long>(spoolDroppedCount),
                  setupPortalActive ? "192.168.4.1"
                                    : WiFi.localIP().toString().c_str());
  }
  delay(100);
}

#elif defined(DEVICE_ROLE_SENSOR)

namespace {

#ifndef SENSOR_PIN
#error "SENSOR_PIN must be the actual analog GPIO for this board"
#endif

#ifndef SENSOR_POWER_PIN
#define SENSOR_POWER_PIN -1
#endif

#ifndef BATTERY_SENSE_PIN
#define BATTERY_SENSE_PIN -1
#endif

// Ratio of the external divider feeding BATTERY_SENSE_PIN. The default assumes
// two equal resistors, so the pin sees half the pack voltage.
#ifndef BATTERY_DIVIDER_NUMERATOR
#define BATTERY_DIVIDER_NUMERATOR 2
#endif

#ifndef BATTERY_DIVIDER_DENOMINATOR
#define BATTERY_DIVIDER_DENOMINATOR 1
#endif

#ifndef SENSOR_DEBUG
#define SENSOR_DEBUG 0
#endif

#ifndef MEASUREMENT_INTERVAL_SECONDS
#define MEASUREMENT_INTERVAL_SECONDS 30
#endif

// Dry-in-air endpoint measured from this sensor at 3.3 V (2511-2515 ADC).
// Replace WET_RAW after measuring fully wetted, drained soil of the target type.
constexpr int DRY_RAW = 2513;
constexpr int WET_RAW = 1300;
constexpr uint32_t RETAINED_STATE_MAGIC = 0x53524E32;
constexpr uint32_t SENSOR_SETTLE_MS = 450;
constexpr uint32_t ACK_TIMEOUT_MS = 500;
constexpr uint8_t MAX_CACHED_CHANNEL_ATTEMPTS = 3;
constexpr uint16_t SENSOR_FAST_SAMPLE_SECONDS = 30;
constexpr uint16_t SENSOR_LOW_POWER_SAMPLE_SECONDS = 300;
constexpr uint8_t GATEWAY_MAC[] = {0xAC, 0xA7, 0x04, 0x2A, 0x19, 0xCC};

RTC_DATA_ATTR uint32_t retainedStateMagic = 0;
RTC_DATA_ATTR uint32_t retainedBootId = 0;
RTC_DATA_ATTR uint32_t retainedSequence = 0;
RTC_DATA_ATTR uint8_t retainedChannel = 1;
RTC_DATA_ATTR uint16_t retainedSampleIntervalSeconds =
    MEASUREMENT_INTERVAL_SECONDS;

volatile bool sendResultReady = false;
volatile bool sendSucceeded = false;
volatile bool applicationAckReceived = false;
volatile uint16_t acknowledgedSampleIntervalSeconds = 0;
uint32_t expectedAckBootId = 0;
uint32_t expectedAckSequence = 0;
portMUX_TYPE deliveryMux = portMUX_INITIALIZER_UNLOCKED;

struct FilteredReading {
  uint16_t raw;
  uint16_t millivolts;
};

template <size_t N>
uint16_t trimmedMean(uint16_t (&values)[N]) {
  static_assert(N >= 9, "Trimmed mean needs enough samples");
  std::sort(values, values + N);
  constexpr size_t trim = 3;
  uint32_t total = 0;
  for (size_t i = trim; i < N - trim; ++i) {
    total += values[i];
  }
  return static_cast<uint16_t>(total / (N - trim * 2));
}

FilteredReading readSensor() {
#if SENSOR_POWER_PIN >= 0
  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, HIGH);
#endif
  delay(SENSOR_SETTLE_MS);

  pinMode(SENSOR_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);
  (void)analogRead(SENSOR_PIN); // Discard the first conversion after power-up.

  constexpr size_t sampleCount = 17;
  uint16_t rawSamples[sampleCount];
  uint16_t millivoltSamples[sampleCount];
  for (size_t i = 0; i < sampleCount; ++i) {
    rawSamples[i] = analogRead(SENSOR_PIN);
    millivoltSamples[i] = analogReadMilliVolts(SENSOR_PIN);
    delay(3);
  }

  const FilteredReading reading = {
      trimmedMean(rawSamples),
      trimmedMean(millivoltSamples),
  };

#if SENSOR_POWER_PIN >= 0
  pinMode(SENSOR_PIN, INPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);
#endif
  return reading;
}

uint8_t rawToPercent(uint16_t raw) {
  const long percent = map(raw, DRY_RAW, WET_RAW, 0, 100);
  return static_cast<uint8_t>(constrain(percent, 0L, 100L));
}

// Reads the battery through an external divider. Returns 0 when no divider is
// wired, which the gateway and the API both already treat as "unknown".
//
// The XIAO ESP32-S3 has no on-board battery divider, so this stays disabled
// until two resistors are fitted. Set BATTERY_SENSE_PIN plus the divider ratio
// in platformio.ini to turn it on. Without it there is no warning before the
// garden node goes silent, which is the failure mode hardest to notice.
uint16_t readBatteryMillivolts() {
#if BATTERY_SENSE_PIN >= 0
  pinMode(BATTERY_SENSE_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_SENSE_PIN, ADC_11db);
  (void)analogRead(BATTERY_SENSE_PIN);

  constexpr size_t sampleCount = 9;
  uint16_t samples[sampleCount];
  for (size_t i = 0; i < sampleCount; ++i) {
    samples[i] = analogReadMilliVolts(BATTERY_SENSE_PIN);
    delay(2);
  }
  std::sort(samples, samples + sampleCount);
  const uint32_t atPin = samples[sampleCount / 2];
  const uint32_t scaled =
      atPin * BATTERY_DIVIDER_NUMERATOR / BATTERY_DIVIDER_DENOMINATOR;
  // The schema only accepts 2500-5500 mV; anything outside that is a wiring
  // fault, and reporting nothing is better than reporting a fiction.
  if (scaled < 2500 || scaled > 5500) return 0;
  return static_cast<uint16_t>(scaled);
#else
  return 0;
#endif
}

void onPacketSent(const uint8_t *mac, esp_now_send_status_t status) {
  (void)mac;
  portENTER_CRITICAL(&deliveryMux);
  sendSucceeded = status == ESP_NOW_SEND_SUCCESS;
  sendResultReady = true;
  portEXIT_CRITICAL(&deliveryMux);
}

void onAcknowledgementReceived(const uint8_t *mac, const uint8_t *data,
                               int length) {
  if (memcmp(mac, GATEWAY_MAC, sizeof(GATEWAY_MAC)) != 0 ||
      length != sizeof(SoilAckPacket)) {
    return;
  }

  SoilAckPacket acknowledgement;
  memcpy(&acknowledgement, data, sizeof(acknowledgement));
  if (acknowledgement.magic != SOIL_PACKET_MAGIC ||
      acknowledgement.version != SOIL_PACKET_VERSION ||
      acknowledgement.type != SoilMessageType::Acknowledgement ||
      acknowledgement.length != sizeof(SoilAckPacket) ||
      acknowledgement.bootId != expectedAckBootId ||
      acknowledgement.sequence != expectedAckSequence ||
      (acknowledgement.requestedSampleSeconds !=
           SENSOR_FAST_SAMPLE_SECONDS &&
       acknowledgement.requestedSampleSeconds !=
           SENSOR_LOW_POWER_SAMPLE_SECONDS)) {
    return;
  }

  portENTER_CRITICAL(&deliveryMux);
  acknowledgedSampleIntervalSeconds =
      acknowledgement.requestedSampleSeconds;
  applicationAckReceived = true;
  portEXIT_CRITICAL(&deliveryMux);
}

void selectChannel(uint8_t channel) {
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

bool startEspNowTransmitter() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) return false;
  if (esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR) != ESP_OK) {
    return false;
  }
  if (esp_wifi_set_max_tx_power(80) != ESP_OK) return false;
  selectChannel(retainedChannel);
  if (esp_now_init() != ESP_OK) return false;
  if (esp_now_set_pmk(ESPNOW_PMK) != ESP_OK) return false;
  if (esp_now_register_send_cb(onPacketSent) != ESP_OK) return false;
  if (esp_now_register_recv_cb(onAcknowledgementReceived) != ESP_OK) {
    return false;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, GATEWAY_MAC, sizeof(GATEWAY_MAC));
  memcpy(peer.lmk, ESPNOW_LMK, ESP_NOW_KEY_LEN);
  peer.channel = 0; // Follow the transmitter's currently selected channel.
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = true;
  return esp_now_add_peer(&peer) == ESP_OK;
}

bool transmitOnChannel(const SoilPacket &packet, uint8_t channel) {
  selectChannel(channel);
  portENTER_CRITICAL(&deliveryMux);
  sendResultReady = false;
  sendSucceeded = false;
  applicationAckReceived = false;
  acknowledgedSampleIntervalSeconds = 0;
  expectedAckBootId = packet.bootId;
  expectedAckSequence = packet.sequence;
  portEXIT_CRITICAL(&deliveryMux);

  const esp_err_t queued =
      esp_now_send(GATEWAY_MAC,
                   reinterpret_cast<const uint8_t *>(&packet),
                   sizeof(packet));
  if (queued != ESP_OK) return false;

  const uint32_t startedAt = millis();
  while (millis() - startedAt < ACK_TIMEOUT_MS) {
    bool callbackReady;
    bool macDelivered;
    bool acknowledged;
    portENTER_CRITICAL(&deliveryMux);
    callbackReady = sendResultReady;
    macDelivered = sendSucceeded;
    acknowledged = applicationAckReceived;
    portEXIT_CRITICAL(&deliveryMux);
    if (acknowledged) return true;
    if (callbackReady && !macDelivered) return false;
    delay(5);
  }
  return false;
}

bool deliverReading(const SoilPacket &packet) {
  for (uint8_t attempt = 0; attempt < MAX_CACHED_CHANNEL_ATTEMPTS;
       ++attempt) {
    if (transmitOnChannel(packet, retainedChannel)) return true;
    delay(20 + (esp_random() % 45));
  }

  for (uint8_t channel = 1; channel <= 11; ++channel) {
    if (channel == retainedChannel) continue;
#if SENSOR_DEBUG
    Serial.printf("Scanning channel %u\n", channel);
#endif
    if (transmitOnChannel(packet, channel)) {
      retainedChannel = channel;
      return true;
    }
  }
  return false;
}

void enterDeepSleep(uint16_t intervalSeconds) {
  esp_now_deinit();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  pinMode(21, OUTPUT); // XIAO user LED is active-low.
  digitalWrite(21, HIGH);
  esp_sleep_enable_timer_wakeup(
      static_cast<uint64_t>(intervalSeconds) * 1000000ULL);
#if SENSOR_DEBUG
  Serial.printf("Sleeping for %u seconds\n", intervalSeconds);
  Serial.flush();
#endif
  esp_deep_sleep_start();
}

} // namespace

void setup() {
#if SENSOR_DEBUG
  Serial.begin(115200);
  delay(150);
  Serial.println("\nESP32-S3 soil sensor transmitter");
  Serial.printf("Sensor pin: GPIO%d | calibration dry=%d wet=%d\n",
                SENSOR_PIN, DRY_RAW, WET_RAW);
#endif

  if (retainedStateMagic != RETAINED_STATE_MAGIC || retainedBootId == 0) {
    retainedStateMagic = RETAINED_STATE_MAGIC;
    retainedBootId = esp_random();
    if (retainedBootId == 0) retainedBootId = 1;
    retainedSequence = 0;
    retainedChannel = 1;
    retainedSampleIntervalSeconds = MEASUREMENT_INTERVAL_SECONDS;
  }
  if (retainedSampleIntervalSeconds != SENSOR_FAST_SAMPLE_SECONDS &&
      retainedSampleIntervalSeconds != SENSOR_LOW_POWER_SAMPLE_SECONDS) {
    retainedSampleIntervalSeconds = MEASUREMENT_INTERVAL_SECONDS;
  }

  const FilteredReading reading = readSensor();
  SoilPacket packet = {};
  packet.magic = SOIL_PACKET_MAGIC;
  packet.version = SOIL_PACKET_VERSION;
  packet.type = SoilMessageType::Reading;
  packet.length = sizeof(SoilPacket);
  packet.bootId = retainedBootId;
  packet.sequence = ++retainedSequence;
  packet.uptimeSeconds = millis() / 1000;
  packet.rawAdc = reading.raw;
  packet.millivolts = reading.millivolts;
  packet.batteryMillivolts = readBatteryMillivolts();
  if (packet.batteryMillivolts != 0) {
    packet.statusFlags |= SOIL_STATUS_BATTERY_AVAILABLE;
  }
  packet.nextSampleSeconds = retainedSampleIntervalSeconds;
  packet.moisturePercent = rawToPercent(reading.raw);
  if (reading.raw >= 200 && reading.raw <= 4000) {
    packet.statusFlags |= SOIL_STATUS_SENSOR_VALID;
  }

  const bool radioReady = startEspNowTransmitter();
#if SENSOR_DEBUG
  Serial.printf("ESP-NOW transmitter: %s; scanning home Wi-Fi channels\n",
                radioReady ? "READY" : "FAILED");
  Serial.printf("Sensor MAC: %s\n", WiFi.macAddress().c_str());
#endif

  const bool delivered = radioReady && deliverReading(packet);
  if (delivered) {
    portENTER_CRITICAL(&deliveryMux);
    const uint16_t requestedInterval = acknowledgedSampleIntervalSeconds;
    portEXIT_CRITICAL(&deliveryMux);
    if (requestedInterval == SENSOR_FAST_SAMPLE_SECONDS ||
        requestedInterval == SENSOR_LOW_POWER_SAMPLE_SECONDS) {
      retainedSampleIntervalSeconds = requestedInterval;
    }
  }
#if SENSOR_DEBUG
  Serial.printf("Moisture %u%% | raw %u | %u mV | channel %u | packet %lu | %s\n",
                packet.moisturePercent, packet.rawAdc, packet.millivolts,
                retainedChannel, static_cast<unsigned long>(packet.sequence),
                delivered ? "ACKNOWLEDGED" : "NO ACK");
#else
  (void)delivered;
#endif
  enterDeepSleep(retainedSampleIntervalSeconds);
}

void loop() {}

#else
#error "Choose display_receiver or sensor_transmitter environment"
#endif
