#pragma once

#include <Arduino.h>

#include "app_config.h"

class WifiManager {
 public:
  void begin(const AppConfig& config);
  void service(uint32_t nowMs);
  bool connected() const { return connected_; }
  bool connecting() const { return started_ && !connected_; }
  uint8_t signalBars() const { return signalBars_; }
  IPAddress localAddress() const { return localAddress_; }

 private:
  void startAttempt(uint32_t nowMs);
  char ssid_[33] = {};
  char password_[65] = {};
  uint32_t attemptStartedMs_ = 0;
  uint32_t nextAttemptMs_ = 0;
  uint32_t backoffMs_ = 30000;
  IPAddress localAddress_{};
  uint8_t signalBars_ = 0;
  bool attempting_ = false;
  bool wasConnected_ = false;
  bool connected_ = false;
  bool started_ = false;
};
