#include <Arduino.h>

#include <WiFi.h>
#include <hardware/watchdog.h>
#include <pico/mutex.h>

#include "app_config.h"
#include "display.h"
#include "light_sensor.h"
#include "ntp.h"
#include "pins.h"
#include "provisioning.h"
#include "rapid_reset.h"
#include "rtc.h"
#include "weather.h"
#include "wifi_manager.h"

namespace {
constexpr char kSoftwareVersion[] = "1.0.54";
constexpr uint32_t kSplashDurationMs = 5000;
constexpr uint32_t kWatchdogTimeoutMs = 8000;

enum class AppMode : uint8_t { kSplash, kProvisioning, kNormal };

ConfigStore configStore;
AppConfig config;
ClockDisplay display;
RtcService rtc;
LightSensor lightSensor;
ProvisioningService provisioning;
RapidResetDetector rapidResetDetector;
WifiManager wifiManager;
WeatherService weather;
WeatherSnapshot weatherView;
NtpService ntp;
AppMode mode = AppMode::kSplash;
AppMode modeAfterSplash = AppMode::kProvisioning;
uint32_t splashStartedMs = 0;
uint32_t lastRtcReadMs = 0;
uint32_t lastDisplayMs = 0;
ClockDateTime currentTime;
bool rtcValid = false;
bool watchdogStarted = false;

// Every call into the Wi-Fi/network stack is serialized across the two cores.
// Core 0 only tries this mutex and never waits for weather HTTPS to finish.
mutex_t networkMutex;
volatile bool weatherCoreEnabled = false;
bool weatherWifiConnected = false;
bool weatherLocalTimeValid = false;
ClockDateTime weatherLocalTime;

uint32_t infoButtonPressedMs = 0;
bool infoScreenShown = false;
bool infoButtonHandled = false;
uint8_t infoPage = 0;
uint32_t infoScreenShownMs = 0;
bool resetInProgress = false;
uint8_t rtcFailures = 0;
char serialCommand[24] = {};
uint8_t serialCommandLength = 0;

bool tryNetworkLock() {
  uint32_t owner = 0;
  return mutex_try_enter(&networkMutex, &owner);
}

void releaseNetworkLock() { mutex_exit(&networkMutex); }

void startRuntimeWatchdog() {
  if (!watchdogStarted) {
    watchdog_enable(kWatchdogTimeoutMs, true);
    watchdogStarted = true;
    Serial.println(F("Watchdog: runtime watchdog enabled"));
  }
}

void enterProvisioning() {
  // Stop new weather work before changing Wi-Fi mode. If a request is already
  // in flight, wait for that one bounded operation to finish before touching
  // Wi-Fi from core 0.
  weatherCoreEnabled = false;
  mutex_enter_blocking(&networkMutex);
  mode = AppMode::kProvisioning;
  Serial.println(F("Application: entering provisioning mode"));
  provisioning.begin(configStore);
  releaseNetworkLock();
  display.showProvisioning(provisioning.address(), configStore.mounted());
}

void enterNormal() {
  mode = AppMode::kNormal;
  Serial.println(F("Application: entering normal clock mode"));
  lightSensor.begin(config.brightnessMin, config.brightnessMax,
                    config.brightnessCutoff);

  // Core 1 is still disabled here, so all network objects can be initialized
  // safely on core 0 before weather is released.
  Serial.println(F("Application: normal init: WiFi start"));
  wifiManager.begin(config);
  Serial.println(F("Application: normal init: web server"));
  provisioning.beginConnected(configStore, config);
  Serial.println(F("Application: normal init: weather"));
  weather.begin(config);
  weather.snapshot(weatherView, millis());
  provisioning.setWeatherSnapshot(weatherView);
  Serial.println(F("Application: normal init: NTP"));
  ntp.begin(config, rtc);
  weatherCoreEnabled = true;
  lastDisplayMs = 0;
  Serial.println(F("Application: normal init complete"));
}

void serviceSerial() {
  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\r' || character == '\n') {
      serialCommand[serialCommandLength] = '\0';
      if (mode == AppMode::kNormal &&
          strcmp(serialCommand, "provision") == 0) {
        Serial.println(
            F("Application: accepted deliberate provisioning command"));
        enterProvisioning();
      } else if (serialCommandLength > 0) {
        Serial.println(F("Commands: provision"));
      }
      serialCommandLength = 0;
    } else if (static_cast<size_t>(serialCommandLength) + 1U <
               sizeof(serialCommand)) {
      serialCommand[serialCommandLength++] = character;
    } else {
      serialCommandLength = 0;
    }
  }
}

void serviceRtc(uint32_t nowMs) {
  if (static_cast<uint32_t>(nowMs - lastRtcReadMs) < 250) return;
  lastRtcReadMs = nowMs;
  ClockDateTime reading;
  if (rtc.read(reading)) {
    currentTime = reading;
    rtcValid = true;
    rtcFailures = 0;
  } else if (++rtcFailures >= 4) {
    rtcValid = false;
  }
}

bool readPicoTime(ClockDateTime& value) {
  if (!ntp.picoTimeValid()) return false;
  const time_t epoch = time(nullptr);
  struct tm local {};
  if (epoch < 1577836800 || !localtime_r(&epoch, &local)) return false;
  ClockDateTime candidate;
  candidate.year = local.tm_year + 1900;
  candidate.month = local.tm_mon + 1;
  candidate.day = local.tm_mday;
  candidate.weekday = local.tm_wday;
  candidate.hour = local.tm_hour;
  candidate.minute = local.tm_min;
  candidate.second = local.tm_sec;
  if (!RtcService::valid(candidate)) return false;
  value = candidate;
  return true;
}

void serviceInfoButton(uint32_t nowMs) {
  const bool pressed = digitalRead(Pins::kInfoButton) == LOW;
  if (!pressed) {
    infoButtonPressedMs = 0;
    infoButtonHandled = false;
    resetInProgress = false;
    if (infoScreenShown &&
        static_cast<uint32_t>(nowMs - infoScreenShownMs) >= 5000) {
      infoScreenShown = false;
    }
    return;
  }

  if (infoButtonPressedMs == 0) infoButtonPressedMs = nowMs;
  const uint32_t heldMs = nowMs - infoButtonPressedMs;

  if (heldMs >= 10000 && !resetInProgress) {
    resetInProgress = true;
    Serial.println(
        F("Application: reset button held for 10 seconds; clearing configuration"));

    // LittleFS writes pause the other core. Do not erase flash while core 1 is
    // inside TLS/network code.
    weatherCoreEnabled = false;
    mutex_enter_blocking(&networkMutex);
    configStore.clear();
    releaseNetworkLock();
    delay(50);
    rp2040.reboot();
    return;
  }

  if (heldMs >= 100 && !infoButtonHandled && mode == AppMode::kNormal) {
    infoButtonHandled = true;
    infoScreenShown = true;
    infoScreenShownMs = nowMs;
    infoPage = static_cast<uint8_t>((infoPage + 1) % 3);

    char configUrl[32] = {};
    if (wifiManager.connected()) {
      const IPAddress address = wifiManager.localAddress();
      snprintf(configUrl, sizeof(configUrl), "http://%u.%u.%u.%u/",
               address[0], address[1], address[2], address[3]);
    }

    display.showInfo(
        infoPage, config.wifiSsid, configUrl[0] ? configUrl : nullptr,
        kSoftwareVersion, config.weatherStationId, weatherView);
  }
}

}  // namespace

// Bare-metal Arduino-Pico runs setup1()/loop1() on core 1. Keep slow weather
// DNS/TLS/HTTP and NTP DNS here; core 0 never waits for this work.
bool core1_separate_stack = true;

void setup1() {}

void loop1() {
  if (weatherCoreEnabled && tryNetworkLock()) {
    if (weatherCoreEnabled) {
      const ClockDateTime localTime = weatherLocalTime;
      const bool wifiConnected = weatherWifiConnected;
      const bool localTimeValid = weatherLocalTimeValid;
      weather.service(millis(), wifiConnected, localTime, localTimeValid);
      // DNS resolution can block for much longer than an NTP reply timeout.
      // Keep it on core 1 with the rest of slow network work.
      ntp.serviceDns(millis(), wifiConnected);
    }
    releaseNetworkLock();
  }
  delay(5);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.print(F("Postal Clock software "));
  Serial.println(kSoftwareVersion);

  if (watchdog_enable_caused_reboot()) {
    Serial.println(F("Watchdog: previous run stalled; automatic recovery succeeded"));
  }

  mutex_init(&networkMutex);

  display.begin();
  pinMode(Pins::kInfoButton, INPUT_PULLUP);
  display.showSplash(kSoftwareVersion);
  splashStartedMs = millis();

  const bool rtcPresent = rtc.begin();
  Serial.println(rtcPresent ? F("RTC: DS3231 detected")
                            : F("RTC: DS3231 not detected"));
  serviceRtc(millis() + 250);

  String configReason;
  if (!configStore.begin()) {
    Serial.println(
        F("Config: LittleFS mount failed; setup can run but cannot save"));
    modeAfterSplash = AppMode::kProvisioning;
  } else if (!configStore.load(config, &configReason)) {
    Serial.print(F("Config: invalid: "));
    Serial.println(configReason);
    modeAfterSplash = AppMode::kProvisioning;
  } else {
    Serial.println(F("Config: valid configuration loaded"));
    const bool rtcReliable = rtcValid && !rtc.oscillatorStopped();
    if (rtcReliable && rapidResetDetector.recordBoot(currentTime)) {
      Serial.println(
          F("Application: three rapid resets detected; setup requested"));
      modeAfterSplash = AppMode::kProvisioning;
    } else {
      if (!rtcReliable) {
        Serial.println(F("Rapid reset: skipped because RTC time is not reliable"));
      }
      modeAfterSplash = AppMode::kNormal;
    }
  }

  lightSensor.begin(
      modeAfterSplash == AppMode::kNormal ? config.brightnessMin : 1,
      modeAfterSplash == AppMode::kNormal ? config.brightnessMax : 255,
      modeAfterSplash == AppMode::kNormal ? config.brightnessCutoff : 300);

}

void loop() {
  // Start only after the splash transition has completed. From then on, a
  // main-core stall returns the appliance to DS3231-backed clock operation.
  if (mode != AppMode::kSplash) startRuntimeWatchdog();
  if (watchdogStarted) watchdog_update();

  const uint32_t nowMs = millis();
  serviceSerial();
  serviceRtc(nowMs);
  serviceInfoButton(nowMs);

  uint8_t brightness;
  if (lightSensor.service(nowMs, brightness)) {
    display.setBrightness(brightness);
  }
  provisioning.setLightValue(lightSensor.value());

  if (mode == AppMode::kSplash) {
    if (static_cast<uint32_t>(nowMs - splashStartedMs) >=
        kSplashDurationMs) {
      if (modeAfterSplash == AppMode::kNormal) {
        enterNormal();
      } else {
        enterProvisioning();
      }
    }
    return;
  }

  if (mode == AppMode::kProvisioning) {
    // Weather core is disabled in this mode, so direct Wi-Fi servicing is safe.
    provisioning.service(nowMs);
    return;
  }

  // Never wait for weather HTTPS. If core 1 owns the network stack, keep the
  // clock/display moving and use the last published Wi-Fi/weather snapshot.
  if (tryNetworkLock()) {
    wifiManager.service(nowMs);

    weatherWifiConnected = wifiManager.connected();
    weatherLocalTime = currentTime;
    weatherLocalTimeValid = rtcValid;

    ntp.service(nowMs, wifiManager.connected());
    weather.snapshot(weatherView, nowMs);

    // The configuration web server and info screen consume only this
    // core-0 snapshot. They never touch WeatherService while core 1 is
    // harvesting NOAA data.
    provisioning.setWeatherSnapshot(weatherView);
    provisioning.service(nowMs);
    releaseNetworkLock();
  }

  if (!infoScreenShown &&
      static_cast<uint32_t>(nowMs - lastDisplayMs) >= 250) {
    lastDisplayMs = nowMs;

    ClockDateTime displayTime = currentTime;
    bool displayTimeValid = rtcValid;
    if (!displayTimeValid) displayTimeValid = readPicoTime(displayTime);

    const WifiIconState wifiState =
        wifiManager.connected()
            ? WifiIconState::kConnected
            : (wifiManager.connecting() ? WifiIconState::kConnecting
                                        : WifiIconState::kDisconnected);

    display.showClock(
        displayTime, displayTimeValid, weatherView.hasTemperature,
        weatherView.temperatureF, weatherView.temperatureStale,
        weatherView.hasDailyRange, weatherView.dailyLowF,
        weatherView.dailyHighF, weatherView.shortForecast,
        weatherView.precipitationChance, wifiState, wifiManager.signalBars(),
        nowMs);
  }
}
