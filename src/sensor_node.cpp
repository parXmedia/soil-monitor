#include <Arduino.h>
#include <WiFi.h>
#include <algorithm>
#include <esp_now.h>
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

#if defined(DEVICE_ROLE_SENSOR)

namespace {

#ifndef SENSOR_PIN
#error "SENSOR_PIN must be the analog GPIO connected to the soil probe"
#endif

#ifndef SENSOR_POWER_PIN
#define SENSOR_POWER_PIN -1
#endif

#ifndef MEASUREMENT_INTERVAL_SECONDS
#define MEASUREMENT_INTERVAL_SECONDS 2
#endif

#ifndef SENSOR_FIRMWARE_BUILD
#define SENSOR_FIRMWARE_BUILD 0x00040000
#endif

static_assert(MEASUREMENT_INTERVAL_SECONDS > 0,
              "MEASUREMENT_INTERVAL_SECONDS must be greater than zero");

// These endpoints only provide a useful fallback value in the radio packet.
// The display gateway applies its own user calibration to the raw ADC reading.
constexpr uint16_t DRY_RAW = 2513;
constexpr uint16_t WET_RAW = 1300;
constexpr uint16_t INSTANT_INTERVAL_SECONDS = 2;
constexpr uint16_t FIVE_MINUTE_INTERVAL_SECONDS = 300;
constexpr uint32_t SENSOR_SETTLE_MS = 500;
constexpr uint32_t SEND_TIMEOUT_MS = 400;
constexpr uint8_t CHANNEL_MIN = 1;
constexpr uint8_t CHANNEL_MAX = 11;
constexpr uint8_t GATEWAY_MAC[] = {0xAC, 0xA7, 0x04, 0x2A, 0x19, 0xCC};

struct SensorReading {
  uint16_t raw;
  uint16_t millivolts;
};

uint32_t bootId = 0;
uint32_t sequence = 0;
uint32_t lastMeasurementAt = 0;
uint8_t radioChannel = CHANNEL_MIN;
bool radioReady = false;
volatile uint16_t sampleIntervalSeconds = MEASUREMENT_INTERVAL_SECONDS;
volatile SoilSamplingMode samplingMode = SoilSamplingMode::Live;
volatile bool modeChangePending = false;

volatile bool sendFinished = false;
volatile bool sendDelivered = false;
portMUX_TYPE sendMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE modeMux = portMUX_INITIALIZER_UNLOCKED;

template <size_t N>
uint16_t median(uint16_t (&samples)[N]) {
  static_assert(N % 2 == 1, "Median sample count must be odd");
  std::sort(samples, samples + N);
  return samples[N / 2];
}

uint8_t moisturePercent(uint16_t raw) {
  const long percent = map(raw, DRY_RAW, WET_RAW, 0, 100);
  return static_cast<uint8_t>(constrain(percent, 0L, 100L));
}

void enableProbe() {
#if SENSOR_POWER_PIN >= 0
  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, HIGH);
#endif
  pinMode(SENSOR_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);
  delay(SENSOR_SETTLE_MS);
}

SensorReading readProbe() {
  constexpr size_t sampleCount = 9;
  uint16_t rawSamples[sampleCount];
  uint16_t millivoltSamples[sampleCount];

  (void)analogRead(SENSOR_PIN); // Discard the first ADC conversion.
  for (size_t index = 0; index < sampleCount; ++index) {
    rawSamples[index] = analogRead(SENSOR_PIN);
    millivoltSamples[index] = analogReadMilliVolts(SENSOR_PIN);
    delay(5);
  }

  return {median(rawSamples), median(millivoltSamples)};
}

void onPacketSent(const uint8_t *mac, esp_now_send_status_t status) {
  (void)mac;
  portENTER_CRITICAL(&sendMux);
  sendDelivered = status == ESP_NOW_SEND_SUCCESS;
  sendFinished = true;
  portEXIT_CRITICAL(&sendMux);
}

void onAcknowledgementReceived(const uint8_t *mac, const uint8_t *data,
                               int length) {
  if (memcmp(mac, GATEWAY_MAC, sizeof(GATEWAY_MAC)) != 0 ||
      length != sizeof(SoilAckPacket)) {
    return;
  }

  SoilAckPacket acknowledgement;
  memcpy(&acknowledgement, data, sizeof(acknowledgement));
  const bool instant =
      acknowledgement.requestedSampleSeconds == INSTANT_INTERVAL_SECONDS &&
      acknowledgement.requestedMode == SoilSamplingMode::Live;
  const bool fiveMinute =
      acknowledgement.requestedSampleSeconds == FIVE_MINUTE_INTERVAL_SECONDS &&
      acknowledgement.requestedMode == SoilSamplingMode::LowPower;
  if (acknowledgement.magic != SOIL_PACKET_MAGIC ||
      acknowledgement.version != SOIL_PACKET_VERSION ||
      acknowledgement.type != SoilMessageType::Acknowledgement ||
      acknowledgement.length != sizeof(SoilAckPacket) ||
      acknowledgement.bootId != bootId ||
      acknowledgement.sequence != sequence || (!instant && !fiveMinute)) {
    return;
  }

  portENTER_CRITICAL(&modeMux);
  if (sampleIntervalSeconds != acknowledgement.requestedSampleSeconds ||
      samplingMode != acknowledgement.requestedMode) {
    sampleIntervalSeconds = acknowledgement.requestedSampleSeconds;
    samplingMode = acknowledgement.requestedMode;
    modeChangePending = true;
  }
  portEXIT_CRITICAL(&modeMux);
}

void selectChannel(uint8_t channel) {
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

bool startRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK ||
      esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR) != ESP_OK ||
      esp_wifi_set_max_tx_power(80) != ESP_OK) {
    return false;
  }

  selectChannel(radioChannel);
  if (esp_now_init() != ESP_OK || esp_now_set_pmk(ESPNOW_PMK) != ESP_OK ||
      esp_now_register_send_cb(onPacketSent) != ESP_OK ||
      esp_now_register_recv_cb(onAcknowledgementReceived) != ESP_OK) {
    return false;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, GATEWAY_MAC, sizeof(GATEWAY_MAC));
  memcpy(peer.lmk, ESPNOW_LMK, ESP_NOW_KEY_LEN);
  peer.channel = 0; // Use whichever channel selectChannel() chose.
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = true;
  return esp_now_add_peer(&peer) == ESP_OK;
}

bool sendOnChannel(const SoilPacket &packet, uint8_t channel) {
  selectChannel(channel);

  portENTER_CRITICAL(&sendMux);
  sendFinished = false;
  sendDelivered = false;
  portEXIT_CRITICAL(&sendMux);

  if (esp_now_send(GATEWAY_MAC,
                   reinterpret_cast<const uint8_t *>(&packet),
                   sizeof(packet)) != ESP_OK) {
    return false;
  }

  const uint32_t startedAt = millis();
  while (millis() - startedAt < SEND_TIMEOUT_MS) {
    bool finished;
    bool delivered;
    portENTER_CRITICAL(&sendMux);
    finished = sendFinished;
    delivered = sendDelivered;
    portEXIT_CRITICAL(&sendMux);
    if (finished) return delivered;
    delay(2);
  }
  return false;
}

bool sendReading(const SoilPacket &packet) {
  // First retry the last working channel. If the gateway moved to a different
  // Wi-Fi channel, scan the remaining 2.4 GHz channels and remember the match.
  if (sendOnChannel(packet, radioChannel)) return true;

  for (uint8_t channel = CHANNEL_MIN; channel <= CHANNEL_MAX; ++channel) {
    if (channel == radioChannel) continue;
    if (sendOnChannel(packet, channel)) {
      radioChannel = channel;
      return true;
    }
  }
  return false;
}

void takeAndSendMeasurement() {
  const SensorReading reading = readProbe();
  uint16_t interval;
  SoilSamplingMode mode;
  portENTER_CRITICAL(&modeMux);
  interval = sampleIntervalSeconds;
  mode = samplingMode;
  portEXIT_CRITICAL(&modeMux);

  SoilPacket packet = {};
  packet.magic = SOIL_PACKET_MAGIC;
  packet.version = SOIL_PACKET_VERSION;
  packet.type = SoilMessageType::Reading;
  packet.length = sizeof(packet);
  packet.bootId = bootId;
  packet.sequence = ++sequence;
  packet.uptimeSeconds = millis() / 1000;
  packet.rawAdc = reading.raw;
  packet.millivolts = reading.millivolts;
  packet.nextSampleSeconds = interval;
  packet.moisturePercent = moisturePercent(reading.raw);
  packet.firmwareBuild = SENSOR_FIRMWARE_BUILD;
  packet.samplingMode = mode;
  if (reading.raw >= 200 && reading.raw <= 4000) {
    packet.statusFlags |= SOIL_STATUS_SENSOR_VALID;
  }

  if (!radioReady) {
    // A transient startup failure should not require a manual reset. Retry the
    // small radio setup on the next measurement while keeping the node awake.
    esp_now_deinit();
    radioReady = startRadio();
  }
  const bool delivered = radioReady && sendReading(packet);
  if (!delivered && radioReady) {
    // Rebuild ESP-NOW on the next pass if its internal state became unhealthy.
    esp_now_deinit();
    radioReady = false;
  }
  Serial.printf(
      "reading=%lu raw=%u voltage=%umV moisture=%u%% interval=%us channel=%u radio=%s\n",
      static_cast<unsigned long>(packet.sequence), packet.rawAdc,
      packet.millivolts, packet.moisturePercent, interval, radioChannel,
      delivered ? "delivered" : "not delivered");
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\nSoil sensor starting (always awake; deep sleep disabled)");

  bootId = esp_random();
  if (bootId == 0) bootId = 1;

  enableProbe();
  radioReady = startRadio();
  Serial.printf("sensor GPIO=%d MAC=%s radio=%s interval=%us\n", SENSOR_PIN,
                WiFi.macAddress().c_str(), radioReady ? "ready" : "failed",
                MEASUREMENT_INTERVAL_SECONDS);

  takeAndSendMeasurement();
  lastMeasurementAt = millis();
}

void loop() {
  uint16_t interval;
  bool modeChanged;
  portENTER_CRITICAL(&modeMux);
  interval = sampleIntervalSeconds;
  modeChanged = modeChangePending;
  modeChangePending = false;
  portEXIT_CRITICAL(&modeMux);
  if (modeChanged) {
    lastMeasurementAt = millis();
    takeAndSendMeasurement();
    delay(10);
    return;
  }
  const uint32_t intervalMs = interval * 1000UL;
  if (millis() - lastMeasurementAt >= intervalMs) {
    lastMeasurementAt = millis();
    takeAndSendMeasurement();
  }

  // A short delay yields to the Wi-Fi task. The CPU and radio remain awake.
  delay(10);
}

#endif
