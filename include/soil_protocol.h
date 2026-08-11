#pragma once

#include <Arduino.h>

constexpr uint32_t SOIL_PACKET_MAGIC = 0x534F494C; // "SOIL"
constexpr uint8_t SOIL_PACKET_LEGACY_VERSION = 2;
constexpr uint8_t SOIL_PACKET_INTERVAL_VERSION = 3;
constexpr uint8_t SOIL_PACKET_VERSION = 4;

enum class SoilSamplingMode : uint8_t {
  Live = 0,
  Fast = 1,
  LowPower = 2,
};

enum class SoilMessageType : uint8_t {
  Reading = 1,
  Acknowledgement = 2,
};

enum SoilStatusFlag : uint8_t {
  SOIL_STATUS_SENSOR_VALID = 1U << 0,
  SOIL_STATUS_BATTERY_AVAILABLE = 1U << 1,
  SOIL_STATUS_POWER_AVAILABLE = 1U << 2,
  SOIL_STATUS_POWER_MEASURED = 1U << 3,
};

enum SoilCommandFlag : uint8_t {
  SOIL_COMMAND_NONE = 0,
  SOIL_COMMAND_SENSOR_UPDATE = 1U << 0,
};

// Protocol v2/v3 wire layout. The gateway keeps accepting this packet so the
// display can be upgraded before a sleeping sensor checks in with v4.
struct __attribute__((packed)) LegacySoilPacket {
  uint32_t magic;
  uint8_t version;
  SoilMessageType type;
  uint16_t length;
  uint32_t bootId;
  uint32_t sequence;
  uint32_t uptimeSeconds;
  uint16_t rawAdc;
  uint16_t millivolts;
  uint16_t batteryMillivolts;
  uint16_t nextSampleSeconds;
  uint8_t moisturePercent;
  uint8_t statusFlags;
};

struct __attribute__((packed)) SoilPacket {
  uint32_t magic;
  uint8_t version;
  SoilMessageType type;
  uint16_t length;
  uint32_t bootId;
  uint32_t sequence;
  uint32_t uptimeSeconds;
  uint16_t rawAdc;
  uint16_t millivolts;
  uint16_t batteryMillivolts;
  uint16_t nextSampleSeconds;
  uint8_t moisturePercent;
  uint8_t statusFlags;
  uint16_t currentMilliamps;
  uint16_t powerMilliwatts;
  uint32_t firmwareBuild;
  SoilSamplingMode samplingMode;
  uint8_t reserved;
};

struct __attribute__((packed)) SoilAckPacket {
  uint32_t magic;
  uint8_t version;
  SoilMessageType type;
  uint16_t length;
  uint32_t bootId;
  uint32_t sequence;
  uint16_t requestedSampleSeconds;
  SoilSamplingMode requestedMode;
  uint8_t commandFlags;
  uint16_t awakeWindowMs;
  uint32_t firmwareSize;
  uint32_t firmwareBuild;
  uint32_t updateNonce;
  uint8_t firmwareSha256[32];
};

static_assert(sizeof(LegacySoilPacket) == 30, "Unexpected legacy packet size");
static_assert(sizeof(SoilPacket) == 40, "Unexpected SoilPacket size");
static_assert(sizeof(SoilAckPacket) == 66, "Unexpected SoilAckPacket size");
