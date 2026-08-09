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

#include <Arduino_GFX_Library.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
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
constexpr char GATEWAY_FIRMWARE_VERSION[] = "2.0.0";
constexpr char SENSOR_FIRMWARE_VERSION[] = "2.0.0";

constexpr uint16_t C_BLACK = 0x0000;
constexpr uint16_t C_WHITE = 0xFFFF;
constexpr uint16_t C_RED = 0xF800;
constexpr uint16_t C_GREEN = 0x07E0;
constexpr uint16_t C_YELLOW = 0xFFE0;
constexpr uint16_t C_CYAN = 0x07FF;
constexpr uint16_t C_DARK_BLUE = 0x018C;
constexpr uint16_t C_DARK_GREY = 0x4208;

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

constexpr size_t HISTORY_CAPACITY = 288; // 24 hours at five-minute intervals.
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
uint32_t scheduledRestartAt = 0;
uint8_t gatewayChannel = 0;
uint32_t lastStatusLogAt = 0;
uint32_t setupPortalStartedAt = 0;
uint32_t lastWifiReconnectAt = 0;
constexpr uint32_t SETUP_PORTAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;

volatile bool acknowledgementPending = false;
SoilAckPacket pendingAcknowledgement = {};

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

uint32_t linkTimeoutMs(const SoilPacket &packet) {
  const uint32_t advertised = packet.nextSampleSeconds < 10
                                  ? 10U
                                  : packet.nextSampleSeconds;
  return advertised * 3000UL + 15000UL;
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
  textAt(12, 267,
         setupPortalActive ? "SETUP: 192.168.4.1" : "soil-monitor.local",
         setupPortalActive ? C_YELLOW : C_CYAN, 1);
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
}

void drawReading(const SoilPacket &packet, bool linked, int8_t rssi) {
  const bool sensorSignalValid =
      (packet.statusFlags & SOIL_STATUS_SENSOR_VALID) != 0 &&
      packet.rawAdc >= 200 && packet.rawAdc <= 4000;
  const uint16_t color = sensorSignalValid
                             ? moistureColor(packet.moisturePercent)
                             : C_RED;

  display->fillRect(103, 45, 129, 15, C_BLACK);
  textAt(103, 46, linked ? "CONNECTED" : "OFFLINE", linked ? C_GREEN : C_RED, 1);

  display->fillRect(10, 78, 220, 109, C_BLACK);
  const String percent = sensorSignalValid
                             ? String(packet.moisturePercent) + "%"
                             : "--";
  const int16_t percentX = 120 - static_cast<int16_t>(percent.length() * 18);
  textAt(percentX < 18 ? 18 : percentX, 91, percent, color, 6);
  const String label = sensorSignalValid
                           ? moistureLabel(packet.moisturePercent)
                           : "CHECK SENSOR WIRE";
  const int16_t centeredLabelX = (LCD_WIDTH - label.length() * 12) / 2;
  const int16_t labelX = centeredLabelX < 14 ? 14 : centeredLabelX;
  textAt(labelX, 165, label, color, 2);

  const int barWidth = sensorSignalValid
                           ? map(packet.moisturePercent, 0, 100, 0, 196)
                           : 0;
  display->fillRoundRect(22, 217, 196, 19, 3, C_BLACK);
  if (barWidth > 0) {
    display->fillRoundRect(22, 217, barWidth, 19, 3, color);
  }

  display->fillRect(41, 247, 67, 13, C_BLACK);
  textAt(41, 250, String(packet.rawAdc), C_WHITE, 1);
  display->fillRect(153, 247, 80, 13, C_BLACK);
  if (!linked) {
    textAt(153, 250, "LOST", C_RED, 1);
  } else if (rssi <= -127) {
    textAt(153, 250, "LINKING", C_YELLOW, 1);
  } else {
    const uint8_t strength = rssiToPercent(rssi);
    const String signal = String(strength) + "% " + String(rssi);
    textAt(153, 250, signal, linkColor(strength), 1);
  }
}

void onPromiscuousPacket(void *buffer, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;

  const wifi_promiscuous_pkt_t *packet =
      static_cast<const wifi_promiscuous_pkt_t *>(buffer);
  if (packet->rx_ctrl.sig_len < 16) return;

  // ESP-NOW uses vendor-specific 802.11 action frames. Address 2 at byte 10
  // is the transmitter MAC address.
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
      incoming.version != SOIL_PACKET_VERSION ||
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
  pendingAcknowledgement = {
      SOIL_PACKET_MAGIC,
      SOIL_PACKET_VERSION,
      SoilMessageType::Acknowledgement,
      sizeof(SoilAckPacket),
      incoming.bootId,
      incoming.sequence,
  };
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
  portENTER_CRITICAL(&packetMux);
  packet = latestPacket;
  havePacket = packetReceived;
  receivedAt = lastPacketAt;
  rssi = latestRssi;
  received = receivedPacketCount;
  missed = missedPacketCount;
  portEXIT_CRITICAL(&packetMux);

  const uint32_t age = havePacket ? millis() - receivedAt : UINT32_MAX;
  const bool linked = havePacket && age < linkTimeoutMs(packet);
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

  String json;
  json.reserve(620);
  json += "{\"linked\":" + String(linked ? "true" : "false");
  json += ",\"valid\":" + String(valid ? "true" : "false");
  json += ",\"moisture\":" + String(packet.moisturePercent);
  json += ",\"condition\":\"" + String(moistureLabel(packet.moisturePercent)) + "\"";
  json += ",\"raw\":" + String(packet.rawAdc);
  json += ",\"millivolts\":" + String(packet.millivolts);
  json += ",\"rssi\":" + String(rssi);
  json += ",\"signal\":" + String(rssiToPercent(rssi));
  json += ",\"age_ms\":" + String(age);
  json += ",\"sequence\":" + String(packet.sequence);
  json += ",\"boot_id\":" + String(packet.bootId);
  json += ",\"next_sample_seconds\":" + String(packet.nextSampleSeconds);
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
      sampledAt, record.packet.moisturePercent, record.packet.rawAdc,
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

void cloudUploaderTask(void *) {
  CloudUploadRecord record;
  for (;;) {
    if (xQueueReceive(cloudUploadQueue, &record, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    CloudUploadResult result = CloudUploadResult::RetryableFailure;
    uint32_t retryDelayMs = 5000;
    for (uint8_t attempt = 0; attempt < MAX_CLOUD_UPLOAD_ATTEMPTS;
         ++attempt) {
      result = uploadToCloud(record);
      if (result != CloudUploadResult::RetryableFailure) break;
      cloudUploadOnline = false;
      if (attempt + 1 >= MAX_CLOUD_UPLOAD_ATTEMPTS) break;
      vTaskDelay(pdMS_TO_TICKS(retryDelayMs));
      retryDelayMs = retryDelayMs >= 150000UL ? 300000UL
                                              : retryDelayMs * 2;
    }
    cloudUploadOnline = result == CloudUploadResult::Succeeded;
    if (!cloudUploadOnline) {
      Serial.printf("Cloud upload dropped sequence %lu after %s failure (HTTP %d)\n",
                    static_cast<unsigned long>(record.packet.sequence),
                    result == CloudUploadResult::PermanentFailure
                        ? "permanent"
                        : "retryable",
                    cloudLastHttpStatus);
    }
  }
}

void queueCloudUpload(const SoilPacket &packet, int8_t rssi) {
  if (!cloudUploadQueue) return;
  const CloudUploadRecord record = {packet, rssi, time(nullptr)};
  if (xQueueSend(cloudUploadQueue, &record, 0) == pdTRUE) return;

  CloudUploadRecord discarded;
  xQueueReceive(cloudUploadQueue, &discarded, 0);
  xQueueSend(cloudUploadQueue, &record, 0);
}

void startCloudUploader() {
  if (!cloudConfigured()) return;
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");
  cloudUploadQueue = xQueueCreate(12, sizeof(CloudUploadRecord));
  if (!cloudUploadQueue) {
    Serial.println("Cloud upload queue allocation failed");
    return;
  }
  if (xTaskCreatePinnedToCore(cloudUploaderTask, "soil-cloud", 8192, nullptr,
                              1, nullptr, 0) != pdPASS) {
    vQueueDelete(cloudUploadQueue);
    cloudUploadQueue = nullptr;
    Serial.println("Cloud uploader task creation failed");
  }
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
    webServer.send(200, "text/html",
                   "<h2>Saved</h2><p>The Soil Monitor is restarting. Rejoin "
                   "your home Wi-Fi, then open <b>http://soil-monitor.local</b>.</p>");
    scheduledRestartAt = millis() + 1200;
  });
  webServer.on("/api/data", HTTP_GET, handleApiData);
  webServer.on("/api/history", HTTP_GET, handleApiHistory);
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
  }
  if (!radioReady) {
    display->fillRect(8, 58, 224, 16, C_BLACK);
    textAt(8, 60, "RADIO START FAILED", C_RED, 2);
  }
}

void loop() {
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
  if (scheduledRestartAt && millis() >= scheduledRestartAt) ESP.restart();

  if (!setupPortalActive && WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiReconnectAt >= 30000) {
    lastWifiReconnectAt = millis();
    WiFi.reconnect();
  }

  bool sendAcknowledgement = false;
  SoilAckPacket acknowledgement;
  portENTER_CRITICAL(&packetMux);
  if (acknowledgementPending) {
    acknowledgement = pendingAcknowledgement;
    acknowledgementPending = false;
    sendAcknowledgement = true;
  }
  portEXIT_CRITICAL(&packetMux);
  if (sendAcknowledgement) {
    esp_now_send(SENSOR_MAC,
                 reinterpret_cast<const uint8_t *>(&acknowledgement),
                 sizeof(acknowledgement));
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

  const bool linked = havePacket &&
                      (millis() - receivedAt < linkTimeoutMs(packet));
  if (havePacket &&
      (packet.sequence != lastRenderedSequence || linked != lastLinkState)) {
    const bool isNewReading = packet.sequence != lastRenderedSequence;
    drawReading(packet, linked, rssi);
    if (isNewReading) queueCloudUpload(packet, rssi);
    lastRenderedSequence = packet.sequence;
    lastLinkState = linked;
    Serial.printf("Moisture %u%% | raw %u | %u mV | RSSI %d dBm | packet %lu\n",
                  packet.moisturePercent, packet.rawAdc, packet.millivolts,
                  rssi,
                  static_cast<unsigned long>(packet.sequence));
    if ((packet.statusFlags & SOIL_STATUS_SENSOR_VALID) != 0 &&
        packet.rawAdc >= 200 && packet.rawAdc <= 4000 &&
        (historyCount == 0 || millis() - lastHistoryAt >= 60000)) {
      addHistorySample(packet.moisturePercent);
      lastHistoryAt = millis();
    }
  }
  if (millis() - lastStatusLogAt >= 5000) {
    lastStatusLogAt = millis();
    Serial.printf("Gateway mode=%s channel=%u link=%s packets=%lu cloud=%s http=%d address=%s\n",
                  setupPortalActive ? "SETUP" : "HOME", gatewayChannel,
                  linked ? "LIVE" : "WAITING",
                  static_cast<unsigned long>(receivedPacketCount),
                  !cloudConfigured()
                      ? "NOT_SET"
                      : (cloudUploadOnline ? "ONLINE" : "WAITING"),
                  cloudLastHttpStatus,
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

#ifndef SENSOR_DEBUG
#define SENSOR_DEBUG 0
#endif

#ifndef MEASUREMENT_INTERVAL_SECONDS
#define MEASUREMENT_INTERVAL_SECONDS 300
#endif

// Adjust these after placing the sensor in completely dry soil and in water.
// Most v1.2 capacitive sensors powered from 3.3 V fall near these values.
constexpr int DRY_RAW = 3000;
constexpr int WET_RAW = 1300;
constexpr uint32_t RETAINED_STATE_MAGIC = 0x53524E32;
constexpr uint32_t SENSOR_SETTLE_MS = 450;
constexpr uint32_t ACK_TIMEOUT_MS = 500;
constexpr uint8_t MAX_CACHED_CHANNEL_ATTEMPTS = 3;
constexpr uint8_t GATEWAY_MAC[] = {0xAC, 0xA7, 0x04, 0x2A, 0x19, 0xCC};

RTC_DATA_ATTR uint32_t retainedStateMagic = 0;
RTC_DATA_ATTR uint32_t retainedBootId = 0;
RTC_DATA_ATTR uint32_t retainedSequence = 0;
RTC_DATA_ATTR uint8_t retainedChannel = 1;

volatile bool sendResultReady = false;
volatile bool sendSucceeded = false;
volatile bool applicationAckReceived = false;
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
      acknowledgement.sequence != expectedAckSequence) {
    return;
  }

  portENTER_CRITICAL(&deliveryMux);
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

void enterDeepSleep() {
  esp_now_deinit();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  pinMode(21, OUTPUT); // XIAO user LED is active-low.
  digitalWrite(21, HIGH);
  esp_sleep_enable_timer_wakeup(
      static_cast<uint64_t>(MEASUREMENT_INTERVAL_SECONDS) * 1000000ULL);
#if SENSOR_DEBUG
  Serial.printf("Sleeping for %u seconds\n", MEASUREMENT_INTERVAL_SECONDS);
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
  packet.batteryMillivolts = 0; // Requires an external divider; see README.
  packet.nextSampleSeconds = MEASUREMENT_INTERVAL_SECONDS;
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
#if SENSOR_DEBUG
  Serial.printf("Moisture %u%% | raw %u | %u mV | channel %u | packet %lu | %s\n",
                packet.moisturePercent, packet.rawAdc, packet.millivolts,
                retainedChannel, static_cast<unsigned long>(packet.sequence),
                delivered ? "ACKNOWLEDGED" : "NO ACK");
#else
  (void)delivered;
#endif
  enterDeepSleep();
}

void loop() {}

#else
#error "Choose display_receiver or sensor_transmitter environment"
#endif
