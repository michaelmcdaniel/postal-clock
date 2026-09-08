# Postal Clock firmware

![Finished Postal Clock](https://raw.githubusercontent.com/michaelmcdaniel/postal-clock/main/images/clock.jpg)

An antique U.S. Post Office box door and coin bank, rebuilt as a dependable
nightstand clock. The original brass hardware, combination lock, glass window,
coin slot, and coin-storage space remain usable; the modern electronics are
hidden behind the postal-box window.

The firmware runs on a Raspberry Pi Pico 2 W. It presents a simple high-
contrast OLED clock, keeps time from a battery-backed DS3231 RTC, dims itself
for a dark room, and adds local NWS weather when Wi-Fi is available. It is
designed to remain a clock—not a network appliance—when the Internet is down.

## At a glance

- Large, readable time, date, day of week, current outdoor temperature, and
  compact forecast on a 128×64 monochrome OLED.
- DS3231-backed local time: the display works without Wi-Fi after setup.
- Automatic display dimming from a photocell.
- Phone-friendly first-boot setup at the `PostalClock-Setup` Wi-Fi access
  point; no app required.
- NWS weather and occasional NTP correction are optional enhancements, never
  prerequisites for displaying the time.
- Wi-Fi retries, weather retrieval, and TLS work run away from the critical
  display/RTC loop. A watchdog recovers from an unexpected main-loop stall.
- A short press of the concealed information button shows weather/network
  diagnostics; holding it for ten seconds deliberately clears configuration.

## Gallery

| Finished clock | Electronics behind the postal door |
| --- | --- |
| ![Postal Clock front](https://raw.githubusercontent.com/michaelmcdaniel/postal-clock/main/images/clock.jpg) | ![Postal Clock internals](https://raw.githubusercontent.com/michaelmcdaniel/postal-clock/main/images/internals.jpg) |

## First use

1. Power the clock. With no saved configuration it shows a setup screen and
   creates the open `PostalClock-Setup` Wi-Fi network.
2. Join that network from a phone or computer and open `http://192.168.4.1/`
   if the captive page does not appear automatically.
3. Choose or enter the home Wi-Fi network, password, timezone, contact email,
   and a four-character NWS station identifier such as `KMBT`.
4. Save. The clock restarts into normal operation and continues to show local
   time even if Wi-Fi or weather service later becomes unavailable.

## Hardware

| Component | Role / connection |
| --- | --- |
| Raspberry Pi Pico 2 W | Controller and Wi-Fi |
| SH1106 128×64 OLED | I2C0 — SDA GP0, SCL GP1 |
| DS3231 RTC | I2C1 — SDA GP6, SCL GP7 |
| Photocell / LDR | ADC GP26 |
| Information/reset button | GP8 to ground, internal pull-up |

GP3 and GP4 are deliberately unused on this board. Confirm that the installed
DS3231 module’s I2C pull-ups are powered from a Pico-safe 3.3 V rail.

## How it stays reliable

The DS3231 is the authoritative clock. The Pico displays its time immediately
at boot, while Wi-Fi connects independently in the background. Slow DNS, HTTPS,
weather JSON, and NTP work execute on the second core; the first core owns the
OLED, RTC, brightness sensor, controls, and watchdog. Failed weather requests
leave the last good temperature visible and marked stale rather than blanking
the display.

## Hardware values preserved from `python/main.py`

| Device | Bus / pin |
|---|---|
| SH1106 128x64 OLED | I2C0: SDA GP0, SCL GP1, 400 kHz |
| DS3231 RTC | I2C1: SDA GP6, SCL GP7, 100 kHz |
| Photocell / LDR | ADC GP26 |
| Information/reset button | GP8 to GND; active-low with internal pull-up |

GP3 and GP4 are unusable on this board and must not be assigned to peripherals or user controls. Confirm that the installed DS3231 board's I2C pull-ups are tied to a Pico-safe 3.3 V rail before powering it.

## Build

The project uses the Earle Philhower Arduino-Pico core and the `rpipico2w` board target. Required libraries are:

- U8g2
- ArduinoJson 7

Validated versions:

- Arduino-Pico core 6.0.0
- U8g2 2.36.19
- ArduinoJson 7.4.3

With Arduino CLI installed:

```text
arduino-cli core install rp2040:rp2040@6.0.0 --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli lib install U8g2@2.36.19
arduino-cli lib install ArduinoJson@7.4.3
arduino-cli compile --fqbn rp2040:rp2040:rpipico2w:flash=4194304_262144 .
```

The explicit flash menu reserves 256 KB for LittleFS; the board default reserves no filesystem and must not be used. `build.ps1` runs the validated command with all compiler warnings enabled. LittleFS, Wi-Fi, DNS, HTTP, TLS, Wire, and UDP support come from the Arduino-Pico core.

## First boot and provisioning

On every boot the firmware shows a Raspberry Pi / Pico 2 W / software-version splash while it mounts LittleFS and checks `/config.json`. Missing, unreadable, malformed, incomplete, or unsupported configuration enters setup mode.

Setup mode starts the open `PostalClock-Setup` access point at `192.168.4.1`, displays those details on the OLED, runs a wildcard DNS responder for common captive-portal probes, and serves a compact local form. If a phone does not open the captive page automatically, browse to `http://192.168.4.1/`.

The form provides asynchronously scanned SSIDs, a hidden-network override, a password field, supported IANA timezone choices, a contact email, and a four-character NWS weather-station ID. The station lookup help link opens in a new browser tab. A successful submission is validated, written to `/config.new`, read back and validated, then promoted while the old config is retained as `/config.bak` until the commit verifies. The clock restarts only after a successful commit. Passwords are never echoed to HTML, OLED, or serial logs.

To deliberately return a configured clock to setup mode without reflashing or erasing its current settings, connect USB serial at 115200 baud and send this line:

```text
provision
```

You can also power-cycle or reset the clock three times within 10 seconds (measured from the first boot to the third boot). On the third boot it starts `PostalClock-Setup` while retaining the current configuration until a replacement is successfully saved. Allow each boot to begin (the splash screen is sufficient) before removing power again. Detection uses the battery-backed DS3231 and two alternating LittleFS records so it works across unplug/plug cycles and retains a recoverable record if one flash write is interrupted. If the RTC is missing, invalid, or reports oscillator-stop, rapid-reset detection is safely skipped.

The existing config remains the rollback copy until a new valid submission commits. BOOTSEL is not used because holding it during reset invokes the Pico ROM bootloader and is not a general-purpose application button gesture.

## Configuration schema

See `config.example.json`. Schema version 5 requires `contact_email`, a four-character U.S. ICAO weather station ID (such as `KMBT`), and a light cutoff. Earlier configurations deliberately return to provisioning so the values can be supplied in the current format. The setup slider defaults to 300 and accepts 0–4000: lower readings use the dark brightness value, while higher readings use the light brightness value. A 20-count hysteresis band avoids flicker at the cutoff. The two brightness sliders retain their 1–255 range and are written directly to the standard SH1106 contrast register. The latest station observation refreshes every five minutes for the displayed current temperature; the `/latest` request explicitly uses `require_qc=false` and locally sanity-checks the numeric value. Forecast data refreshes hourly. The client reads the observation station's coordinates, resolves them through the NWS `/points` endpoint, follows the returned `properties.forecast` URL, and uses forecast period numbers 1 and 2 for the displayed high/low. The current period's `shortForecast` appears at the top center. Supported timezones are UTC and the listed U.S. IANA zones in the setup page. Internally, each is mapped to a POSIX rule so DST conversion works without hard-coded dates. The DS3231 stores and supplies local civil time and remains the authoritative display source. NTP runs at most daily after Wi-Fi connects and writes the DS3231 only if its oscillator-stop flag is set, its value is invalid, or drift exceeds two seconds. A valid DS3231 reading is always preferred; if the RTC is absent or stops responding, the display falls back to the Pico system clock after that clock has been seeded from the RTC or synchronized by NTP. A failed DS3231 write does not discard a successful network-time update.

Weather uses an NWS observation endpoint with `properties.temperature.value` in Celsius. Requests identify the appliance as `(postal-clock.mcdaniel.ws, configured-email)` in the HTTP User-Agent. HTTPS encryption is used without certificate validation, so weather retrieval does not depend on a bundled CA or a correct RTC during the TLS handshake. Weather retrieval runs on the Pico's second core so DNS, TLS, and HTTP waits do not pause OLED or clock servicing on the main core. Core 1 receives a separate 8 KB stack for TLS/JSON work, and a cross-core mutex serializes access to the shared Wi-Fi stack. Core 0 never waits for weather; it renders the last safe weather snapshot. An 8-second hardware watchdog reboots the appliance if the main clock loop itself ever stalls. The last good value stays visible after failures and is marked `*` when older than ten minutes. Network connection attempts time out and use exponential retry backoff; clock, RTC, display, and brightness servicing do not depend on a connection.

While the clock is connected to Wi-Fi, its configuration page is also available at `http://<clock-ip>/`. The current IP address is printed to the serial log after connection. Existing values are prefilled, and leaving the password blank preserves the saved Wi-Fi password. The Scan button beside the network list starts an asynchronous scan and updates the list in place when it completes. Saving validates and atomically replaces the configuration, then restarts the clock.

The forecast low/high appears at the upper right when the next two NWS forecast periods contain one daytime and one nighttime period. Period order is preserved; `isDaytime` decides which value is the high and which is the low.

The lower-right Wi-Fi icon blinks while the station is connecting or waiting for its next retry. Once connected, one, two, or three arcs indicate weak (below -70 dBm), medium (-70 through -56 dBm), or strong (-55 dBm and above) signal. NTP queries `time.nist.gov` with a 48-byte version-3 client request, validates the response, applies its fractional timestamp to the system clock, and rounds to the nearest whole second only when the DS3231 needs correction.

Do not commit a real `/config.json` or credentials. LittleFS data is provisioned on the device, not from this repository.


## Harvested weather detail

Software 1.0.39 retains additional fields from the latest NWS station
observation without making extra requests: relative humidity, wind speed and
direction, barometric pressure, heat-index/wind-chill based "feels like", and
precipitation during the last hour. NWS metric values are converted to familiar
display units (F, mph, inHg, and inches). Pressure is also shown as a signed
difference from standard sea-level pressure (29.92 inHg).

The first short press of the GP8 information button shows a compact weather
snapshot on the OLED. The normal connected configuration page also links to
`/weather`, which renders the latest core-0 weather snapshot. Loading that page
does not trigger a NOAA request, so web viewing cannot stall or increase the
weather polling rate.
