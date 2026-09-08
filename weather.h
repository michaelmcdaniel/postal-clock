#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "app_config.h"
#include "rtc.h"

struct WeatherSnapshot {
  bool hasTemperature = false;
  int16_t temperatureF = 0;
  bool temperatureStale = false;
  bool hasDailyRange = false;
  int16_t dailyLowF = 0;
  int16_t dailyHighF = 0;
  char shortForecast[64] = {};
  int precipitationChance = 0;

  // Latest harvested station-observation details.  Each field carries its own
  // validity flag because api.weather.gov may legitimately omit individual
  // measurements while still returning a usable observation.
  bool hasHumidity = false;
  uint8_t humidityPct = 0;
  bool hasWind = false;
  float windMph = 0.0f;
  int16_t windDirectionDeg = 0;
  bool hasPressure = false;
  float pressureInHg = 0.0f;
  float pressureDeltaInHg = 0.0f;  // relative to standard 29.92 inHg
  bool hasFeelsLike = false;
  int16_t feelsLikeF = 0;
  bool hasPrecipLastHour = false;
  float precipLastHourIn = 0.0f;
  float latitude = 0.0f;
  float longitude = 0.0f;
  int lastObservationStatus = 0;
  int lastForecastStatus = 0;
  ClockDateTime lastObservationRequest{};
  ClockDateTime lastForecastRequest{};
  char lastFailedRequest[16] = {};
  int lastTlsError = 0;
  char lastTlsErrorText[64] = {};
  char lastTransportDetail[96] = {};
  char observationDetails[128] = {};
};

class WeatherService {
 public:
  void begin(const AppConfig& config);
  void service(uint32_t nowMs, bool wifiConnected, const ClockDateTime& localTime,
               bool localTimeValid);
  void snapshot(WeatherSnapshot& out, uint32_t nowMs) const;

 private:
  bool fetchObservation();
  bool fetchForecast(const ClockDateTime& localTime, bool localTimeValid);
  bool updateTemperature(JsonVariantConst value);
  bool fetchJson(const char* url, JsonDocument& filter, JsonDocument& document,
                 size_t maximumBytes, const char* requestType);
  bool stale(uint32_t nowMs) const;

  char url_[256] = {};
  char recentObservationsUrl_[256] = {};
  char userAgent_[176] = {};
  uint32_t forecastRefreshMs_ = 60UL * 60UL * 1000UL;
  uint32_t nextObservationMs_ = 5000;
  uint32_t nextForecastMs_ = 5000;
  uint32_t lastTemperatureSuccessMs_ = 0;
  float latitude_ = 0.0f;
  float longitude_ = 0.0f;
  int16_t temperatureF_ = 0;
  int16_t dailyLowF_ = 0;
  int16_t dailyHighF_ = 0;
  char shortForecast_[64] = {};
  int precipitationChance_ = 0;
  bool hasTemperature_ = false;
  bool hasDailyRange_ = false;

  bool hasHumidity_ = false;
  uint8_t humidityPct_ = 0;
  bool hasWind_ = false;
  float windMph_ = 0.0f;
  int16_t windDirectionDeg_ = 0;
  bool hasPressure_ = false;
  float pressureInHg_ = 0.0f;
  float pressureDeltaInHg_ = 0.0f;
  bool hasFeelsLike_ = false;
  int16_t feelsLikeF_ = 0;
  bool hasPrecipLastHour_ = false;
  float precipLastHourIn_ = 0.0f;
  bool hasCoordinates_ = false;
  int lastHttpStatus_ = -1;
  int lastObservationStatus_ = 0;
  int lastForecastStatus_ = 0;
  ClockDateTime lastObservationRequest_{};
  ClockDateTime lastForecastRequest_{};
  char lastFailedRequest_[16] = {};
  int lastTlsError_ = 0;
  char lastTlsErrorText_[64] = {};
  char lastTransportDetail_[96] = "Waiting for first weather request";
  char observationDetails_[128] = {};
};
