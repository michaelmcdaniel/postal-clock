# Postal Clock reliability review (1.0.38)

## Root cause assessment

The most likely cause of the observed all-day freeze is the combination of three reliability defects in 1.0.37:

1. `setup1()/loop1()` enabled multicore without `core1_separate_stack = true`. Arduino-Pico therefore split the normal 8 KB stack into 4 KB per core. Core 1 was running BearSSL/TLS, HTTP and ArduinoJson on that 4 KB stack.
2. Core 0 and core 1 entered the same Wi-Fi/network stack concurrently (`WiFi`, web server/NTP on core 0; HTTPS on core 1).
3. Core 0 read `WeatherService` fields while core 1 mutated them, including character arrays and multi-field state, with no snapshot/mutex.

Those faults can produce intermittent corruption/deadlock rather than a repeatable immediate failure. There was also no hardware watchdog to restore clock operation after a main-loop stall.

## Fixes in 1.0.38

- Separate 8 KB core-1 stack.
- Cross-core network mutex; core 0 uses a non-blocking try-lock and never waits for weather.
- `WeatherSnapshot` is the only weather state consumed by core 0.
- Cached Wi-Fi state means display/info code no longer calls the Wi-Fi stack outside the mutex.
- 8-second hardware watchdog on the main clock loop.
- `require_qc=false` on the NWS `/observations/latest` request.
- Compact ArduinoJson array filters instead of retaining complete forecast/observation objects.
- Next-two-period low/high logic preserved; `isDaytime` decides which is low/high.
- 12-hour shortForecast/PoP remains as fallback when hourly retrieval fails.
- Forecast condensation is capped at 16 characters and does not cut words.
- Display weather clipping removes whole words and respects temperature/range boundaries.
- Configuration now rejects brightness 0, matching the UI's documented 1-255 range.

## File-by-file review

- `Clock2.ino` — **critical defects fixed:** undersized core-1 stack, concurrent network access, unsafe weather sharing, no watchdog. Main clock/UI now never waits for weather HTTPS.
- `weather.cpp` — **critical/medium defects fixed:** mutable state was shared cross-core; latest KMBT temperature could be null without explicit QC behavior; JSON filters retained far too much; old forecast condenser mislabeled precipitation and chopped text; hourly failure lost PoP fallback.
- `weather.h` — **critical defect fixed:** exposed raw mutable core-1 state through getters/pointers. Replaced core-0 consumption with bounded `WeatherSnapshot`.
- `wifi_manager.cpp/.h` — **critical defect fixed:** getters called `WiFi.*` outside synchronization while core 1 could be inside HTTPS. Getters now return cached state.
- `display.cpp` — **medium defect fixed:** weather text was trimmed one character at a time and the >=50% PoP branch ignored reserved left/right areas.
- `display.h` — no functional defect found.
- `light_sensor.cpp` — no functional defect found; two-level hysteresis is simple and appropriate.
- `light_sensor.h` — stale comment corrected; implementation remains direct SH1106 contrast.
- `rtc.cpp/.h` — no logic defect found. DS3231 remains authoritative and isolated on I2C1 GP6/GP7.
- `ntp.cpp/.h` — logic is sound; NTP is subordinate to DS3231 and only corrects meaningful drift. It still runs on core 0, but its network access is serialized and the watchdog protects a pathological stall.
- `app_config.cpp` — minor validation mismatch fixed: a hand-edited config could use brightness 0 although the UI says 1-255.
- `app_config.h` — no defect found.
- `provisioning.cpp/.h` — no correctness defect found. In normal mode it is now serviced only while core 0 owns the network mutex, so it cannot race core-1 HTTPS.
- `rapid_reset.cpp/.h` — no defect found. Dual-file state/verification is reasonable.
- `pins.h` — correct for this hardware: OLED GP0/1, RTC GP6/7, LDR GP26, button GP8; GP3/GP4 remain documented bad.
- `build.ps1` — no defect found; unchanged.
- `config.example.json` — no defect found; unchanged.
- `README.md` — stale runtime descriptions corrected for next-two-period low/high and reliability architecture.
- `AGENTS.md` — reliability invariants added so future agent changes do not remove stack separation, serialization, snapshots or watchdog.

## Runtime trace

Core 0: watchdog feed -> serial -> DS3231 -> GP8 -> LDR -> (try network mutex) Wi-Fi/web/NTP + copy WeatherSnapshot -> OLED every 250 ms.

Core 1: try network mutex -> copy the clock/Wi-Fi inputs -> perform due NWS work -> release mutex. During HTTPS, core 0 simply fails its try-lock and continues displaying DS3231 time from the last safe snapshot.

If core 0 ever blocks for more than 8 seconds, the hardware watchdog reboots the clock and the DS3231 restores correct time on startup.

## Build status

`build.ps1` is preserved. This review environment does not have `arduino-cli` or PowerShell installed, so the edited tree could not be compiled here. The code was statically checked for balanced delimiters and API usage was checked against the Pico SDK/Arduino-Pico interfaces, but the first build should be treated as the compile validation step.


## 1.0.39 additions

- Harvest humidity, wind, pressure, feels-like temperature, and last-hour precipitation from the existing NWS observation response.
- GP8 info page 1 shows that harvested snapshot.
- Connected configuration server exposes `/weather` and links to it from `/`; the page reads only the published snapshot and never performs a network fetch.

## 1.0.40 build compatibility fix

- Arduino-Pico 6.0.0 returns `uint8_t` from `WiFi.status()`; `WifiManager::service()` now stores that return value as `uint8_t` instead of `wl_status_t`.
- Removed the unused dewpoint JSON field/read from `weather.cpp`.
- The remaining `WiFiClient::write` / BearSSL messages are warnings inside the Arduino-Pico libraries, not project errors.
