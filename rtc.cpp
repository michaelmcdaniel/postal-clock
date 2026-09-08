#include "rtc.h"

#include "pins.h"

namespace {
constexpr uint8_t kRtcAddress = 0x68;
}

bool RtcService::begin() {
  wire_->setSDA(Pins::kRtcSda);
  wire_->setSCL(Pins::kRtcScl);
  wire_->begin();
  wire_->setClock(100000);
  wire_->beginTransmission(kRtcAddress);
  return wire_->endTransmission() == 0;
}

bool RtcService::readRegisters(uint8_t start, uint8_t* data, size_t length) {
  wire_->beginTransmission(kRtcAddress);
  wire_->write(start);
  if (wire_->endTransmission(false) != 0) return false;
  const size_t received = wire_->requestFrom(kRtcAddress, length);
  if (received != length) {
    while (wire_->available()) wire_->read();
    return false;
  }
  for (size_t i = 0; i < length; ++i) data[i] = wire_->read();
  return true;
}

bool RtcService::read(ClockDateTime& value) {
  uint8_t data[7];
  if (!readRegisters(0x00, data, sizeof(data))) return false;

  ClockDateTime candidate;
  candidate.second = fromBcd(data[0] & 0x7F);
  candidate.minute = fromBcd(data[1] & 0x7F);
  if (data[2] & 0x40) {
    candidate.hour = fromBcd(data[2] & 0x1F);
    if (candidate.hour == 12) candidate.hour = 0;
    if (data[2] & 0x20) candidate.hour += 12;
  } else {
    candidate.hour = fromBcd(data[2] & 0x3F);
  }
  const uint8_t rtcWeekday = data[3] & 0x07;
  candidate.weekday = rtcWeekday >= 1 ? (rtcWeekday - 1) : 0;
  candidate.day = fromBcd(data[4] & 0x3F);
  candidate.month = fromBcd(data[5] & 0x1F);
  candidate.year = 2000 + fromBcd(data[6]);
  if (!valid(candidate)) return false;
  value = candidate;
  return true;
}

bool RtcService::write(const ClockDateTime& value) {
  if (!valid(value)) return false;
  const uint8_t data[7] = {
      toBcd(value.second), toBcd(value.minute), toBcd(value.hour),
      toBcd((value.weekday % 7) + 1), toBcd(value.day),
      toBcd(value.month), toBcd(value.year - 2000)};
  wire_->beginTransmission(kRtcAddress);
  wire_->write(0x00);
  wire_->write(data, sizeof(data));
  if (wire_->endTransmission() != 0) return false;

  uint8_t status;
  if (readRegisters(0x0F, &status, 1)) {
    wire_->beginTransmission(kRtcAddress);
    wire_->write(0x0F);
    wire_->write(status & ~0x80);
    wire_->endTransmission();
  }
  return true;
}

bool RtcService::oscillatorStopped() {
  uint8_t status = 0x80;
  return !readRegisters(0x0F, &status, 1) || (status & 0x80) != 0;
}

bool RtcService::valid(const ClockDateTime& value) {
  if (value.year < 2020 || value.year > 2099 || value.month < 1 ||
      value.month > 12 || value.hour > 23 || value.minute > 59 ||
      value.second > 59 || value.weekday > 6) return false;
  static constexpr uint8_t daysPerMonth[] =
      {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint8_t days = daysPerMonth[value.month - 1];
  if (value.month == 2 && value.year % 4 == 0) ++days;
  return value.day >= 1 && value.day <= days;
}

uint8_t RtcService::fromBcd(uint8_t value) {
  return ((value >> 4) * 10) + (value & 0x0F);
}

uint8_t RtcService::toBcd(uint8_t value) {
  return ((value / 10) << 4) | (value % 10);
}

