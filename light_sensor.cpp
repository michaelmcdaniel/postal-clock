#include "light_sensor.h"

#include "pins.h"

namespace {
constexpr uint16_t kCutoffHysteresis = 20;
}

void LightSensor::begin(uint8_t minimumContrast, uint8_t maximumContrast,
                        uint16_t lightCutoff) {
  minimum_ = minimumContrast;
  maximum_ = maximumContrast;
  cutoff_ = lightCutoff;
  initialized_ = false;
  dark_ = false;
  lastBrightness_ = 0;
  analogReadResolution(12);
  pinMode(Pins::kLightAdc, INPUT);
}

bool LightSensor::service(uint32_t nowMs, uint8_t& brightness) {
  if (static_cast<uint32_t>(nowMs - lastSampleMs_) < 100) return false;
  lastSampleMs_ = nowMs;
  const uint16_t sample = analogRead(Pins::kLightAdc);
  const bool firstSample = !initialized_;
  if (firstSample) {
    filtered_ = static_cast<uint32_t>(sample) << 4;
    initialized_ = true;
  } else {
    const int32_t target = static_cast<int32_t>(sample) << 4;
    filtered_ = static_cast<uint32_t>(static_cast<int32_t>(filtered_) +
                                      ((target - static_cast<int32_t>(filtered_)) >> 3));
  }

  const uint16_t adc = value();
  if (firstSample) {
    dark_ = adc < cutoff_;
  } else if (dark_) {
    if (adc >= min<uint16_t>(4000, cutoff_ + kCutoffHysteresis)) dark_ = false;
  } else if (adc + kCutoffHysteresis < cutoff_) {
    dark_ = true;
  }

  const uint8_t mapped = dark_ ? minimum_ : maximum_;
  if (mapped == lastBrightness_) return false;
  lastBrightness_ = mapped;
  brightness = mapped;
  return true;
}
