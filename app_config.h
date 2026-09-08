#pragma once

#include <Arduino.h>

struct AppConfig {
  static constexpr uint16_t kSchemaVersion = 5;
  char wifiSsid[33] = {};
  char wifiPassword[65] = {};
  char timezone[40] = {};
  char contactEmail[129] = {};
  char weatherStationId[5] = {};
  uint32_t weatherRefreshMinutes = 60;
  uint8_t brightnessMin = 1;
  uint8_t brightnessMax = 255;
  uint16_t brightnessCutoff = 300;
  bool debugLogging = true;
};

class ConfigStore {
 public:
  bool begin();
  bool mounted() const { return mounted_; }
  bool load(AppConfig& config, String* reason = nullptr) const;
  bool save(const AppConfig& config, String* reason = nullptr) const;
  bool clear();
  static bool validate(const AppConfig& config, String* reason = nullptr);
  static const char* posixTimezone(const char* ianaTimezone);

 private:
  bool loadPath(const char* path, AppConfig& config, String* reason) const;
  bool mounted_ = false;
};
