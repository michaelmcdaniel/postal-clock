#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "app_config.h"
#include "weather.h"

class ProvisioningService {
 public:
  bool begin(ConfigStore& store);
  bool beginConnected(ConfigStore& store, const AppConfig& config);
  void service(uint32_t nowMs);
  void setLightValue(uint16_t value) { lightValue_ = value; }
  void setWeatherSnapshot(const WeatherSnapshot& snapshot) {
    weatherSnapshot_ = snapshot;
    hasWeatherSnapshot_ = true;
  }
  IPAddress address() const { return address_; }

 private:
  void registerRoutes();
  void handleRoot();
  void handleSave();
  void sendSaveError(int status, const String& message);
  void handleScan();
  void handleScanStatus();
  void handleLightStatus();
  void handleWeather();
  void handleDebug();
  void handleNotFound();
  void startScan(uint32_t nowMs);
  String pageHtml(const String& message = String()) const;
  static String htmlEscape(const String& input);

  ConfigStore* store_ = nullptr;
  DNSServer dns_;
  WebServer server_{80};
  IPAddress address_{192, 168, 4, 1};
  uint32_t nextScanMs_ = 0;
  uint32_t rebootAtMs_ = 0;
  bool scanRunning_ = false;
  bool scanFailed_ = false;
  bool routesRegistered_ = false;
  bool connectedMode_ = false;
  bool connectedAddressLogged_ = false;
  uint16_t lightValue_ = 0;
  WeatherSnapshot weatherSnapshot_;
  bool hasWeatherSnapshot_ = false;
  AppConfig existingConfig_;
};
