#include "weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace {

constexpr uint32_t kObservationRefreshMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kWeatherRetryMs = 60UL * 1000UL;
// NOAA is usually quick, but a 1.5-second budget is too short for a cold DNS
// lookup plus TLS handshake on a small Wi-Fi device. This work is isolated on
// core 1, so this bounded timeout cannot interrupt clock/display servicing.
constexpr uint16_t kWeatherHttpTimeoutMs = 8000;
constexpr char kNwsHost[] = "api.weather.gov";
constexpr size_t kForecastMaxChars = 16;

uint64_t localMinuteKey(const char* isoTime) {
  if (!isoTime || strlen(isoTime) < 16) return 0;
  unsigned year, month, day, hour, minute;
  if (sscanf(isoTime, "%4u-%2u-%2uT%2u:%2u", &year, &month, &day,
             &hour, &minute) != 5) return 0;
  return (((static_cast<uint64_t>(year) * 100 + month) * 100 + day) * 100 +
          hour) * 100 + minute;
}

uint64_t localMinuteKey(const ClockDateTime& time) {
  return (((static_cast<uint64_t>(time.year) * 100 + time.month) * 100 +
           time.day) * 100 + time.hour) * 100 + time.minute;
}

bool containsIgnoreCase(const char* text, const char* needle) {
  if (!text || !needle || !*needle) return false;
  for (; *text; ++text) {
    const char* left = text;
    const char* right = needle;
    while (*left && *right) {
      char a = *left++;
      char b = *right++;
      if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
      if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
      if (a != b) break;
    }
    if (!*right) return true;
  }
  return false;
}

struct ForecastRule {
  const char* match;
  const char* label;
};

constexpr ForecastRule kForecastRules[] = {
    {"severe thunder", "Severe Storms"},
    {"severe tst", "Severe Storms"},
    {"blizzard", "Blizzard"},
    {"freezing drizzle", "Frz Drizzle"},
    {"freezing rain", "Frz Rain"},
    {"winter weather", "Wintry Mix"},
    {"wintry mix", "Wintry Mix"},
    {"rain/snow", "Rain/Snow"},
    {"rain/sleet", "Rain/Sleet"},
    {"snow/sleet", "Snow/Sleet"},
    {"ice pellets", "Sleet"},
    {"sleet", "Sleet"},
    {"snow shower", "Snow Shwrs"},
    {"blowing snow", "Blwng Snow"},
    {"flurr", "Flurries"},
    {"snow", "Snow"},
    {"thunder", "Storms"},
    {"heavy rain", "Heavy Rain"},
    {"rain shower", "Showers"},
    {"shower", "Showers"},
    {"drizzle", "Drizzle"},
    {"rain", "Rain"},
    {"hail", "Hail"},
    {"freezing fog", "Frz Fog"},
    {"dense fog", "Dense Fog"},
    {"fog", "Fog"},
    {"blowing dust", "Blwng Dust"},
    {"blowing sand", "Blwng Sand"},
    {"volcanic ash", "Volcanic Ash"},
    {"smoke", "Smoke"},
    {"haze", "Haze"},
    {"freezing spray", "Frz Spray"},
    {"frost", "Frost"},
    {"water spout", "Waterspouts"},
    {"blustery", "Blustery"},
    {"breezy", "Breezy"},
    {"windy", "Windy"},
    {"mostly sunny", "< Sunny"},
    {"partly sunny", "Pt Sunny"},
    {"sunny", "Sunny"},
    {"mostly clear", "< Clear"},
    {"partly cloudy", "Pt Cloudy"},
    {"mostly cloudy", "< Cloudy"},
    {"increasing clouds", "Clouds +"},
    {"decreasing clouds", "Clouds -"},
    {"becoming cloudy", "Cloudier"},
    {"becoming sunny", "Clearing"},
    {"gradual clearing", "Clearing"},
    {"clearing", "Clearing"},
    {"cloudy", "Cloudy"},
    {"clear", "Clear"},
    {"hot", "Hot"},
    {"cold", "Cold"},
};

void copyAtWordBoundary(const char* source, char* destination, size_t capacity) {
  if (!destination || capacity == 0) return;
  destination[0] = '\0';
  if (!source || !*source) return;

  const size_t allowed = min(kForecastMaxChars, capacity - 1);
  const size_t length = strlen(source);
  if (length <= allowed) {
    memcpy(destination, source, length);
    destination[length] = '\0';
    return;
  }

  size_t end = allowed;
  while (end > 0 && source[end] != ' ') --end;
  while (end > 0 && source[end - 1] == ' ') --end;
  if (end == 0) {
    strlcpy(destination, "Weather", capacity);
    return;
  }

  memcpy(destination, source, end);
  destination[end] = '\0';
}

void condenseForecast(const char* source, int precipitationChance,
                      char* destination, size_t capacity) {
  (void)precipitationChance;  // PoP is displayed separately by ClockDisplay.
  if (!destination || capacity == 0) return;
  destination[0] = '\0';
  if (!source || !*source) return;

  if (strlen(source) <= kForecastMaxChars) {
    strlcpy(destination, source, capacity);
    return;
  }

  for (const auto& rule : kForecastRules) {
    if (containsIgnoreCase(source, rule.match)) {
      copyAtWordBoundary(rule.label, destination, capacity);
      return;
    }
  }
  copyAtWordBoundary(source, destination, capacity);
}

}  // namespace

void WeatherService::begin(const AppConfig& config) {
  // KMBT and some other stations may expose a null value when the default
  // quality-control selection is used.  Explicitly request the latest report
  // without requiring every QC gate; values are still sanity-checked locally.
  snprintf(url_, sizeof(url_),
           "https://api.weather.gov/stations/%s/observations/latest?require_qc=false",
           config.weatherStationId);
  snprintf(recentObservationsUrl_, sizeof(recentObservationsUrl_),
           "https://api.weather.gov/stations/%s/observations?limit=3",
           config.weatherStationId);
  snprintf(userAgent_, sizeof(userAgent_), "(postal-clock.mcdaniel.ws, %s)",
           config.contactEmail);
  forecastRefreshMs_ = config.weatherRefreshMinutes * 60UL * 1000UL;
  nextObservationMs_ = millis() + 5000;
  nextForecastMs_ = millis() + 5000;
}

void WeatherService::service(uint32_t nowMs, bool wifiConnected,
                             const ClockDateTime& localTime,
                             bool localTimeValid) {
  if (!wifiConnected) return;

  if (static_cast<int32_t>(nowMs - nextObservationMs_) >= 0) {
    lastObservationRequest_ = localTime;
    const bool success = fetchObservation();
    lastObservationStatus_ = success ? 200 : lastHttpStatus_;
    const uint32_t completedMs = millis();
    nextObservationMs_ = completedMs +
                         (success ? kObservationRefreshMs : kWeatherRetryMs);
    return;
  }

  if (hasCoordinates_ &&
      static_cast<int32_t>(nowMs - nextForecastMs_) >= 0) {
    lastForecastRequest_ = localTime;
    const bool success = fetchForecast(localTime, localTimeValid);
    lastForecastStatus_ = success ? 200 : lastHttpStatus_;
    const uint32_t completedMs = millis();
    nextForecastMs_ = completedMs +
                      (success ? forecastRefreshMs_ : kWeatherRetryMs);
  }
}

bool WeatherService::fetchJson(const char* url, JsonDocument& filter,
                               JsonDocument& document, size_t maximumBytes,
                               const char* requestType) {
  lastHttpStatus_ = -1;
  BearSSL::WiFiClientSecure client;
  // api.weather.gov accepts TLS 1.2.  Pinning that protocol avoids offering
  // obsolete TLS versions and makes the expected BearSSL handshake explicit.
  client.setSSLVersion(BR_TLS12, BR_TLS12);
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    strlcpy(lastFailedRequest_, requestType, sizeof(lastFailedRequest_));
    lastTlsError_ =
        client.getLastSSLError(lastTlsErrorText_, sizeof(lastTlsErrorText_));
    snprintf(lastTransportDetail_, sizeof(lastTransportDetail_),
             "HTTP initialization failed");
    Serial.print(F("Weather: "));
    Serial.print(requestType);
    Serial.println(F(" request could not initialize HTTPS"));
    return false;
  }

  // Certificate validation is deliberately disabled for this appliance. The
  // API is still contacted over encrypted HTTPS, without a bundled CA or a
  // dependency on the DS3231/system clock being correct during the handshake.
  http.setTimeout(kWeatherHttpTimeoutMs);
  http.setUserAgent(userAgent_);
  http.addHeader("Accept", "application/geo+json, application/json");
  const int status = http.GET();
  lastHttpStatus_ = status;
  if (status != HTTP_CODE_OK) {
    strlcpy(lastFailedRequest_, requestType, sizeof(lastFailedRequest_));
    lastTlsError_ =
        client.getLastSSLError(lastTlsErrorText_, sizeof(lastTlsErrorText_));
    Serial.print(F("Weather: "));
    Serial.print(requestType);
    Serial.print(F(" request failed, HTTP status "));
    Serial.println(status);
    if (status < 0) {
      // HTTPClient closes its connection before returning a negative error,
      // which can erase BearSSL's reason. Reproduce the connection only on
      // failure so /debug can distinguish DNS, TCP, and TLS trouble without
      // adding work to successful weather polling.
      IPAddress address;
      if (WiFi.hostByName(kNwsHost, address) != 1) {
        strlcpy(lastTransportDetail_, "DNS lookup failed", sizeof(lastTransportDetail_));
      } else {
        BearSSL::WiFiClientSecure probe;
        probe.setSSLVersion(BR_TLS12, BR_TLS12);
        probe.setInsecure();
        if (probe.connect(kNwsHost, 443)) {
          snprintf(lastTransportDetail_, sizeof(lastTransportDetail_),
                   "DNS %u.%u.%u.%u; TCP/TLS probe connected", address[0],
                   address[1], address[2], address[3]);
          probe.stop();
        } else {
          lastTlsError_ = probe.getLastSSLError(
              lastTlsErrorText_, sizeof(lastTlsErrorText_));
          snprintf(lastTransportDetail_, sizeof(lastTransportDetail_),
                   "DNS %u.%u.%u.%u; TLS probe failed", address[0],
                   address[1], address[2], address[3]);
        }
      }
      Serial.print(F("Weather: TLS detail "));
      Serial.println(lastTlsErrorText_);
      Serial.print(F("Weather: transport "));
      Serial.println(lastTransportDetail_);
    } else {
      snprintf(lastTransportDetail_, sizeof(lastTransportDetail_),
               "HTTP status %d", status);
    }
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength > static_cast<int>(maximumBytes)) {
    Serial.print(F("Weather: "));
    Serial.print(requestType);
    Serial.println(F(" response exceeded size limit"));
    http.end();
    return false;
  }

  const DeserializationError error = deserializeJson(
      document, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    Serial.print(F("Weather: "));
    Serial.print(requestType);
    Serial.print(F(" JSON parse failed: "));
    Serial.println(error.c_str());
    return false;
  }
  // A completed request supersedes any previous transport failure. Keep the
  // diagnostics truthful instead of displaying a stale DNS/TLS error beside
  // fresh weather data.
  lastFailedRequest_[0] = '\0';
  lastTlsError_ = 0;
  lastTlsErrorText_[0] = '\0';
  snprintf(lastTransportDetail_, sizeof(lastTransportDetail_),
           "HTTP 200; %d byte response", contentLength);
  return true;
}

bool WeatherService::fetchObservation() {
  JsonDocument observationFilter;
  observationFilter["geometry"]["coordinates"][0] = true;
  observationFilter["geometry"]["coordinates"][1] = true;
  observationFilter["properties"]["temperature"]["value"] = true;
  observationFilter["properties"]["relativeHumidity"]["value"] = true;
  observationFilter["properties"]["windSpeed"]["value"] = true;
  observationFilter["properties"]["windDirection"]["value"] = true;
  observationFilter["properties"]["barometricPressure"]["value"] = true;
  observationFilter["properties"]["precipitationLastHour"]["value"] = true;
  observationFilter["properties"]["windChill"]["value"] = true;
  observationFilter["properties"]["heatIndex"]["value"] = true;

  JsonDocument observation;
  if (!fetchJson(url_, observationFilter, observation, 32768,
                 "observation")) return false;

  JsonArrayConst coordinates = observation["geometry"]["coordinates"];
  if (coordinates.size() < 2 ||
      (!coordinates[0].is<float>() && !coordinates[0].is<double>()) ||
      (!coordinates[1].is<float>() && !coordinates[1].is<double>())) {
    Serial.println(F("Weather: station response had no coordinates"));
    return false;
  }

  longitude_ = coordinates[0].as<float>();
  latitude_ = coordinates[1].as<float>();
  hasCoordinates_ = true;

  JsonObjectConst props = observation["properties"];
  const float temperature = props["temperature"]["value"] | NAN;
  const float humidity = props["relativeHumidity"]["value"] | NAN;
  const float windSpeed = props["windSpeed"]["value"] | NAN;
  const float windDirection = props["windDirection"]["value"] | NAN;
  const float pressure = props["barometricPressure"]["value"] | NAN;
  const float rain = props["precipitationLastHour"]["value"] | NAN;
  const float chill = props["windChill"]["value"] | NAN;
  const float heat = props["heatIndex"]["value"] | NAN;

  // NWS observation units are Celsius, percent, km/h, degrees, Pa, and mm.
  // Convert once here so every consumer (OLED and web page) sees familiar
  // U.S. display units without duplicating conversion logic.
  hasHumidity_ = isfinite(humidity) && humidity >= 0.0f && humidity <= 100.0f;
  if (hasHumidity_) humidityPct_ = static_cast<uint8_t>(lroundf(humidity));

  hasWind_ = isfinite(windSpeed) && windSpeed >= 0.0f && windSpeed < 500.0f;
  if (hasWind_) windMph_ = windSpeed * 0.621371f;
  if (isfinite(windDirection)) {
    int direction = static_cast<int>(lroundf(windDirection)) % 360;
    if (direction < 0) direction += 360;
    windDirectionDeg_ = static_cast<int16_t>(direction);
  } else {
    windDirectionDeg_ = -1;
  }

  hasPressure_ = isfinite(pressure) && pressure > 80000.0f && pressure < 110000.0f;
  if (hasPressure_) {
    pressureInHg_ = pressure * 0.0002952998751f;
    pressureDeltaInHg_ = pressureInHg_ - 29.9213f;
  }

  hasPrecipLastHour_ = isfinite(rain) && rain >= 0.0f && rain < 1000.0f;
  if (hasPrecipLastHour_) precipLastHourIn_ = rain * 0.0393701f;

  const float feelsC = isfinite(heat) ? heat : (isfinite(chill) ? chill : temperature);
  hasFeelsLike_ = isfinite(feelsC) && feelsC >= -90.0f && feelsC <= 70.0f;
  if (hasFeelsLike_) {
    feelsLikeF_ = static_cast<int16_t>(
        lroundf(feelsC * 9.0f / 5.0f + 32.0f));
  }

  snprintf(observationDetails_, sizeof(observationDetails_),
           "T%.0f RH%.0f W%.0f@%.0f P%.0f R%.1f F%.0f",
           temperature, humidity, windSpeed, windDirection, pressure, rain,
           feelsC);

  if (updateTemperature(props["temperature"]["value"])) return true;

  // The latest convenience endpoint can occasionally have no temperature.
  // Keep the previous value and inspect the three most recent observations.
  // In an ArduinoJson array filter, element [0] is the template for every
  // source-array element, so this retains only the temperature field.
  JsonDocument recentFilter;
  recentFilter["features"][0]["properties"]["temperature"]["value"] = true;
  JsonDocument recent;
  if (!fetchJson(recentObservationsUrl_, recentFilter, recent, 32768,
                 "observation")) {
    Serial.println(F("Weather: could not obtain a recent temperature"));
    return true;
  }

  for (JsonObjectConst feature : recent["features"].as<JsonArrayConst>()) {
    if (updateTemperature(feature["properties"]["temperature"]["value"])) {
      Serial.println(F("Weather: used newest valid recent observation"));
      return true;
    }
  }

  Serial.println(F("Weather: station observation had no valid temperature"));
  return true;
}

bool WeatherService::updateTemperature(JsonVariantConst value) {
  if (!value.is<float>() && !value.is<double>() && !value.is<int>() &&
      !value.is<long>() && !value.is<unsigned long>()) return false;

  const float celsius = value.as<float>();
  if (!isfinite(celsius) || celsius < -90.0f || celsius > 60.0f) return false;

  temperatureF_ = lroundf(celsius * 9.0f / 5.0f + 32.0f);
  hasTemperature_ = true;
  lastTemperatureSuccessMs_ = millis();
  Serial.print(F("Weather: station temperature updated to "));
  Serial.print(temperatureF_);
  Serial.println(F(" F"));
  return true;
}

bool WeatherService::fetchForecast(const ClockDateTime& localTime,
                                   bool localTimeValid) {
  char pointsUrl[96];
  snprintf(pointsUrl, sizeof(pointsUrl),
           "https://api.weather.gov/points/%.4f,%.4f",
           latitude_, longitude_);

  JsonDocument pointsFilter;
  pointsFilter["properties"]["forecast"] = true;
  pointsFilter["properties"]["forecastHourly"] = true;
  JsonDocument points;
  if (!fetchJson(pointsUrl, pointsFilter, points, 32768, "points")) return false;

  const char* forecastUrl = points["properties"]["forecast"];
  const char* hourlyUrl = points["properties"]["forecastHourly"];
  if (!forecastUrl ||
      strncmp(forecastUrl, "https://api.weather.gov/", 24) != 0) {
    Serial.println(F("Weather: points response had no valid forecast URL"));
    return false;
  }

  JsonDocument forecastFilter;
  // For arrays, element zero in an ArduinoJson filter is the template applied
  // to every source element.  Keep only fields the clock actually consumes.
  forecastFilter["properties"]["periods"][0]["number"] = true;
  forecastFilter["properties"]["periods"][0]["isDaytime"] = true;
  forecastFilter["properties"]["periods"][0]["temperature"] = true;
  forecastFilter["properties"]["periods"][0]["temperatureUnit"] = true;
  forecastFilter["properties"]["periods"][0]["startTime"] = true;
  forecastFilter["properties"]["periods"][0]["endTime"] = true;
  forecastFilter["properties"]["periods"][0]["shortForecast"] = true;
  forecastFilter["properties"]["periods"][0]["probabilityOfPrecipitation"]["value"] = true;

  JsonDocument forecast;
  if (!fetchJson(forecastUrl, forecastFilter, forecast, 32768,
                 "forecast")) return false;

  bool foundDay = false;
  bool foundNight = false;
  char fallbackSummary[sizeof(shortForecast_)] = {};
  int fallbackPop = 0;
  const uint64_t nowKey = localTimeValid ? localMinuteKey(localTime) : 0;

  for (JsonObjectConst period :
       forecast["properties"]["periods"].as<JsonArrayConst>()) {
    const int periodNumber = period["number"] | 0;
    const char* unit = period["temperatureUnit"] | "";
    const bool daytime = period["isDaytime"] | false;

    // The clock intentionally shows the next two NWS forecast periods.  Their
    // order matters; isDaytime decides which value is the low or high.
    if (strcmp(unit, "F") == 0 && periodNumber >= 1 && periodNumber <= 2) {
      if (daytime) {
        dailyHighF_ = period["temperature"] | 0;
        foundDay = true;
      } else {
        dailyLowF_ = period["temperature"] | 0;
        foundNight = true;
      }
    }

    if (localTimeValid && fallbackSummary[0] == '\0') {
      const uint64_t start = localMinuteKey(period["startTime"] | "");
      const uint64_t end = localMinuteKey(period["endTime"] | "");
      if (start && end && nowKey >= start && nowKey < end) {
        fallbackPop =
            period["probabilityOfPrecipitation"]["value"] | 0;
        condenseForecast(period["shortForecast"] | "", fallbackPop,
                         fallbackSummary, sizeof(fallbackSummary));
      }
    }
  }

  // Commit the 12-hour result first.  Hourly data below can refine it, but a
  // failed hourly call will not erase a useful forecast/PoP.
  strlcpy(shortForecast_, fallbackSummary, sizeof(shortForecast_));
  precipitationChance_ = fallbackPop;

  if (hourlyUrl &&
      strncmp(hourlyUrl, "https://api.weather.gov/", 24) == 0) {
    JsonDocument hourlyFilter;
    hourlyFilter["properties"]["periods"][0]["number"] = true;
    hourlyFilter["properties"]["periods"][0]["shortForecast"] = true;
    hourlyFilter["properties"]["periods"][0]["probabilityOfPrecipitation"]["value"] = true;

    JsonDocument hourly;
    if (fetchJson(hourlyUrl, hourlyFilter, hourly, 32768, "hourly")) {
      for (JsonObjectConst period :
           hourly["properties"]["periods"].as<JsonArrayConst>()) {
        if ((period["number"] | 0) == 1) {
          precipitationChance_ =
              period["probabilityOfPrecipitation"]["value"] | 0;
          condenseForecast(period["shortForecast"] | "",
                           precipitationChance_, shortForecast_,
                           sizeof(shortForecast_));
          break;
        }
      }
    }
  }

  hasDailyRange_ = foundDay && foundNight;
  Serial.println(F("Weather: forecast updated"));
  return true;
}

bool WeatherService::stale(uint32_t nowMs) const {
  return hasTemperature_ &&
         static_cast<uint32_t>(nowMs - lastTemperatureSuccessMs_) >
             kObservationRefreshMs * 2UL;
}

void WeatherService::snapshot(WeatherSnapshot& out, uint32_t nowMs) const {
  out.hasTemperature = hasTemperature_;
  out.temperatureF = temperatureF_;
  out.temperatureStale = stale(nowMs);
  out.hasDailyRange = hasDailyRange_;
  out.dailyLowF = dailyLowF_;
  out.dailyHighF = dailyHighF_;
  strlcpy(out.shortForecast, shortForecast_, sizeof(out.shortForecast));
  out.precipitationChance = precipitationChance_;
  out.hasHumidity = hasHumidity_;
  out.humidityPct = humidityPct_;
  out.hasWind = hasWind_;
  out.windMph = windMph_;
  out.windDirectionDeg = windDirectionDeg_;
  out.hasPressure = hasPressure_;
  out.pressureInHg = pressureInHg_;
  out.pressureDeltaInHg = pressureDeltaInHg_;
  out.hasFeelsLike = hasFeelsLike_;
  out.feelsLikeF = feelsLikeF_;
  out.hasPrecipLastHour = hasPrecipLastHour_;
  out.precipLastHourIn = precipLastHourIn_;
  out.latitude = latitude_;
  out.longitude = longitude_;
  out.lastObservationStatus = lastObservationStatus_;
  out.lastForecastStatus = lastForecastStatus_;
  out.lastObservationRequest = lastObservationRequest_;
  out.lastForecastRequest = lastForecastRequest_;
  strlcpy(out.lastFailedRequest, lastFailedRequest_,
          sizeof(out.lastFailedRequest));
  out.lastTlsError = lastTlsError_;
  strlcpy(out.lastTlsErrorText, lastTlsErrorText_,
          sizeof(out.lastTlsErrorText));
  strlcpy(out.lastTransportDetail, lastTransportDetail_,
          sizeof(out.lastTransportDetail));
  strlcpy(out.observationDetails, observationDetails_,
          sizeof(out.observationDetails));
}
