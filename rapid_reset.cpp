#include "rapid_reset.h"

#include <LittleFS.h>
#include <stddef.h>

namespace {
constexpr char kStatePathA[] = "/rapid-reset-a.bin";
constexpr char kStatePathB[] = "/rapid-reset-b.bin";
constexpr uint32_t kMagic = 0x52525354;  // "RRST"
constexpr uint32_t kWindowSeconds = 10;
constexpr uint8_t kBootsRequired = 3;

struct ResetState {
  uint32_t magic = kMagic;
  uint32_t generation = 0;
  uint32_t firstBootSeconds = 0;
  uint32_t lastBootSeconds = 0;
  uint8_t bootCount = 0;
  uint8_t reserved[3] = {};
  uint32_t checksum = 0;
};

static_assert(offsetof(ResetState, checksum) == 20,
              "ResetState layout changed; update its checksum calculation");

uint32_t checksum(const ResetState& state) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < offsetof(ResetState, checksum); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool readState(const char* path, ResetState& state) {
  File file = LittleFS.open(path, "r");
  if (!file || file.size() != sizeof(state)) {
    if (file) file.close();
    return false;
  }
  const size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(&state),
                                     sizeof(state));
  file.close();
  return bytesRead == sizeof(state) && state.magic == kMagic &&
         state.bootCount < kBootsRequired && state.checksum == checksum(state);
}

bool writeState(const char* path, ResetState& state) {
  state.checksum = checksum(state);
  File file = LittleFS.open(path, "w");
  if (!file) return false;
  const size_t bytesWritten =
      file.write(reinterpret_cast<const uint8_t*>(&state), sizeof(state));
  file.flush();
  file.close();
  if (bytesWritten != sizeof(state)) return false;

  ResetState verified;
  return readState(path, verified) &&
         verified.generation == state.generation &&
         verified.checksum == state.checksum;
}

bool newer(uint32_t left, uint32_t right) {
  return static_cast<int32_t>(left - right) > 0;
}

bool leapYear(uint16_t year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}
}  // namespace

bool RapidResetDetector::recordBoot(const ClockDateTime& now) const {
  if (!RtcService::valid(now)) return false;

  ResetState stateA;
  ResetState stateB;
  const bool validA = readState(kStatePathA, stateA);
  const bool validB = readState(kStatePathB, stateB);

  ResetState previous;
  const char* targetPath = kStatePathA;
  bool havePrevious = false;
  if (validA && (!validB || newer(stateA.generation, stateB.generation))) {
    previous = stateA;
    targetPath = kStatePathB;
    havePrevious = true;
  } else if (validB) {
    previous = stateB;
    targetPath = kStatePathA;
    havePrevious = true;
  }

  const uint32_t bootSeconds = secondsSince2020(now);
  ResetState next;
  next.generation = havePrevious ? previous.generation + 1U : 1U;
  const bool followsSequence =
      havePrevious && previous.bootCount > 0 &&
      bootSeconds >= previous.lastBootSeconds &&
      bootSeconds - previous.firstBootSeconds <= kWindowSeconds;
  if (followsSequence) {
    next.firstBootSeconds = previous.firstBootSeconds;
    next.bootCount = previous.bootCount + 1U;
  } else {
    next.firstBootSeconds = bootSeconds;
    next.bootCount = 1;
  }
  next.lastBootSeconds = bootSeconds;

  const bool triggered = next.bootCount >= kBootsRequired;
  if (triggered) {
    // Consume the gesture so the reboot after saving new settings starts fresh.
    next.firstBootSeconds = 0;
    next.lastBootSeconds = 0;
    next.bootCount = 0;
  }

  if (!writeState(targetPath, next)) {
    Serial.println(F("Rapid reset: could not persist boot sequence"));
  }
  return triggered;
}

uint32_t RapidResetDetector::secondsSince2020(const ClockDateTime& value) {
  static constexpr uint8_t daysPerMonth[] =
      {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint32_t days = 0;
  for (uint16_t year = 2020; year < value.year; ++year) {
    days += leapYear(year) ? 366U : 365U;
  }
  for (uint8_t month = 1; month < value.month; ++month) {
    days += daysPerMonth[month - 1];
    if (month == 2 && leapYear(value.year)) ++days;
  }
  days += value.day - 1U;
  return (((days * 24U) + value.hour) * 60U + value.minute) * 60U +
         value.second;
}
