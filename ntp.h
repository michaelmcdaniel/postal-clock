#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>

#include "app_config.h"
#include "rtc.h"

class NtpService {
 public:
  void begin(const AppConfig& config, RtcService& rtc);
  // Must run on core 1 while the shared network mutex is held. DNS can block
  // for seconds on Arduino-Pico, so it is deliberately never called by core 0.
  void serviceDns(uint32_t nowMs, bool wifiConnected);
  void service(uint32_t nowMs, bool wifiConnected);
  bool picoTimeValid() const { return picoTimeValid_; }

 private:
  bool startRequest(uint32_t nowMs);
  bool applyResponse();
  WiFiUDP udp_;
  IPAddress serverAddress_;
  RtcService* rtc_ = nullptr;
  const char* posixTimezone_ = nullptr;
  uint32_t nextAttemptMs_ = 0;
  uint32_t nextDnsAttemptMs_ = 0;
  uint32_t requestStartedMs_ = 0;
  bool waiting_ = false;
  bool hasServerAddress_ = false;
  bool picoTimeValid_ = false;
};
