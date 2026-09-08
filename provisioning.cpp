#include "provisioning.h"

#include <ArduinoJson.h>
#include <WiFi.h>

namespace {
constexpr char kAccessPointName[] = "PostalClock-Setup";
constexpr char kDefaultWeatherStationId[] = "KMBT";
constexpr uint32_t kScanIntervalMs = 60000;

bool boundedCopy(const String& source, char* destination, size_t capacity) {
  if (source.length() >= capacity) return false;
  source.toCharArray(destination, capacity);
  return true;
}

bool normalizeStationId(String& stationId) {
  stationId.trim();
  stationId.toUpperCase();
  if (stationId.length() != 4 || stationId[0] != 'K') return false;
  for (size_t i = 0; i < stationId.length(); ++i) {
    const char character = stationId[i];
    if (!((character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9'))) {
      return false;
    }
  }
  return true;
}

bool parseLightCutoff(const String& input, uint16_t& cutoff) {
  if (input.isEmpty() || input.length() > 4) return false;
  uint16_t value = 0;
  for (size_t i = 0; i < input.length(); ++i) {
    const char character = input[i];
    if (character < '0' || character > '9') return false;
    value = value * 10U + static_cast<uint16_t>(character - '0');
  }
  if (value > 4000) return false;
  cutoff = value;
  return true;
}

void appendTimezoneOption(String& page, const char* value, const char* label,
                          const char* selectedValue) {
  page += F("<option value='");
  page += value;
  page += F("'");
  if (strcmp(value, selectedValue) == 0) page += F(" selected");
  page += F(">");
  page += label;
  page += F("</option>");
}

const char* compassDirection(int degrees) {
  if (degrees < 0) return "--";
  static const char* const directions[] = {
      "N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  const uint8_t index = static_cast<uint8_t>((degrees + 22) / 45) & 7U;
  return directions[index];
}
}

bool ProvisioningService::begin(ConfigStore& store) {
  store_ = &store;
  connectedMode_ = false;
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  const IPAddress subnet(255, 255, 255, 0);
  if (!WiFi.softAPConfig(address_, address_, subnet)) {
    Serial.println(F("Provisioning: AP address configuration failed"));
  }
  if (!WiFi.softAP(kAccessPointName)) {
    Serial.println(F("Provisioning: access point failed to start"));
    return false;
  }
  address_ = WiFi.softAPIP();
  dns_.start(53, "*", address_);
  registerRoutes();
  server_.begin();
  startScan(millis());
  Serial.print(F("Provisioning: join PostalClock-Setup and open http://"));
  Serial.println(address_);
  return true;
}

bool ProvisioningService::beginConnected(ConfigStore& store,
                                          const AppConfig& config) {
  store_ = &store;
  existingConfig_ = config;
  connectedMode_ = true;
  connectedAddressLogged_ = false;
  registerRoutes();
  server_.begin();
  Serial.println(F("Configuration: web page will be available on the Wi-Fi address"));
  return true;
}

void ProvisioningService::registerRoutes() {
  if (routesRegistered_) return;
  routesRegistered_ = true;
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/save", HTTP_POST, [this]() { handleSave(); });
  server_.on("/scan", HTTP_POST, [this]() { handleScan(); });
  server_.on("/scan-status", HTTP_GET, [this]() { handleScanStatus(); });
  server_.on("/light-status", HTTP_GET, [this]() { handleLightStatus(); });
  server_.on("/weather", HTTP_GET, [this]() { handleWeather(); });
  server_.on("/debug", HTTP_GET, [this]() { handleDebug(); });
  server_.on("/generate_204", HTTP_ANY, [this]() { handleRoot(); });
  server_.on("/gen_204", HTTP_ANY, [this]() { handleRoot(); });
  server_.on("/hotspot-detect.html", HTTP_ANY, [this]() { handleRoot(); });
  server_.on("/connecttest.txt", HTTP_ANY, [this]() { handleRoot(); });
  server_.on("/ncsi.txt", HTTP_ANY, [this]() { handleRoot(); });
  server_.onNotFound([this]() { handleNotFound(); });
}

void ProvisioningService::service(uint32_t nowMs) {
  if (!connectedMode_) dns_.processNextRequest();
  server_.handleClient();
  if (rebootAtMs_ != 0 && static_cast<int32_t>(nowMs - rebootAtMs_) >= 0) {
    Serial.println(F("Provisioning: restarting with saved configuration"));
    rp2040.reboot();
  }

  if (scanRunning_) {
    const int result = WiFi.scanComplete();
    if (result != -1) {
      scanRunning_ = false;
      scanFailed_ = result < 0;
      nextScanMs_ = nowMs + kScanIntervalMs;
      if (result >= 0) {
        Serial.print(F("Provisioning: Wi-Fi scan found networks: "));
        Serial.println(result);
      } else {
        Serial.println(F("Provisioning: Wi-Fi scan failed"));
      }
    }
  }

  if (connectedMode_) {
    if (!connectedAddressLogged_ && WiFi.status() == WL_CONNECTED) {
      connectedAddressLogged_ = true;
      address_ = WiFi.localIP();
      Serial.print(F("Configuration: open http://"));
      Serial.println(address_);
    }
    return;
  }

  if (!scanRunning_ && static_cast<int32_t>(nowMs - nextScanMs_) >= 0) {
    startScan(nowMs);
  }
}

void ProvisioningService::startScan(uint32_t nowMs) {
  (void)nowMs;
  WiFi.scanDelete();
  const int result = WiFi.scanNetworks(true);
  scanRunning_ = result == -1;
  scanFailed_ = result < -1;
  if (!scanRunning_) nextScanMs_ = millis() + 10000;
}

void ProvisioningService::handleRoot() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/html; charset=utf-8", pageHtml());
}

void ProvisioningService::sendSaveError(int status, const String& message) {
  if (!connectedMode_) {
    server_.send(status, "text/html; charset=utf-8", pageHtml(message));
    return;
  }
  JsonDocument response;
  response["saved"] = false;
  response["message"] = message;
  String body;
  serializeJson(response, body);
  server_.send(status, "application/json", body);
}

void ProvisioningService::handleSave() {
  AppConfig candidate = connectedMode_ ? existingConfig_ : AppConfig{};
  String ssid = server_.arg("manual_ssid");
  ssid.trim();
  if (ssid.isEmpty()) ssid = server_.arg("ssid");
  String password = server_.arg("password");
  String timezone = server_.arg("timezone");
  String contactEmail = server_.arg("contact_email");
  contactEmail.trim();
  String weatherStationId = server_.arg("weather_station_id");
  if (!normalizeStationId(weatherStationId)) {
    sendSaveError(400, "Enter a four-character U.S. ICAO station ID, such as KMBT.");
    return;
  }
  uint16_t lightCutoff = 0;
  if (!parseLightCutoff(server_.arg("light_cutoff"), lightCutoff)) {
    sendSaveError(400, "Light cutoff must be a whole number from 0 to 4000.");
    return;
  }
  uint16_t darkBrightness = 0, lightBrightness = 0;
  if (!parseLightCutoff(server_.arg("dark_brightness"), darkBrightness) ||
      !parseLightCutoff(server_.arg("light_brightness"), lightBrightness) ||
      darkBrightness < 1 || lightBrightness < 1 || darkBrightness > 255 ||
      lightBrightness > 255 || darkBrightness > lightBrightness) {
    sendSaveError(400, "Brightness values must be 1-255, with dark no brighter than light.");
    return;
  }

  if (!boundedCopy(ssid, candidate.wifiSsid, sizeof(candidate.wifiSsid)) ||
      (!(connectedMode_ && password.isEmpty()) &&
       !boundedCopy(password, candidate.wifiPassword,
                    sizeof(candidate.wifiPassword))) ||
      !boundedCopy(timezone, candidate.timezone, sizeof(candidate.timezone)) ||
      !boundedCopy(contactEmail, candidate.contactEmail,
                   sizeof(candidate.contactEmail)) ||
      !boundedCopy(weatherStationId, candidate.weatherStationId,
                   sizeof(candidate.weatherStationId))) {
    sendSaveError(400, "A field is too long.");
    return;
  }
  candidate.weatherRefreshMinutes = connectedMode_
      ? existingConfig_.weatherRefreshMinutes : 60;
  candidate.brightnessMin = static_cast<uint8_t>(darkBrightness);
  candidate.brightnessMax = static_cast<uint8_t>(lightBrightness);
  candidate.brightnessCutoff = lightCutoff;
  candidate.debugLogging = true;

  String reason;
  if (!ConfigStore::validate(candidate, &reason)) {
    sendSaveError(400, reason);
    return;
  }
  if (!store_->save(candidate, &reason)) {
    Serial.print(F("Provisioning: configuration save failed: "));
    Serial.println(reason);
    sendSaveError(500, "Could not save configuration. The previous configuration was retained.");
    return;
  }

  Serial.println(F("Provisioning: configuration saved (password not logged)"));
  if (connectedMode_) {
    // Keep the normal LAN configuration page available.  The persisted
    // settings are intentionally applied at the next restart rather than
    // partially hot-reconfiguring Wi-Fi, time, and weather services.
    existingConfig_ = candidate;
    server_.send(200, "application/json",
                 "{\"saved\":true,\"message\":\"Configuration saved. Clock restarting...\"}");
    rebootAtMs_ = millis() + 1500;
    return;
  }
  server_.send(200, "text/html; charset=utf-8",
               F("<!doctype html><meta name=viewport content='width=device-width'>"
                 "<title>Postal Clock</title><h2>Saved</h2>"
                 "<p>The clock is restarting. You may reconnect your phone to normal Wi-Fi.</p>"));
  rebootAtMs_ = millis() + 1500;
}

void ProvisioningService::handleScan() {
  if (!scanRunning_) startScan(millis());
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(202, "application/json", "{\"running\":true}");
}

void ProvisioningService::handleScanStatus() {
  const int result = WiFi.scanComplete();
  const bool running = scanRunning_ || result == -1;
  JsonDocument document;
  document["running"] = running;
  document["failed"] = !running && (scanFailed_ || result < 0);
  JsonArray networks = document["networks"].to<JsonArray>();
  if (!running && result >= 0) {
    for (int i = 0; i < result; ++i) {
      JsonObject network = networks.add<JsonObject>();
      network["ssid"] = WiFi.SSID(i);
      network["rssi"] = WiFi.RSSI(i);
    }
  }
  String response;
  response.reserve(512);
  serializeJson(document, response);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", response);
}

void ProvisioningService::handleLightStatus() {
  JsonDocument document;
  document["value"] = lightValue_;
  String response;
  serializeJson(document, response);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", response);
}


void ProvisioningService::handleWeather() {
  String page;
  page.reserve(3000);
  page += F("<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Postal Clock Weather</title>"
            "<style>body{font-family:system-ui,sans-serif;max-width:42rem;margin:2rem auto;"
            "padding:0 1rem;background:#f6f4ee;color:#222}table{border-collapse:collapse;"
            "width:100%;background:white}td,th{padding:.55rem .7rem;border-bottom:1px solid #ddd;"
            "text-align:left}th{width:45%}.muted{color:#666}a{color:#174b7a}</style>"
            "</head><body><h2>Postal Clock Weather</h2>");

  if (!connectedMode_) {
    page += F("<p class=muted>Weather data is available after the clock connects to normal Wi-Fi.</p>");
  } else if (!hasWeatherSnapshot_) {
    page += F("<p class=muted>No harvested weather snapshot is available yet.</p>");
  } else {
    const WeatherSnapshot& w = weatherSnapshot_;
    page += F("<p>Last harvested station data for <strong>");
    page += htmlEscape(existingConfig_.weatherStationId);
    page += F("</strong>. This page does not trigger a new NOAA request.</p><table>");

    page += F("<tr><th>Temperature</th><td>");
    if (w.hasTemperature) {
      page += String(w.temperatureF);
      page += F("&deg;F");
      if (w.temperatureStale) page += F(" <span class=muted>(stale)</span>");
    } else page += F("--");
    page += F("</td></tr>");

    page += F("<tr><th>Humidity</th><td>");
    if (w.hasHumidity) {
      page += String(w.humidityPct);
      page += F("%");
    } else page += F("--");
    page += F("</td></tr>");

    page += F("<tr><th>Feels like</th><td>");
    if (w.hasFeelsLike) {
      page += String(w.feelsLikeF);
      page += F("&deg;F");
    } else page += F("--");
    page += F("</td></tr>");

    page += F("<tr><th>Wind</th><td>");
    if (w.hasWind) {
      page += String(w.windMph, 1);
      page += F(" mph ");
      page += compassDirection(w.windDirectionDeg);
      if (w.windDirectionDeg >= 0) {
        page += F(" (");
        page += String(w.windDirectionDeg);
        page += F("&deg;)");
      }
    } else page += F("--");
    page += F("</td></tr>");

    page += F("<tr><th>Pressure</th><td>");
    if (w.hasPressure) {
      page += String(w.pressureInHg, 2);
      page += F(" inHg (");
      if (w.pressureDeltaInHg >= 0.0f) page += F("+");
      page += String(w.pressureDeltaInHg, 2);
      page += F(" vs 29.92 standard)");
    } else page += F("--");
    page += F("</td></tr>");

    page += F("<tr><th>Precipitation, last hour</th><td>");
    if (w.hasPrecipLastHour) {
      page += String(w.precipLastHourIn, 2);
      page += F(" in");
    } else page += F("--");
    page += F("</td></tr>");

    page += F("<tr><th>Current forecast</th><td>");
    page += w.shortForecast[0] ? htmlEscape(w.shortForecast) : String("--");
    if (w.precipitationChance > 0) {
      page += F(" (");
      page += String(w.precipitationChance);
      page += F("% precip)");
    }
    page += F("</td></tr>");

    page += F("<tr><th>Next low / high</th><td>");
    if (w.hasDailyRange) {
      page += String(w.dailyLowF);
      page += F("&deg; / ");
      page += String(w.dailyHighF);
      page += F("&deg;F");
    } else page += F("--");
    page += F("</td></tr>");

    page += F("<tr><th>Observation fetch</th><td>HTTP ");
    page += String(w.lastObservationStatus);
    page += F(" at ");
    char timeLine[16];
    snprintf(timeLine, sizeof(timeLine), "%02u:%02u:%02u",
             w.lastObservationRequest.hour, w.lastObservationRequest.minute,
             w.lastObservationRequest.second);
    page += timeLine;
    page += F("</td></tr></table>");
  }

  page += F("<p><a href='/'>Back to clock configuration</a></p></body></html>");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/html; charset=utf-8", page);
}

void ProvisioningService::handleDebug() {
  String page;
  page.reserve(1800);
  page += F("<!doctype html><meta name=viewport content='width=device-width'>"
            "<title>Postal Clock Debug</title><style>body{font-family:system-ui,"
            "sans-serif;max-width:42rem;margin:2rem auto;padding:0 1rem}table{border-"
            "collapse:collapse;width:100%}th,td{padding:.5rem;border-bottom:1px solid "
            "#ddd;text-align:left}th{width:40%}code{word-break:break-word}</style>"
            "<h2>Weather transport debug</h2><p>This page is read-only and does not "
            "start a weather request.</p><table>");
  if (!hasWeatherSnapshot_) {
    page += F("<tr><td colspan=2>No weather snapshot has been published yet.</td></tr>");
  } else {
    const WeatherSnapshot& w = weatherSnapshot_;
    page += F("<tr><th>Observation status</th><td>");
    page += String(w.lastObservationStatus);
    page += F("</td></tr><tr><th>Forecast status</th><td>");
    page += String(w.lastForecastStatus);
    page += F("</td></tr><tr><th>Last failed request</th><td><code>");
    page += htmlEscape(w.lastFailedRequest[0] ? w.lastFailedRequest : "--");
    page += F("</code></td></tr><tr><th>Transport detail</th><td><code>");
    page += htmlEscape(w.lastTransportDetail[0] ? w.lastTransportDetail : "--");
    page += F("</code></td></tr><tr><th>BearSSL error</th><td>");
    page += String(w.lastTlsError);
    page += F("</td></tr><tr><th>BearSSL detail</th><td><code>");
    page += htmlEscape(w.lastTlsErrorText[0] ? w.lastTlsErrorText : "--");
    page += F("</code></td></tr>");
  }
  page += F("</table><p><a href='/weather'>Weather data</a> · <a href='/'>Configuration</a></p>");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/html; charset=utf-8", page);
}

void ProvisioningService::handleNotFound() {
  if (connectedMode_) {
    server_.send(404, "text/plain", "Not found");
  } else {
    server_.sendHeader("Location", String("http://") + address_.toString() + "/", true);
    server_.send(302, "text/plain", "");
  }
}

String ProvisioningService::pageHtml(const String& message) const {
  String page;
  page.reserve(7000);
  page += F("<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Postal Clock Setup</title><style>"
            "body{font:16px system-ui;margin:0;background:#f3f0e8;color:#241b13}"
            "main{max-width:32rem;margin:auto;padding:1rem}fieldset{border:1px solid #9b8977;border-radius:.5rem;background:#fff}"
            "label{display:block;margin:.8rem 0 .25rem}input,select,button{box-sizing:border-box;width:100%;font:inherit;padding:.7rem;border:1px solid #877667;border-radius:.35rem}"
            "button{margin-top:1rem;background:#493421;color:#fff;font-weight:700}.msg{padding:.7rem;background:#ffe2dc;border-radius:.35rem}"
            ".wifi-row{display:grid;grid-template-columns:1fr auto;gap:.5rem}.wifi-row button{width:auto;margin:0}"
            "small{display:block;color:#66584d;margin-top:.3rem}</style></head><body><main>"
            "<h2>Postal Clock Setup</h2><p>Choose the network and local settings for your clock.</p>");
  if (!message.isEmpty()) {
    page += F("<p class=msg>");
    page += htmlEscape(message);
    page += F("</p>");
  }
  page += F("<form method=post action=/save");
  if (connectedMode_) page += F(" id=config_form");
  page += F("><fieldset><legend>Clock settings</legend>"
            "<label for=ssid>Nearby Wi-Fi</label><div class=wifi-row>"
            "<select id=ssid name=ssid><option value=''>Select a network</option>");
  if (connectedMode_) {
    const String escaped = htmlEscape(existingConfig_.wifiSsid);
    page += F("<option selected value=\""); page += escaped; page += F("\">");
    page += escaped; page += F(" (current)</option>");
  }
  const int count = WiFi.scanComplete();
  if (count > 0) {
    for (int i = 0; i < count; ++i) {
      const String escaped = htmlEscape(WiFi.SSID(i));
      page += F("<option value=\""); page += escaped; page += F("\">");
      page += escaped; page += F(" ("); page += WiFi.RSSI(i); page += F(" dBm)</option>");
    }
  }
  page += F("</select><button id=scan_button type=button onclick='scanNetworks()'>Scan</button></div>"
            "<small id=scan_status>Press Scan to refresh nearby networks.</small>"
            "<label for=manual_ssid>Hidden network (optional)</label>"
            "<input id=manual_ssid name=manual_ssid maxlength=32 autocomplete=off>"
            "<small>If entered, this overrides the selected network.</small>"
            "<label for=password>Wi-Fi password</label>"
            "<input id=password name=password type=password maxlength=64 autocomplete=new-password>");
  if (connectedMode_) {
    page += F("<small>Leave blank to keep the current password.</small>");
  }
  page += F("<label for=timezone>Timezone</label>"
            "<select id=timezone name=timezone required>");
  const char* selectedTimezone = connectedMode_
      ? existingConfig_.timezone : "America/Chicago";
  appendTimezoneOption(page, "America/Chicago", "Central - America/Chicago",
                       selectedTimezone);
  appendTimezoneOption(page, "America/New_York", "Eastern - America/New_York",
                       selectedTimezone);
  appendTimezoneOption(page, "America/Denver", "Mountain - America/Denver",
                       selectedTimezone);
  appendTimezoneOption(page, "America/Phoenix", "Arizona - America/Phoenix",
                       selectedTimezone);
  appendTimezoneOption(page, "America/Los_Angeles", "Pacific - America/Los_Angeles",
                       selectedTimezone);
  appendTimezoneOption(page, "America/Anchorage", "Alaska - America/Anchorage",
                       selectedTimezone);
  appendTimezoneOption(page, "Pacific/Honolulu", "Hawaii - Pacific/Honolulu",
                       selectedTimezone);
  appendTimezoneOption(page, "UTC", "UTC", selectedTimezone);
  page += F("</select>"
            "<label for=contact_email>Contact email</label>"
            "<input id=contact_email name=contact_email type=email required maxlength=128 "
            "placeholder='you@example.com' autocomplete=email value='");
  if (connectedMode_) page += htmlEscape(existingConfig_.contactEmail);
  page += F("'>"
            "<small>Sent to NOAA/NWS only as part of the weather request User-Agent.</small>"
            "<label for=weather_station_id>NWS weather station ID</label>"
            "<input id=weather_station_id name=weather_station_id required maxlength=4 "
            "pattern='K[A-Za-z0-9]{3}' autocapitalize=characters "
            "value='");
  page += connectedMode_ ? existingConfig_.weatherStationId
                         : kDefaultWeatherStationId;
  page += F("'>"
            "<small>Use a four-character ICAO station ID (for example, KMBT). "
            "<a href='https://www.weather.gov/tg/siteloc' target='_blank' rel='noopener'>Help finding a station ID</a></small>"
            "<label for=light_cutoff>Light/dark cutoff (current: <output id=photocell_value>");
  page += String(lightValue_);
  page += F("</output>): <output id=cutoff_value>");
  page += connectedMode_ ? String(existingConfig_.brightnessCutoff) : String(300);
  page += F("</output></label>"
            "<input id=light_cutoff name=light_cutoff type=range min=0 max=4000 value=");
  page += connectedMode_ ? String(existingConfig_.brightnessCutoff) : String(300);
  page += F(" "
            "oninput=\"document.getElementById('cutoff_value').value=this.value\">"
            "<small>Readings below this value use dark, low-contrast mode; higher readings use light mode.</small>"
            "<label for=dark_brightness>Dark mode brightness: <output id=dark_value>");
  page += connectedMode_ ? String(existingConfig_.brightnessMin) : String(1);
  page += F("</output></label><input id=dark_brightness name=dark_brightness type=range min=1 max=255 value=");
  page += connectedMode_ ? String(existingConfig_.brightnessMin) : String(1);
  page += F(" oninput=\"dark_value.value=this.value\"><label for=light_brightness>Light mode brightness: <output id=light_value>");
  page += connectedMode_ ? String(existingConfig_.brightnessMax) : String(255);
  page += F("</output></label><input id=light_brightness name=light_brightness type=range min=1 max=255 value=");
  page += connectedMode_ ? String(existingConfig_.brightnessMax) : String(255);
  page += F(" oninput=\"light_value.value=this.value\"><small>Brightness is the OLED contrast value from 1 to 255; 0 is not allowed.</small>");
  if (connectedMode_) {
    page += F("<button type=submit>Save configuration</button><p id=save_status class=msg hidden></p>");
  } else {
    page += F("<button type=submit>Save and restart clock</button>");
  }
  page += F("</fieldset></form>"
            "<p><small>The password is written only to the clock and is never shown again.</small></p>");
  if (connectedMode_) {
    page += F("<p><a href='/weather'>View harvested weather data</a></p>");
  }
  page += F("<script>async function scanNetworks(){const b=document.getElementById('scan_button'),s=document.getElementById('scan_status'),l=document.getElementById('ssid');b.disabled=true;s.textContent='Scanning...';try{const q=await fetch('/scan',{method:'POST'});if(!q.ok)throw 0;for(let i=0;i<30;i++){await new Promise(r=>setTimeout(r,500));const d=await(await fetch('/scan-status',{cache:'no-store'})).json();if(!d.running){if(d.failed)throw 0;const current=l.value;l.replaceChildren(new Option('Select a network',''));for(const n of d.networks)l.add(new Option(n.ssid+' ('+n.rssi+' dBm)',n.ssid));if(current&&!Array.from(l.options).some(o=>o.value===current))l.add(new Option(current+' (current)',current));l.value=current;s.textContent='Found '+d.networks.length+' network'+(d.networks.length===1?'':'s')+'.';return}}s.textContent='Scan timed out.'}catch(e){s.textContent='Scan failed. Try again.'}finally{b.disabled=false}}"
            "const photocell=document.getElementById('photocell_value');if(photocell)setInterval(async()=>{try{const r=await fetch('/light-status',{cache:'no-store'});if(r.ok)photocell.value=(await r.json()).value}catch(e){}},1000);const configForm=document.getElementById('config_form');if(configForm)configForm.addEventListener('submit',async e=>{e.preventDefault();const b=configForm.querySelector('button[type=submit]'),s=document.getElementById('save_status');b.disabled=true;s.hidden=false;s.textContent='Saving...';try{const r=await fetch('/save',{method:'POST',body:new FormData(configForm)}),d=await r.json();if(!r.ok||!d.saved)throw Error(d.message||'Could not save configuration.');s.textContent=d.message;setTimeout(async function online(){try{if((await fetch('/',{cache:'no-store'})).ok){s.hidden=true;return}}catch(e){}setTimeout(online,1000)},2000)}catch(e){s.textContent=e.message||'Could not save configuration. Check the fields and try again.'}finally{b.disabled=false}})</script>"
            "</main></body></html>");
  return page;
}

String ProvisioningService::htmlEscape(const String& input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    switch (input[i]) {
      case '&': output += F("&amp;"); break;
      case '<': output += F("&lt;"); break;
      case '>': output += F("&gt;"); break;
      case '\"': output += F("&quot;"); break;
      case '\'': output += F("&#39;"); break;
      default: output += input[i]; break;
    }
  }
  return output;
}
