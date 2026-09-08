# AGENTS.md

## Project Overview

This repository contains firmware for a custom nightstand clock built into an antique wooden coin bank with an original brass U.S. Post Office box door.

The goal is to hide modern electronics behind the antique postal hardware while preserving the original appearance, working combination lock, coin slot, and usable coin-storage space.

The device should behave like a reliable appliance rather than an experimental development board.

## Hardware

Primary controller:

- Raspberry Pi Pico 2 W

Connected hardware:

- 1.3-inch 128x64 monochrome OLED
- DS3231 RTC
- Photocell / LDR for automatic OLED brightness
- Wi-Fi via Pico 2 W
- 5V Micro-USB power input

Board GPIO wiring is fixed and must be preserved: the OLED uses SDA GP0 / SCL GP1; the DS3231 RTC uses SDA GP6 / SCL GP7; the information/reset button uses GP8 to ground with an active-low internal pull-up; and the photocell uses ADC GP26. GP3 and GP4 are unusable on this board and must not be assigned to peripherals, buttons, or other functions.

Mechanical design:

- OLED is mounted directly behind the original postal-box glass window.
- Original combination lock mechanism remains functional.
- The photocell sits near the top-right edge of the glass.
- A removable 1887 Indian Head cent hides access to the Pico BOOTSEL button.
- Pico is mounted behind the display assembly.
- A removable rear electronics cap carries the DS3231.
- RTC wiring connects from the removable cap to the Pico so the RTC remains serviceable.
- The rear cap also carries or surrounds the Micro-USB power entry.

Do not make assumptions about pin assignments unless they are documented in the repository or existing code.

## Core Design Philosophy

This is a clock first.

The following priority order should guide firmware design:

1. Accurate local timekeeping
2. Reliable display operation
3. Automatic brightness control
4. Graceful recovery from failures
5. Configuration/provisioning
6. Weather retrieval
7. Network time correction
8. Development/debug convenience

Wi-Fi must never be required for basic clock operation after initial configuration.

Network failures must not freeze, reboot-loop, or significantly delay the clock display.

## First-Boot / Provisioning Mode

If no valid configuration file exists at startup, the firmware must enter provisioning mode instead of normal clock operation.

Provisioning mode should:

- Show a clear startup/setup screen on the OLED.
- Start a Wi-Fi access point on the Pico 2 W.
- Host a small local configuration webpage.
- Preferably provide captive-portal behavior using DNS redirection if practical.
- At minimum, display the AP SSID and local setup URL/IP address on the OLED.
- Scan for nearby Wi-Fi networks and present the discovered SSIDs in the webpage.
- Allow manual SSID entry for hidden networks.
- Allow entry of the Wi-Fi password.
- Allow configuration of timezone.
- Allow configuration of the NOAA/NWS weather endpoint or equivalent weather location settings.
- Validate required fields before saving.
- Save configuration to persistent local storage.
- Reboot or transition cleanly into normal clock mode after successful configuration.

The setup UI should be simple, compact, and usable from a phone.

Provisioning must not require a separate app.

Suggested default AP behavior:

- SSID: PostalClock-Setup
- Use an implementation-appropriate local address such as 192.168.4.1.
- If captive portal support is implemented, redirect DNS requests to the setup page.
- If captive portal support is not reliable on the selected framework, fall back gracefully to displaying the setup IP on the OLED.

Do not hard-code a permanent setup password unless there is a clear reason. If a setup password is introduced, document it and make it configurable.

## Configuration Web UI

The configuration page is available both from the first-boot `PostalClock-Setup` access point and from the clock's normal LAN address while it is connected to Wi-Fi.

These are two entry modes to the same UI, not separate pages. They must use the same route handlers, HTML renderer, field definitions, validation, help links, and save behavior. Whenever either configuration experience is changed, verify that the change appears and works in both AP provisioning mode and connected-Wi-Fi mode. Mode-specific behavior, such as captive DNS, saved-value prefilling, or retaining a blank existing password, should be explicit and kept to the smallest practical conditional branches.

The network list must include a user-triggered scan control. Scanning must be asynchronous, must report progress or failure, and must not block clock display/timekeeping service while nearby networks are discovered.

## Configuration Storage

Use a small persistent configuration file in local flash storage.

Preferred approach for Arduino-Pico:

- LittleFS
- JSON or another simple human-readable format

Suggested configuration values:

- Wi-Fi SSID
- Wi-Fi password
- timezone
- NOAA/NWS endpoint or location parameters
- optional weather refresh interval
- optional display brightness tuning values
- optional debug/logging flag

Requirements:

- Treat missing, unreadable, malformed, or incomplete config as invalid.
- Never partially overwrite a valid config if a save operation fails.
- Prefer write-to-temp + rename/replace semantics when practical.
- Do not log Wi-Fi passwords.
- Avoid exposing secrets on the OLED.
- Keep secrets out of source control.
- Provide a sample/default config schema in documentation.

Normal boot behavior:

1. Mount config storage.
2. Load and validate config.
3. If config is valid, continue normal startup.
4. If config is absent or invalid, enter provisioning mode.

## Configuration Reset / Re-entry

Provide a practical way to return to provisioning mode later.

Preferred options, in order:

1. A deliberate software-triggered reset mode.
2. A documented BOOTSEL-assisted or startup-button gesture if practical and safe.
3. A temporary configuration-reset build option during development.

Do not erase configuration accidentally on ordinary reboot.

If the BOOTSEL button is used as part of a setup-reset gesture, ensure the design does not interfere with the Pico boot ROM behavior and document the exact sequence.

## Timekeeping Architecture

The DS3231 is the authoritative local clock.

Expected behavior:

- Read the DS3231 during startup.
- Display usable local time immediately when a valid configuration exists.
- Normal time display must continue without Wi-Fi or Internet access.
- NTP may be used periodically to correct RTC drift.
- NTP is not the primary runtime clock source.
- Do not write the DS3231 continuously.
- Only update the RTC when there is a meaningful reason to do so.
- Handle invalid RTC data explicitly.
- Preserve sensible behavior if the RTC battery is missing or depleted.

If timezone or DST handling is implemented, keep it separate from raw RTC access and document the chosen approach.

Timezone configuration should preferably use an IANA timezone name or another format that can correctly handle daylight-saving rules without hard-coded seasonal dates.

## Display

OLED resolution:

- 128 x 64

Normal screen should remain intentionally uncluttered.

Desired content:

- Large segmented-style time
- Outdoor temperature
- Day of week
- Date

Do not turn the display into a dense dashboard.

The clock should remain highly readable from normal nightstand distance.

Provisioning/setup screens are allowed to use a different layout optimized for instructions.

Avoid unnecessary full-screen redraws if the chosen display library permits efficient updates.

## Photocell / Brightness

The photocell controls OLED brightness automatically.

Important behavior:

- Smooth ADC readings.
- Avoid visible brightness flicker.
- Avoid rapid oscillation near threshold values.
- Use filtering, hysteresis, averaging, or another simple stable method.
- Very low brightness must be possible in a dark bedroom.
- Brightness mapping should be easy to tune.

Do not assume a linear perceived-brightness response.

Prefer configurable minimum and maximum brightness values.

## Wi-Fi

Wi-Fi is an enhancement during normal operation and is required only for provisioning and online services.

Requirements:

- Clock must continue running when Wi-Fi is unavailable after configuration.
- Avoid indefinite blocking calls.
- Use explicit timeouts.
- Retry intelligently.
- Prefer increasing retry delays/backoff over constant aggressive reconnect attempts.
- Do not spam connection attempts continuously.
- Avoid resetting the whole device merely because Wi-Fi failed.
- Keep network work isolated from critical display/timekeeping behavior.

During provisioning, AP mode and configuration HTTP/DNS services may run continuously until configuration is saved.

## Weather

Weather will be obtained over Wi-Fi.

Preferred source:

- NOAA / National Weather Service

Only modest weather information is required for the display.

Primary displayed weather value:

- Outdoor temperature

Requirements:

- Cache the last valid weather reading.
- Continue displaying cached data when temporarily offline.
- Track whether cached data is stale.
- Do not clear a valid displayed temperature merely because one update failed.
- Validate received data.
- Handle malformed JSON, HTTP errors, DNS failures, TLS failures, and timeouts gracefully.
- Avoid excessive API polling.
- Make the endpoint/location configurable through the provisioning webpage.

A weather refresh interval on the order of minutes is appropriate, not seconds.

## Networking and Scheduling

Avoid a design where a failed HTTP request stalls the clock.

On this dual-core Pico 2 W, weather DNS/TLS/HTTP retrieval must remain on core 1. Core 0 owns the OLED, RTC, brightness, and responsive UI; cross-core weather state must be exchanged through bounded snapshots or otherwise safe synchronization.

Reliability implementation notes for this repository:
- Keep `core1_separate_stack = true`; TLS/ArduinoJson work must not run on the default split 4 KB core-1 stack.
- Serialize all Wi-Fi stack access across cores with the shared network mutex. Core 0 must use a non-blocking try-lock so weather can never stall the clock display.
- Core 0 renders only a copied `WeatherSnapshot`; do not read mutable `WeatherService` state while core 1 may be updating it.
- Keep the main-core hardware watchdog enabled so an unexpected main-loop stall automatically returns to DS3231-backed clock operation.

Prefer a cooperative task/state-machine architecture using elapsed-time checks rather than long delays.

Conceptually:

main loop:
- service display
- service timekeeping
- sample light sensor
- adjust brightness
- service network state
- fetch weather when due
- perform NTP correction when due
- feed watchdog if used

Avoid:

- long delay()
- indefinite while loops waiting for Wi-Fi
- indefinite socket waits
- large synchronous operations inside the display path

Provisioning mode may use a separate application state, but it should still avoid indefinite blocking operations.

## Reliability

This device may run continuously for months.

Design accordingly.

Pay attention to:

- memory leaks
- runaway String allocation
- repeated heap fragmentation
- stuck network states
- failed reconnect logic
- millis() rollover
- malformed server responses
- RTC read failures
- I2C failures
- stale weather values
- watchdog interaction
- accidental reboot loops
- corrupt or partially written configuration files
- failed flash filesystem mounts
- captive portal edge cases

Prefer fixed-size buffers or bounded allocations where practical.

Do not over-engineer the code, but prioritize predictable behavior.

## Watchdog

A watchdog may be used if supported cleanly by the chosen framework.

If used:

- It should protect against genuine lockups.
- It must not mask poor blocking network design.
- Network failures alone should not routinely trigger watchdog resets.
- Provisioning mode must feed the watchdog correctly if enabled.
- Log or preserve enough information to diagnose repeated resets where practical.

## Logging

During development, useful serial diagnostics are encouraged.

Log events such as:

- boot
- config load success/failure
- provisioning mode entry
- Wi-Fi scan results count
- configuration save success/failure
- RTC initialization
- RTC validity problems
- Wi-Fi connect/disconnect
- weather update success/failure
- NTP synchronization
- major I2C failures
- watchdog-related events

Never log Wi-Fi passwords.

Avoid high-rate logging from the normal display loop.

Logging should be easy to reduce or disable for final deployment.

## Code Structure

Favor modular files/classes.

Suggested responsibilities:

- rtc.*
- display.*
- light_sensor.*
- wifi_manager.*
- weather.*
- ntp.*
- config.*
- provisioning.*
- main.*

Do not create abstractions merely for abstraction's sake.

Keep hardware drivers separate from application policy where practical.

Examples:

- RTC driver reads/writes RTC.
- Time service decides when RTC should be corrected.
- Weather client obtains/parses weather.
- Provisioning service owns AP/DNS/HTTP setup behavior.
- Config service validates and persists settings.
- Application decides which mode is active.

## Build System

Inspect the repository before selecting or modifying the build system.

Preferred choices:

1. Existing working build system
2. Arduino-Pico C++ if starting fresh
3. Raspberry Pi Pico SDK if the repository already uses it or there is a strong technical reason

Do not migrate frameworks casually once the project is established.

## Libraries

Prefer mature, maintained libraries with modest dependency weight.

Before adding a library:

- Check whether equivalent functionality already exists in the repo.
- Avoid duplicate display/RTC/network libraries.
- Avoid large dependencies for trivial functions.
- Prefer libraries compatible with the selected Pico 2 W networking stack.
- For configuration portals, avoid heavyweight frameworks unless they materially simplify the implementation.

Pin versions where practical if dependency changes could break the build.

## Hardware Safety

This project uses low-voltage electronics, but hardware assumptions still matter.

Do not change GPIO voltage assumptions or wiring expectations silently.

Before recommending a wiring change, identify:

- expected voltage
- GPIO pin
- bus type
- whether pull-ups are required
- whether the module is 3.3V-safe

The DS3231 module may contain onboard pull-ups tied to its supply voltage. Do not assume every DS3231 breakout is electrically identical.

The actual installed module should determine whether it is powered from 3.3V or 5V.

## Working Method

Every firmware or user-visible project change must increment `kSoftwareVersion` in `Clock2.ino`. Do not reuse a version number after changing code, configuration behavior, display output, or web UI. Documentation-only changes that alter project requirements should also bump the version unless the user explicitly says otherwise. Compile the incremented version so the generated UF2 and splash-screen version always identify the exact current build.

Before substantial code changes:

1. Inspect the repo.
2. Read existing code.
3. Identify build system.
4. Identify pin definitions.
5. Identify current dependencies.
6. Identify what already works.
7. Identify how persistent storage is currently handled, if at all.
8. Give a concise plan.

Preserve working functionality whenever possible.

Make changes in small, testable steps.

Do not rewrite working modules merely for stylistic preference.

## Validation

After code changes:

- Compile/build the firmware.
- Address compiler errors.
- Review meaningful warnings.
- Run available tests.
- Do not claim hardware validation unless actual hardware was tested.
- Clearly distinguish:
  - compiled successfully
  - logically reviewed
  - hardware verified

For provisioning changes, verify logically and/or with available tests:

- no config -> setup mode
- malformed config -> setup mode
- valid config -> normal mode
- Wi-Fi scan populates network list
- hidden SSID can be entered manually
- password field is not echoed/logged
- timezone is persisted
- NOAA/NWS setting is persisted
- successful save survives reboot
- failed save does not destroy the previous valid config

When hardware testing is required, provide a concise checklist for the user.

## Review Expectations

When asked to review existing code, look specifically for:

- blocking calls
- bad retry loops
- incorrect RTC authority
- unnecessary RTC writes
- Wi-Fi coupling to display operation
- unsafe I2C assumptions
- ADC instability
- OLED flicker
- memory fragmentation
- stale-data handling
- HTTP/TLS timeout issues
- malformed JSON handling
- millis() rollover bugs
- watchdog misuse
- error paths that freeze normal operation
- hidden pin or voltage assumptions
- insecure handling of Wi-Fi credentials
- brittle captive portal behavior
- config corruption or unsafe writes
- setup mode that cannot be re-entered later

Do not merely comment on formatting.

Prioritize bugs that could affect long-term unattended operation.

## User Experience Goal

From the outside, this should still look like an antique locked postal-box coin bank.

The electronics should largely disappear.

Normal operation should feel simple:

- plug it in
- clock appears
- correct time persists offline
- display dims automatically
- weather appears when available
- Internet trouble is mostly invisible

First-time setup should feel equally simple:

- power on with no config
- OLED says setup is required
- connect phone/laptop to PostalClock-Setup
- choose Wi-Fi network
- enter Wi-Fi password
- choose timezone
- enter NOAA/NWS endpoint/location
- save
- clock enters normal operation

Reliability beats feature count.
