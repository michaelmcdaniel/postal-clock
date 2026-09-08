#pragma once

#include <Arduino.h>
#include <U8g2lib.h>

#include "rtc.h"
#include "weather.h"

enum class WifiIconState : uint8_t {
  kDisconnected,
  kConnecting,
  kConnected,
};

class ClockDisplay {
 public:
  ClockDisplay();
  bool begin();
  void showSplash(const char* softwareVersion);
  void showInfo(uint8_t page, const char* networkName, const char* configUrl,
                const char* softwareVersion, const char* stationId,
                const WeatherSnapshot& weather);
  void showProvisioning(const IPAddress& address, bool storageReady);
  void showClock(const ClockDateTime& now, bool rtcValid, bool hasWeather,
                 int16_t temperatureF, bool weatherStale,
                 bool hasDailyRange, int16_t dailyLowF, int16_t dailyHighF,
                 const char* shortForecast,
                 int precipitationChance,
                 WifiIconState wifiState, uint8_t wifiBars,
                 uint32_t nowMs);
  // The configured 1-255 brightness value is written directly to the SH1106
  // contrast register.
  void setBrightness(uint8_t brightness);

 private:
  void drawDigit(uint8_t digit, int16_t x, int16_t y);
  void drawRaspberryPiLogo(int16_t x, int16_t y);
  void drawWifiDot(int16_t x, int16_t y);
  U8G2_SH1106_128X64_NONAME_F_HW_I2C oled_;
};
