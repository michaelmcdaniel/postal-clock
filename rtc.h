#pragma once

#include <Arduino.h>
#include <Wire.h>

struct ClockDateTime {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t weekday = 0;  // 0=Sunday
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

class RtcService {
 public:
  bool begin();
  bool read(ClockDateTime& value);
  bool write(const ClockDateTime& value);
  bool oscillatorStopped();
  static bool valid(const ClockDateTime& value);

 private:
  static uint8_t fromBcd(uint8_t value);
  static uint8_t toBcd(uint8_t value);
  bool readRegisters(uint8_t start, uint8_t* data, size_t length);
  TwoWire* wire_ = &Wire1;
};

