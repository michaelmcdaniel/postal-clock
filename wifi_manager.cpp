#include "wifi_manager.h"

#include <WiFi.h>

namespace {
constexpr uint32_t kConnectTimeoutMs = 12000;
constexpr uint32_t kMaximumBackoffMs = 30UL * 60UL * 1000UL;
const IPAddress kPublicDnsFallback(8, 8, 8, 8);
}

void WifiManager::begin(const AppConfig& config) {
  strlcpy(ssid_, config.wifiSsid, sizeof(ssid_));
  strlcpy(password_, config.wifiPassword, sizeof(password_));
  connected_ = false;
  wasConnected_ = false;
  signalBars_ = 0;
  localAddress_ = IPAddress();
  WiFi.mode(WIFI_STA);
  started_ = true;
  startAttempt(millis());
}

void WifiManager::startAttempt(uint32_t nowMs) {
  Serial.print(F("Wi-Fi: connecting to "));
  Serial.println(ssid_);
  // Arduino-Pico's begin() waits for association. Keep the clock task
  // responsive while the connection state machine runs in the background.
  WiFi.beginNoBlock(ssid_, password_);
  attemptStartedMs_ = nowMs;
  attempting_ = true;
}

void WifiManager::service(uint32_t nowMs) {
  const uint8_t status = WiFi.status();
  const bool isConnected = status == WL_CONNECTED;
  connected_ = isConnected;

  if (isConnected) {
    localAddress_ = WiFi.localIP();
    const int32_t rssi = WiFi.RSSI();
    signalBars_ = rssi >= -55 ? 3 : (rssi >= -70 ? 2 : 1);

    if (!wasConnected_) {
      // DHCP commonly provides only one resolver. Preserve it for local
      // names and Pi-hole, and add Google's resolver as a second fallback
      // when that primary service is unavailable.
      const IPAddress primaryDns = WiFi.dnsIP(0);
      if (primaryDns != IPAddress()) {
        WiFi.setDNS(primaryDns, kPublicDnsFallback);
      } else {
        WiFi.setDNS(kPublicDnsFallback);
      }
      Serial.print(F("Wi-Fi: connected, address "));
      Serial.println(localAddress_);
      Serial.print(F("Wi-Fi: DNS primary "));
      Serial.print(primaryDns);
      Serial.println(F(", fallback 8.8.8.8"));
    }
    wasConnected_ = true;
    attempting_ = false;
    backoffMs_ = 30000;
    return;
  }

  signalBars_ = 0;
  localAddress_ = IPAddress();

  if (wasConnected_) {
    Serial.println(F("Wi-Fi: disconnected"));
    wasConnected_ = false;
    nextAttemptMs_ = nowMs + backoffMs_;
  }

  if (attempting_) {
    if (static_cast<uint32_t>(nowMs - attemptStartedMs_) <
        kConnectTimeoutMs) {
      return;
    }
    WiFi.disconnect();
    attempting_ = false;
    nextAttemptMs_ = nowMs + backoffMs_;
    Serial.print(F("Wi-Fi: connection timed out; retry in seconds: "));
    Serial.println(backoffMs_ / 1000);
    backoffMs_ = min(backoffMs_ * 2, kMaximumBackoffMs);
    return;
  }

  if (static_cast<int32_t>(nowMs - nextAttemptMs_) >= 0) {
    startAttempt(nowMs);
  }
}
