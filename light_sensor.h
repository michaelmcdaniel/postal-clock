#pragma once

#include <Arduino.h>

class LightSensor {
 public:
  void begin(uint8_t minimumContrast, uint8_t maximumContrast,
             uint16_t lightCutoff);
  // Returns the selected configured brightness. ClockDisplay writes it
  // directly to the SH1106 contrast register.
  bool service(uint32_t nowMs, uint8_t& brightness);
  uint16_t value() const { return static_cast<uint16_t>(filtered_ >> 4); }

 private:
  uint32_t filtered_ = 0;
  uint32_t lastSampleMs_ = 0;
  // Zero is not a valid configured value, so it forces the first sample to
  // apply brightness even when the selected value is the normal 255 maximum.
  uint8_t lastBrightness_ = 0;
  uint8_t minimum_ = 1;
  uint8_t maximum_ = 255;
  uint16_t cutoff_ = 300;
  bool dark_ = false;
  bool initialized_ = false;
};
