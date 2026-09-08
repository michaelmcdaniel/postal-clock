#include "display.h"

#include <Wire.h>

#include "pins.h"

namespace {
// Raspberry Pi Logo (32x32), horizontal monochrome bitmap.
const uint8_t kRaspberryPiLogo[] PROGMEM = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7c,0x3f,0x00,
    0x01,0x86,0x40,0x80,0x01,0x01,0x80,0x80,0x01,0x11,0x88,0x80,0x01,0x05,0xa0,0x80,
    0x00,0x83,0xc1,0x00,0x00,0x43,0xe3,0x00,0x00,0x7e,0xfc,0x00,0x00,0x4c,0x27,0x00,
    0x00,0x9c,0x11,0x00,0x00,0xbf,0xfd,0x00,0x00,0xe1,0x87,0x00,0x01,0xc1,0x83,0x80,
    0x02,0x41,0x82,0x40,0x02,0x41,0x82,0x40,0x02,0xc1,0xc2,0x40,0x02,0xf6,0x3e,0xc0,
    0x01,0xfc,0x3d,0x80,0x01,0x18,0x18,0x80,0x01,0x88,0x10,0x80,0x00,0x8c,0x21,0x00,
    0x00,0x87,0xf1,0x00,0x00,0x7f,0xf6,0x00,0x00,0x38,0x1c,0x00,0x00,0x0c,0x20,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00};

struct Point { int8_t x; int8_t y; };
constexpr Point kSegments[7][7] = {
    {{2,0},{17,0},{13,4},{6,4},{2,0},{2,0},{2,0}},
    {{0,2},{0,16},{4,12},{4,6},{0,2},{0,2},{0,2}},
    {{19,2},{19,16},{15,12},{15,6},{19,2},{19,2},{19,2}},
    {{5,16},{14,16},{16,18},{14,20},{5,20},{3,18},{5,16}},
    {{0,20},{0,35},{4,31},{4,24},{0,20},{0,20},{0,20}},
    {{19,20},{19,35},{15,31},{15,24},{19,20},{19,20},{19,20}},
    {{2,37},{17,37},{13,33},{6,33},{2,37},{2,37},{2,37}}};
constexpr uint8_t kPointCounts[] = {5,5,5,7,5,5,5};
constexpr uint8_t kDigitMask[] = {
    0b1110111, 0b0100100, 0b1011101, 0b1101101, 0b0101110,
    0b1101011, 0b1111011, 0b0100101, 0b1111111, 0b1101111};
const char* const kWeekdays[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY",
                                 "THURSDAY", "FRIDAY", "SATURDAY"};
const char* const kShortWeekdays[] = {"SUND", "MOND", "TUES", "WEDN", "THUR", "FRID", "SATU"};
const char* const kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                               "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
constexpr int16_t kDigitWidth = 20;
constexpr int16_t kDigitGap = 3;
constexpr int16_t kColonWidth = 12;
constexpr int16_t kTimeTop = 14;
constexpr int16_t kOneLeftBlankPixels = 15;

}

ClockDisplay::ClockDisplay()
    : oled_(U8G2_R0, U8X8_PIN_NONE) {}

bool ClockDisplay::begin() {
  Wire.setSDA(Pins::kOledSda);
  Wire.setSCL(Pins::kOledScl);
  Wire.begin();
  Wire.setClock(400000);
  return oled_.begin();
}

void ClockDisplay::drawRaspberryPiLogo(int16_t x, int16_t y) {
  // The supplied buffer is horizontal/MSB-first (Adafruit drawBitmap style),
  // so render each row explicitly instead of treating it as an XBM/LSB image.
  for (uint8_t row = 0; row < 32; ++row) {
    for (uint8_t column = 0; column < 32; ++column) {
      const uint8_t byteValue =
          pgm_read_byte(&kRaspberryPiLogo[row * 4 + column / 8]);
      if (byteValue & (0x80U >> (column & 7U))) {
        oled_.drawPixel(x + column, y + row);
      }
    }
  }
}

void ClockDisplay::showSplash(const char* softwareVersion) {
  oled_.clearBuffer();
  drawRaspberryPiLogo(94, 0);
  oled_.setFont(u8g2_font_6x10_tf);
  oled_.drawStr(2, 12, "Postal Clock");
  oled_.drawStr(2, 28, "Raspberry Pi");
  oled_.drawStr(2, 40, "Pico 2 W");
  char line[24];
  snprintf(line, sizeof(line), "Software %s", softwareVersion);
  oled_.drawStr(2, 50, line);
  oled_.setFont(u8g2_font_5x8_tf);
  oled_.drawUTF8(2, 63, "Michael McDaniel - 2026");
  oled_.sendBuffer();
}

void ClockDisplay::showInfo(uint8_t page, const char* networkName,
                            const char* configUrl, const char* softwareVersion,
                            const char* stationId,
                            const WeatherSnapshot& weather) {
  oled_.clearBuffer();
  oled_.setFont(u8g2_font_5x8_tf);
  char line[32];

  if (page == 1) {
    // Human-readable snapshot of the last station observation harvested by
    // WeatherService.  No network call is made when the button is pressed.
    snprintf(line, sizeof(line), "Weather %s", stationId ? stationId : "");
    oled_.drawStr(0, 8, line);

    if (weather.hasTemperature) {
      if (weather.hasHumidity) {
        snprintf(line, sizeof(line), "%dF  RH %u%%",
                 weather.temperatureF, weather.humidityPct);
      } else {
        snprintf(line, sizeof(line), "%dF  RH --", weather.temperatureF);
      }
    } else {
      strlcpy(line, "Temp --  RH --", sizeof(line));
    }
    oled_.drawStr(0, 18, line);

    if (weather.hasFeelsLike) {
      snprintf(line, sizeof(line), "Feels %dF", weather.feelsLikeF);
    } else {
      strlcpy(line, "Feels --", sizeof(line));
    }
    oled_.drawStr(0, 28, line);

    if (weather.hasWind) {
      static const char* const directions[] = {
          "N", "NE", "E", "SE", "S", "SW", "W", "NW"};
      const char* direction = "--";
      if (weather.windDirectionDeg >= 0) {
        const uint8_t index =
            static_cast<uint8_t>((weather.windDirectionDeg + 22) / 45) & 7U;
        direction = directions[index];
      }
      snprintf(line, sizeof(line), "Wind %.0f mph %s",
               weather.windMph, direction);
    } else {
      strlcpy(line, "Wind --", sizeof(line));
    }
    oled_.drawStr(0, 38, line);

    if (weather.hasPressure) {
      snprintf(line, sizeof(line), "Press %+.2f in",
               weather.pressureDeltaInHg);
    } else {
      strlcpy(line, "Press --", sizeof(line));
    }
    oled_.drawStr(0, 48, line);

    if (weather.hasPrecipLastHour) {
      snprintf(line, sizeof(line), "Rain 1h %.2f in",
               weather.precipLastHourIn);
    } else {
      strlcpy(line, "Rain 1h --", sizeof(line));
    }
    oled_.drawStr(0, 60, line);
    oled_.sendBuffer();
    return;
  }

  if (page == 2) {
    oled_.drawStr(0, 8, "Weather diagnostics");
    snprintf(line, sizeof(line), "Now HTTP %d %02u:%02u",
             weather.lastObservationStatus, weather.lastObservationRequest.hour,
             weather.lastObservationRequest.minute);
    oled_.drawStr(0, 20, line);
    snprintf(line, sizeof(line), "Fcst HTTP %d %02u:%02u",
             weather.lastForecastStatus, weather.lastForecastRequest.hour,
             weather.lastForecastRequest.minute);
    oled_.drawStr(0, 32, line);
    if (weather.lastFailedRequest[0]) {
      snprintf(line, sizeof(line), "Fail: %s", weather.lastFailedRequest);
    } else {
      strlcpy(line, "Fail: --", sizeof(line));
    }
    oled_.drawStr(0, 42, line);

    if (weather.lastTransportDetail[0]) {
      strlcpy(line, weather.lastTransportDetail, sizeof(line));
      // 5x8 font fits roughly 25 characters across the 128-pixel panel.
      line[25] = '\0';
    } else {
      strlcpy(line, "Transport: --", sizeof(line));
    }
    oled_.drawStr(0, 51, line);

    if (weather.lastTlsErrorText[0]) {
      strlcpy(line, weather.lastTlsErrorText, sizeof(line));
      line[25] = '\0';
    } else {
      snprintf(line, sizeof(line), "TLS: %d", weather.lastTlsError);
    }
    oled_.drawStr(0, 60, line);
    oled_.sendBuffer();
    return;
  }

  snprintf(line, sizeof(line), "Wi-Fi: %s",
           networkName ? networkName : "offline");
  oled_.drawStr(0, 8, line);
  oled_.drawStr(0, 18, configUrl ? configUrl : "Wi-Fi not connected");
  snprintf(line, sizeof(line), "Version %s", softwareVersion);
  oled_.drawStr(0, 28, line);
  oled_.drawStr(0, 38, "Michael McDaniel - 2026");
  oled_.drawStr(0, 60, "Press 10s to reset");
  oled_.sendBuffer();
}

void ClockDisplay::showProvisioning(const IPAddress& address, bool storageReady) {
  oled_.clearBuffer();
  oled_.setFont(u8g2_font_6x10_tf);
  oled_.drawStr(0, 9, storageReady ? "SETUP REQUIRED" : "STORAGE ERROR");
  oled_.drawStr(0, 24, "Join Wi-Fi:");
  oled_.drawStr(0, 35, "PostalClock-Setup");
  oled_.drawStr(0, 50, "Open in browser:");
  char ip[20];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
  oled_.drawStr(0, 62, ip);
  oled_.sendBuffer();
}

void ClockDisplay::drawDigit(uint8_t digit, int16_t x, int16_t y) {
  if (digit > 9) return;
  for (uint8_t segment = 0; segment < 7; ++segment) {
    if ((kDigitMask[digit] & (1U << segment)) == 0) continue;
    const Point* points = kSegments[segment];
    for (uint8_t i = 1; i + 1 < kPointCounts[segment]; ++i) {
      oled_.drawTriangle(x + points[0].x, y + points[0].y,
                         x + points[i].x, y + points[i].y,
                         x + points[i + 1].x, y + points[i + 1].y);
    }
  }
}

void ClockDisplay::showClock(const ClockDateTime& now, bool rtcValid,
                             bool hasWeather, int16_t temperatureF,
                             bool weatherStale, bool hasDailyRange,
                             int16_t dailyLowF, int16_t dailyHighF,
                             const char* shortForecast, int precipitationChance,
                             WifiIconState wifiState,
                             uint8_t wifiBars,
                             uint32_t nowMs) {
  oled_.clearBuffer();
  oled_.setFont(u8g2_font_5x8_tf);
  if (!rtcValid) {
    oled_.drawStr(0, 9, "RTC TIME INVALID");
    oled_.drawStr(0, 22, "Waiting for NTP");
  } else {
    uint8_t hour = now.hour % 12;
    if (hour == 0) hour = 12;

    // A one-digit hour deliberately takes one digit's width; calculate the
    // left edge from the visible glyphs so the whole time stays centered.
    const bool twoDigitHour = hour >= 10;
    const int16_t timeWidth =
        (twoDigitHour ? (2 * kDigitWidth + kDigitGap) : kDigitWidth) +
        kColonWidth + (2 * kDigitWidth + kDigitGap);
    // The segmented 1 occupies only the rightmost five pixels of its
    // 20-pixel cell. Center the illuminated bounds, not the empty left space.
    const uint8_t leadingDigit = twoDigitHour ? hour / 10 : hour;
    const int16_t leadingBlank = leadingDigit == 1 ? kOneLeftBlankPixels : 0;
    int16_t x = (128 - (timeWidth - leadingBlank)) / 2 - leadingBlank;
    if (twoDigitHour) {
      drawDigit(hour / 10, x, kTimeTop);
      x += kDigitWidth + kDigitGap;
    }
    drawDigit(hour % 10, x, kTimeTop);
    x += kDigitWidth;
    if ((now.second & 1U) == 0) {
      const int16_t colonX = x + 5;
      oled_.drawBox(colonX, kTimeTop + 10, 4, 4);
      oled_.drawBox(colonX, kTimeTop + 23, 4, 4);
    }
    x += kColonWidth;
    drawDigit(now.minute / 10, x, kTimeTop);
    x += kDigitWidth + kDigitGap;
    drawDigit(now.minute % 10, x, kTimeTop);
  }

  char top[16];
  if (hasWeather) {
    snprintf(top, sizeof(top), "%d", temperatureF);
  } else {
    snprintf(top, sizeof(top), "--");
  }
  oled_.setFont(u8g2_font_7x14_tf);
  oled_.drawStr(2, 13, top);
  int16_t topWidth = oled_.getStrWidth(top);
  oled_.setFont(u8g2_font_5x8_tf);
  const bool showWifiDot = wifiState == WifiIconState::kConnected ||
      (wifiState == WifiIconState::kConnecting &&
       ((nowMs / 400U) & 1U) == 0U);
  if (showWifiDot) {
    drawWifiDot(3 + topWidth, 4);
    topWidth += 5;
  }
  if (hasWeather && weatherStale) {
    oled_.drawStr(3 + topWidth, 11, "*");
    topWidth += oled_.getStrWidth("*") + 1;
  }

  char range[16];
  if (hasDailyRange) {
    snprintf(range, sizeof(range), "%d/%d", dailyLowF, dailyHighF);
  } else {
    range[0] = '\0';
  }
  const int16_t rangeWidth = oled_.getStrWidth(range);
  if (range[0]) oled_.drawStr(126 - rangeWidth, 8, range);

  if (shortForecast && shortForecast[0]) {
    char summary[32];
    strlcpy(summary, shortForecast, sizeof(summary));
    const int16_t leftEdge = 4 + topWidth;
    const int16_t rightEdge = 123 - rangeWidth;
    const int16_t availableWidth = max<int16_t>(0, rightEdge - leftEdge);

    // Never make a weather word ugly just to squeeze it in. Remove complete
    // trailing words until the text fits between temperature and daily range.
    auto trimSummaryToWidth = [&](int16_t width) {
      while (summary[0] && oled_.getStrWidth(summary) > width) {
        char* lastSpace = strrchr(summary, ' ');
        if (!lastSpace) {
          summary[0] = '\0';
          break;
        }
        *lastSpace = '\0';
      }
    };

    if (precipitationChance >= 50) {
      char rain[12];
      snprintf(rain, sizeof(rain), "%d%%", precipitationChance);
      const int16_t rainWidth = oled_.getStrWidth(rain);
      constexpr int16_t kDotAndGapWidth = 10;
      const int16_t summaryAllowance =
          max<int16_t>(0, availableWidth - rainWidth - kDotAndGapWidth);
      trimSummaryToWidth(summaryAllowance);

      const int16_t summaryWidth = oled_.getStrWidth(summary);
      const int16_t totalWidth =
          rainWidth + (summary[0] ? kDotAndGapWidth + summaryWidth : 0);
      int16_t x = leftEdge + max<int16_t>(0, (availableWidth - totalWidth) / 2);
      oled_.drawStr(x, 8, rain);
      if (summary[0]) {
        oled_.drawDisc(x + rainWidth + 5, 5, 2);
        oled_.drawStr(x + rainWidth + kDotAndGapWidth, 8, summary);
      }
    } else {
      trimSummaryToWidth(availableWidth);
      if (summary[0]) {
        const int16_t summaryWidth = oled_.getStrWidth(summary);
        const int16_t summaryX =
            leftEdge + max<int16_t>(0, (availableWidth - summaryWidth) / 2);
        oled_.drawStr(summaryX, 8, summary);
      }
    }
  }

  (void)wifiBars;

  if (rtcValid) {
    const char* weekday = kWeekdays[now.weekday];
    const char* month = now.month >= 1 && now.month <= 12
        ? kMonths[now.month - 1] : "???";
    char date[24];
    snprintf(date, sizeof(date), "%s %s %u", weekday, month, now.day);
    if (oled_.getStrWidth(date) > 128) {
      snprintf(date, sizeof(date), "%s %s %u", kShortWeekdays[now.weekday],
               month, now.day);
    }
    const int16_t dateWidth = oled_.getStrWidth(date);
    oled_.drawStr(max<int16_t>(0, (128 - dateWidth) / 2), 63, date);
  }
  oled_.sendBuffer();
}

void ClockDisplay::drawWifiDot(int16_t x, int16_t y) {
  oled_.drawPixel(x + 1, y);
  oled_.drawPixel(x, y + 1);
  oled_.drawPixel(x + 2, y + 1);
  oled_.drawPixel(x + 1, y + 2);
}

void ClockDisplay::setBrightness(uint8_t brightness) {
  oled_.setContrast(brightness);
}
