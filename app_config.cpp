#include "app_config.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace {
constexpr char kConfigPath[] = "/config.json";
constexpr char kTempPath[] = "/config.new";
constexpr char kBackupPath[] = "/config.bak";
constexpr size_t kMaxConfigBytes = 2048;

void setReason(String* reason, const __FlashStringHelper* text) {
  if (reason) *reason = text;
}

bool copyJsonString(JsonVariantConst value, char* destination, size_t size) {
  if (!value.is<const char*>()) return false;
  const char* source = value.as<const char*>();
  if (!source || strlen(source) >= size) return false;
  strlcpy(destination, source, size);
  return true;
}
}  // namespace

bool ConfigStore::begin() {
  mounted_ = LittleFS.begin();
  return mounted_;
}

bool ConfigStore::load(AppConfig& config, String* reason) const {
  if (!mounted_) {
    setReason(reason, F("LittleFS is not mounted"));
    return false;
  }
  return loadPath(kConfigPath, config, reason);
}

bool ConfigStore::loadPath(const char* path, AppConfig& config, String* reason) const {
  File file = LittleFS.open(path, "r");
  if (!file) {
    setReason(reason, F("configuration file is missing or unreadable"));
    return false;
  }
  if (file.size() == 0 || file.size() > kMaxConfigBytes) {
    file.close();
    setReason(reason, F("configuration file size is invalid"));
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error) {
    if (reason) *reason = String(F("malformed JSON: ")) + error.c_str();
    return false;
  }
  if (document["schema_version"] != AppConfig::kSchemaVersion) {
    setReason(reason, F("unsupported configuration schema"));
    return false;
  }

  AppConfig candidate;
  if (!copyJsonString(document["wifi"]["ssid"], candidate.wifiSsid,
                      sizeof(candidate.wifiSsid)) ||
      !copyJsonString(document["wifi"]["password"], candidate.wifiPassword,
                      sizeof(candidate.wifiPassword)) ||
      !copyJsonString(document["timezone"], candidate.timezone,
                      sizeof(candidate.timezone)) ||
      !copyJsonString(document["contact_email"], candidate.contactEmail,
                      sizeof(candidate.contactEmail)) ||
      !copyJsonString(document["weather"]["station_id"],
                      candidate.weatherStationId,
                      sizeof(candidate.weatherStationId))) {
    setReason(reason, F("required string field is missing or too long"));
    return false;
  }

  candidate.weatherRefreshMinutes =
      document["weather"]["refresh_minutes"] | 60U;
  candidate.brightnessMin = document["display"]["brightness_min"] | 1U;
  candidate.brightnessMax = document["display"]["brightness_max"] | 255U;
  candidate.brightnessCutoff =
      document["display"]["light_cutoff"] | 300U;
  candidate.debugLogging = document["debug_logging"] | true;
  if (!validate(candidate, reason)) return false;
  config = candidate;
  return true;
}

bool ConfigStore::save(const AppConfig& config, String* reason) const {
  if (!mounted_) {
    setReason(reason, F("LittleFS is not mounted"));
    return false;
  }
  if (!validate(config, reason)) return false;

  JsonDocument document;
  document["schema_version"] = AppConfig::kSchemaVersion;
  document["wifi"]["ssid"] = config.wifiSsid;
  document["wifi"]["password"] = config.wifiPassword;
  document["timezone"] = config.timezone;
  document["contact_email"] = config.contactEmail;
  document["weather"]["station_id"] = config.weatherStationId;
  document["weather"]["refresh_minutes"] = config.weatherRefreshMinutes;
  document["display"]["brightness_min"] = config.brightnessMin;
  document["display"]["brightness_max"] = config.brightnessMax;
  document["display"]["light_cutoff"] = config.brightnessCutoff;
  document["debug_logging"] = config.debugLogging;

  LittleFS.remove(kTempPath);
  File temporary = LittleFS.open(kTempPath, "w");
  if (!temporary) {
    setReason(reason, F("could not create staged configuration"));
    return false;
  }
  const size_t expected = measureJson(document);
  const size_t written = serializeJson(document, temporary);
  temporary.flush();
  temporary.close();
  if (written != expected) {
    LittleFS.remove(kTempPath);
    setReason(reason, F("staged configuration write was incomplete"));
    return false;
  }

  AppConfig verified;
  if (!loadPath(kTempPath, verified, reason)) {
    LittleFS.remove(kTempPath);
    return false;
  }

  LittleFS.remove(kBackupPath);
  const bool hadOriginal = LittleFS.exists(kConfigPath);
  if (hadOriginal && !LittleFS.rename(kConfigPath, kBackupPath)) {
    LittleFS.remove(kTempPath);
    setReason(reason, F("could not preserve previous configuration"));
    return false;
  }
  if (!LittleFS.rename(kTempPath, kConfigPath)) {
    if (hadOriginal) LittleFS.rename(kBackupPath, kConfigPath);
    setReason(reason, F("could not activate staged configuration"));
    return false;
  }

  AppConfig committed;
  if (!loadPath(kConfigPath, committed, reason)) {
    LittleFS.remove(kConfigPath);
    if (hadOriginal) LittleFS.rename(kBackupPath, kConfigPath);
    return false;
  }
  LittleFS.remove(kBackupPath);
  return true;
}

bool ConfigStore::clear() {
  if (!mounted_) return false;
  const bool removedConfig = !LittleFS.exists(kConfigPath) ||
                             LittleFS.remove(kConfigPath);
  LittleFS.remove(kTempPath);
  LittleFS.remove(kBackupPath);
  return removedConfig;
}

bool ConfigStore::validate(const AppConfig& config, String* reason) {
  const size_t ssidLength = strnlen(config.wifiSsid, sizeof(config.wifiSsid));
  if (ssidLength == 0 || ssidLength > 32) {
    setReason(reason, F("Wi-Fi SSID is required and must be at most 32 bytes"));
    return false;
  }
  if (strnlen(config.wifiPassword, sizeof(config.wifiPassword)) >=
      sizeof(config.wifiPassword)) {
    setReason(reason, F("Wi-Fi password is too long"));
    return false;
  }
  if (!posixTimezone(config.timezone)) {
    setReason(reason, F("timezone is not supported"));
    return false;
  }
  const size_t emailLength =
      strnlen(config.contactEmail, sizeof(config.contactEmail));
  const char* at = strchr(config.contactEmail, '@');
  if (emailLength < 3 || emailLength >= sizeof(config.contactEmail) ||
      !at || at == config.contactEmail || at[1] == '\0' ||
      strchr(at + 1, '@')) {
    setReason(reason, F("a valid contact email is required"));
    return false;
  }
  for (size_t i = 0; i < emailLength; ++i) {
    const char character = config.contactEmail[i];
    if (character <= ' ' || character == ',' || character == '(' ||
        character == ')') {
      setReason(reason, F("contact email contains an invalid character"));
      return false;
    }
  }
  if (strnlen(config.weatherStationId, sizeof(config.weatherStationId)) != 4) {
    setReason(reason, F("weather station ID must be four characters"));
    return false;
  }
  for (const char* character = config.weatherStationId; *character; ++character) {
    if (!((*character >= 'A' && *character <= 'Z') ||
          (*character >= '0' && *character <= '9'))) {
      setReason(reason, F("weather station ID must use uppercase letters and digits"));
      return false;
    }
  }
  if (config.weatherStationId[0] != 'K') {
    setReason(reason, F("weather station ID must be a U.S. ICAO identifier beginning with K"));
    return false;
  }
  if (config.weatherRefreshMinutes < 5 || config.weatherRefreshMinutes > 180) {
    setReason(reason, F("weather refresh must be between 5 and 180 minutes"));
    return false;
  }
  if (config.brightnessMin == 0 || config.brightnessMax == 0 ||
      config.brightnessMin > config.brightnessMax) {
    setReason(reason, F("brightness values must be 1-255 with minimum not exceeding maximum"));
    return false;
  }
  if (config.brightnessCutoff > 4000) {
    setReason(reason, F("light cutoff must be between 0 and 4000"));
    return false;
  }
  return true;
}

const char* ConfigStore::posixTimezone(const char* timezone) {
  struct Zone { const char* iana; const char* posix; };
  static constexpr Zone zones[] = {
      {"UTC", "UTC0"},
      {"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
      {"America/Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2"},
      {"America/Denver", "MST7MDT,M3.2.0/2,M11.1.0/2"},
      {"America/Phoenix", "MST7"},
      {"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
      {"America/Anchorage", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
      {"Pacific/Honolulu", "HST10"},
  };
  for (const auto& zone : zones) {
    if (strcmp(timezone, zone.iana) == 0) return zone.posix;
  }
  return nullptr;
}
