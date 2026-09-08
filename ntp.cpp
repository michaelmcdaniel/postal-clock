#include "ntp.h"

#include <WiFi.h>
#include <time.h>
#include <sys/time.h>

namespace {
constexpr uint16_t kNtpPort = 123;
constexpr uint32_t kNtpToUnix = 2208988800UL;
constexpr char kNtpServer[] = "time.nist.gov";
constexpr uint32_t kResponseTimeoutMs = 1500;
constexpr uint32_t kRetryMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kSuccessIntervalMs = 24UL * 60UL * 60UL * 1000UL;
}

void NtpService::begin(const AppConfig& config, RtcService& rtc) {
  rtc_ = &rtc;
  posixTimezone_ = ConfigStore::posixTimezone(config.timezone);
  if (posixTimezone_) {
    setenv("TZ", posixTimezone_, 1);
    tzset();
    ClockDateTime current;
    if (rtc_->read(current)) {
      struct tm local {};
      local.tm_year = current.year - 1900;
      local.tm_mon = current.month - 1;
      local.tm_mday = current.day;
      local.tm_hour = current.hour;
      local.tm_min = current.minute;
      local.tm_sec = current.second;
      local.tm_isdst = -1;
      const time_t epoch = mktime(&local);
      if (epoch != static_cast<time_t>(-1)) {
        const struct timeval systemTime = {epoch, 0};
        if (settimeofday(&systemTime, nullptr) == 0) picoTimeValid_ = true;
      }
    }
  }
  udp_.begin(2390);
  nextAttemptMs_ = millis();
  nextDnsAttemptMs_ = millis();
}

void NtpService::serviceDns(uint32_t nowMs, bool wifiConnected) {
  if (hasServerAddress_ || !wifiConnected ||
      static_cast<int32_t>(nowMs - nextDnsAttemptMs_) < 0) {
    return;
  }

  // This may take roughly 15 seconds if DNS is unavailable. It is called only
  // from core 1 while the network mutex is held, so core 0 keeps servicing the
  // RTC, OLED, brightness, and watchdog instead of waiting here.
  IPAddress resolved;
  if (WiFi.hostByName(kNtpServer, resolved) != 1) {
    nextDnsAttemptMs_ = nowMs + kRetryMs;
    Serial.println(F("NTP: DNS lookup failed; retrying later"));
    return;
  }

  serverAddress_ = resolved;
  hasServerAddress_ = true;
  Serial.print(F("NTP: DNS resolved time.nist.gov to "));
  Serial.println(serverAddress_);
}

void NtpService::service(uint32_t nowMs, bool wifiConnected) {
  if (waiting_) {
    if (udp_.parsePacket() >= 48) {
      const bool success = applyResponse();
      waiting_ = false;
      nextAttemptMs_ = millis() + (success ? kSuccessIntervalMs : kRetryMs);
    } else if (static_cast<uint32_t>(nowMs - requestStartedMs_) >= kResponseTimeoutMs) {
      waiting_ = false;
      nextAttemptMs_ = nowMs + kRetryMs;
      Serial.println(F("NTP: response timed out"));
    }
    return;
  }
  if (wifiConnected && hasServerAddress_ &&
      static_cast<int32_t>(nowMs - nextAttemptMs_) >= 0) {
    if (!startRequest(nowMs)) nextAttemptMs_ = millis() + kRetryMs;
  }
}

bool NtpService::startRequest(uint32_t nowMs) {
  uint8_t packet[48] = {};
  // LI=0, VN=3, Mode=3 (client), matching the conventional NIST request.
  packet[0] = 0x1B;
  if (!hasServerAddress_) {
    // Core 0 normally filters this case before calling startRequest().
    Serial.println(F("NTP: no cached server address; skipping sync"));
    return false;
  }
  Serial.print(F("NTP: using cached server address "));
  Serial.println(serverAddress_);
  if (!udp_.beginPacket(serverAddress_, kNtpPort)) {
    Serial.println(F("NTP: UDP request setup failed"));
    return false;
  }
  udp_.write(packet, sizeof(packet));
  if (!udp_.endPacket()) {
    Serial.println(F("NTP: request send failed"));
    return false;
  }
  requestStartedMs_ = nowMs;
  waiting_ = true;
  return true;
}

bool NtpService::applyResponse() {
  if (udp_.remotePort() != kNtpPort || udp_.remoteIP() != serverAddress_) {
    Serial.println(F("NTP: rejected response from unexpected endpoint"));
    while (udp_.available()) udp_.read();
    return false;
  }
  uint8_t packet[48];
  if (udp_.read(packet, sizeof(packet)) != sizeof(packet)) return false;
  const uint8_t leapIndicator = packet[0] >> 6;
  const uint8_t mode = packet[0] & 0x07;
  const uint8_t stratum = packet[1];
  if (leapIndicator == 3 || (mode != 4 && mode != 5) ||
      stratum == 0 || stratum > 15) {
    Serial.println(F("NTP: rejected malformed or unsynchronized response"));
    return false;
  }
  const uint32_t ntpSeconds =
      (static_cast<uint32_t>(packet[40]) << 24) |
      (static_cast<uint32_t>(packet[41]) << 16) |
      (static_cast<uint32_t>(packet[42]) << 8) | packet[43];
  const uint32_t ntpFraction =
      (static_cast<uint32_t>(packet[44]) << 24) |
      (static_cast<uint32_t>(packet[45]) << 16) |
      (static_cast<uint32_t>(packet[46]) << 8) | packet[47];
  if (ntpSeconds <= kNtpToUnix || !posixTimezone_) return false;
  const time_t utc = static_cast<time_t>(ntpSeconds - kNtpToUnix);
  if (utc < 1577836800) return false;  // 2020-01-01 sanity floor.
  const uint32_t microseconds = static_cast<uint32_t>(
      (static_cast<uint64_t>(ntpFraction) * 1000000ULL) >> 32);
  const struct timeval systemTime = {utc, static_cast<suseconds_t>(microseconds)};
  if (settimeofday(&systemTime, nullptr) != 0) {
    Serial.println(F("NTP: could not set Pico system clock"));
    return false;
  }
  picoTimeValid_ = true;
  Serial.println(F("NTP: Pico system clock synchronized"));

  // The DS3231 stores whole seconds. Round the fractional NTP timestamp to
  // the nearest second instead of silently truncating it.
  const time_t rtcUtc = utc + (microseconds >= 500000U ? 1 : 0);
  struct tm local {};
  if (!localtime_r(&rtcUtc, &local)) return false;

  ClockDateTime corrected;
  corrected.year = local.tm_year + 1900;
  corrected.month = local.tm_mon + 1;
  corrected.day = local.tm_mday;
  corrected.weekday = local.tm_wday;
  corrected.hour = local.tm_hour;
  corrected.minute = local.tm_min;
  corrected.second = local.tm_sec;
  if (!RtcService::valid(corrected)) return false;

  bool shouldWrite = rtc_->oscillatorStopped();
  ClockDateTime current;
  if (!rtc_->read(current)) {
    shouldWrite = true;
  } else {
    struct tm currentLocal {};
    currentLocal.tm_year = current.year - 1900;
    currentLocal.tm_mon = current.month - 1;
    currentLocal.tm_mday = current.day;
    currentLocal.tm_hour = current.hour;
    currentLocal.tm_min = current.minute;
    currentLocal.tm_sec = current.second;
    currentLocal.tm_isdst = -1;
    const time_t currentEpoch = mktime(&currentLocal);
    if (currentEpoch == static_cast<time_t>(-1) ||
        llabs(static_cast<long long>(currentEpoch) - static_cast<long long>(rtcUtc)) > 2) {
      shouldWrite = true;
    }
  }
  if (!shouldWrite) {
    Serial.println(F("NTP: RTC already within tolerance"));
    return true;
  }
  if (!rtc_->write(corrected)) {
    Serial.println(F("NTP: RTC unavailable; continuing on Pico system clock"));
    return true;
  }
  Serial.println(F("NTP: corrected DS3231 local time"));
  return true;
}
