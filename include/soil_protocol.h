#pragma once

#include <Arduino.h>

constexpr uint32_t SOIL_PACKET_MAGIC = 0x534F494C; // "SOIL"
constexpr uint8_t SOIL_PACKET_LEGACY_VERSION = 2;
constexpr uint8_t SOIL_PACKET_VERSION = 3;

enum class SoilMessageType : uint8_t {
  Reading = 1,
  Acknowledgement = 2,
};

enum SoilStatusFlag : uint8_t {
  SOIL_STATUS_SENSOR_VALID = 1U << 0,
  SOIL_STATUS_BATTERY_AVAILABLE = 1U << 1,
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
};

struct __attribute__((packed)) SoilAckPacket {
  uint32_t magic;
  uint8_t version;
  SoilMessageType type;
  uint16_t length;
  uint32_t bootId;
  uint32_t sequence;
  uint16_t requestedSampleSeconds;
};

static_assert(sizeof(SoilPacket) == 30, "Unexpected SoilPacket size");
static_assert(sizeof(SoilAckPacket) == 18, "Unexpected SoilAckPacket size");
