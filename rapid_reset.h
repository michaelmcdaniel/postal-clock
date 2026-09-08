#pragma once

#include <Arduino.h>

#include "rtc.h"

class RapidResetDetector {
 public:
  // Records this boot and returns true on the third boot in a 10-second window.
  // LittleFS must already be mounted and the RTC value must be valid.
  bool recordBoot(const ClockDateTime& now) const;

 private:
  static uint32_t secondsSince2020(const ClockDateTime& value);
};
