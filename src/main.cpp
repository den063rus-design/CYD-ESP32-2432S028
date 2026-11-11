#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <time.h>
#include <esp_netif.h>
#include <esp_wifi.h>

// Target: ESP32-2432S028 CYD, matching the working NerdMiner ILI9341_2 setup.
// The display is driven by TFT_eSPI; touch is not needed for this dashboard.
static constexpr uint8_t BOOT_BUTTON = 0;
static constexpr uint32_t WEATHER_INTERVAL_MS = 15UL * 60UL * 1000UL;
static constexpr uint32_t WEATHER_RETRY_MS = 60UL * 1000UL;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

TFT_eSPI tft;
U8g2_for_TFT_eSPI u8g2;
WebServer server(80);
Preferences prefs;

String savedSsid;
String savedPassword;
String cityName;
String displayCity;
String timezoneName = "UTC+7";
String resolvedTimezone;
String apName;
static constexpr char SETTINGS_AP_PASSWORD[] = "cydweather";
String apPassword = SETTINGS_AP_PASSWORD;
bool naptEnabled = false;
String condition = "";
String weatherDescription = "";
String lastError;
float temperatureC = NAN;
float feelsLikeC = NAN;
int humidity = -1;
float windKmh = NAN;
int weatherCode = -1;
int timezoneOffset = 0;
bool weatherReady = false;
bool portalMode = false;
bool timeReady = false;
uint32_t lastWeatherMs = 0;
uint32_t lastDrawMs = 0;
uint32_t bootPressMs = 0;
int lastMinuteDrawn = -1;

int timezoneOffsetHours(const String &zone) {
  if (zone.startsWith("UTC")) return zone.substring(3).toInt();
  if (zone == "Asia/Krasnoyarsk" || zone == "Asia/Novosibirsk") return 7;
  if (zone == "Asia/Omsk") return 6;
  if (zone == "Asia/Yekaterinburg") return 5;
  if (zone == "Asia/Irkutsk") return 8;
  if (zone == "Asia/Yakutsk") return 9;
  if (zone == "Asia/Vladivostok") return 10;
  if (zone == "Asia/Magadan") return 11;
  if (zone == "Asia/Kamchatka") return 12;
  if (zone == "Europe/Moscow") return 3;
  if (zone == "Europe/Samara") return 4;
  // auto and unknown IANA zones fall back to the board's default region.
  return 7;
}

String normalizedTimezoneInput(const String &zone) {
  return String("UTC+") + timezoneOffsetHours(zone);
}

void configureTimezone(const String &zone) {
  const int offset = timezoneOffsetHours(zone);
  char posixZone[16];
  // POSIX reverses the sign: UTC-7 means local time is UTC+7.
  snprintf(posixZone, sizeof(posixZone), "UTC%c%d", offset >= 0 ? '-' : '+', abs(offset));
  configTzTime(posixZone, "pool.ntp.org", "time.nist.gov");
  Serial.printf("Timezone: %s -> %s (UTC%+d)\n", zone.c_str(), posixZone, offset);
}

void drawUtf8(const String &text, int16_t x, int16_t baseline, uint16_t color) {
  u8g2.setFont(u8g2_font_6x13_t_cyrillic);
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);
  u8g2.setForegroundColor(color);
  u8g2.drawUTF8(x, baseline, text.c_str());
}

void drawUtf8Centered(const String &text, int16_t centerX, int16_t baseline, uint16_t color) {
  u8g2.setFont(u8g2_font_6x13_t_cyrillic);
  const int16_t width = u8g2.getUTF8Width(text.c_str());
  drawUtf8(text, centerX - width / 2, baseline, color);
}

static const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta charset="utf-8"><title>Настройка CYD Weather</title>
<style>body{font:16px system-ui;background:#101827;color:#edf3ff;max-width:520px;margin:24px auto;padding:0 16px}main{background:#1b2940;padding:22px;border-radius:16px}h1{font-size:24px;margin:0 0 8px}p{color:#bac9df}label{display:block;margin:18px 0 6px}input{box-sizing:border-box;width:100%;padding:12px;border-radius:9px;border:1px solid #53647e;background:#0f1929;color:white;font-size:16px}button{margin-top:22px;width:100%;padding:13px;border:0;border-radius:9px;background:#42d6b0;color:#08231e;font-size:17px;font-weight:700}</style>
<main><h1>CYD Weather</h1><p>Введите Wi-Fi, город и часовой пояс. Дата и время синхронизируются автоматически через интернет.</p>
<form method="post" action="/save"><label>Название Wi-Fi сети (SSID)</label><input name="ssid" required maxlength="32" autocomplete="username">
<label>Пароль Wi-Fi</label><input name="password" type="password" maxlength="63" autocomplete="new-password">
<label>Город (например, Krasnoyarsk или Новосибирск)</label><input name="city" required maxlength="64" value="Krasnoyarsk">
<label>Часовой пояс (UTC±N; Красноярск — UTC+7)</label><input name="timezone" maxlength="16" value="UTC+7">
<button type="submit">Сохранить и перезапустить</button></form></main></html>
)HTML";

String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (char c : s) {
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else out += c;
  }
  return out;
}

String urlEncode(const String &s) {
  String out;
  char hex[4];
  for (size_t i = 0; i < s.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else if (c == ' ') {
      out += "%20";
    } else {
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

void showMessage(const String &title, const String &line1, const String &line2 = "") {
  tft.fillScreen(TFT_BLACK);
  drawUtf8Centered(title, 160, 65, TFT_CYAN);
  drawUtf8Centered(line1, 160, 112, TFT_WHITE);
  if (line2.length()) drawUtf8Centered(line2, 160, 142, TFT_WHITE);
}

void configureFastWifi() {
  // Prefer HT40 on both interfaces. Keep the default B/G/N protocol set so
  // legacy clients and upstream routers remain compatible.
  const esp_err_t staBandwidth = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
  const esp_err_t apBandwidth = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);
  Serial.printf("Wi-Fi performance: STA HT40=%s AP HT40=%s\n",
    esp_err_to_name(staBandwidth),
    esp_err_to_name(apBandwidth));
}

void startPortal() {
  portalMode = true;
  WiFi.mode(WIFI_AP_STA);
  uint32_t id = static_cast<uint32_t>(ESP.getEfuseMac());
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<unsigned>(id & 0xFFFF));
  if (apName.isEmpty()) apName = String("CYD-Weather-") + suffix;
  WiFi.softAP(apName.c_str(), apPassword.c_str());
  configureFastWifi();
  const IPAddress ip = WiFi.softAPIP();
  showMessage("Настройка Wi-Fi", apName, String("Пароль: ") + apPassword);
  Serial.printf("Setup AP: %s  password: %s  open http://%s\n", apName.c_str(), apPassword.c_str(), ip.toString().c_str());
}

void startSettingsAP() {
  uint32_t id = static_cast<uint32_t>(ESP.getEfuseMac());
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<unsigned>(id & 0xFFFF));
  if (apName.isEmpty()) apName = String("CYD-Weather-") + suffix;
  WiFi.mode(WIFI_AP_STA);
  const bool ok = WiFi.softAP(apName.c_str(), apPassword.c_str());
  if (ok) configureFastWifi();
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ok && ap) {
    // Stop DHCP while changing its DNS offer so clients receive a public DNS
    // server and their DNS requests can be routed through NAPT.
    const esp_err_t dhcpStop = esp_netif_dhcps_stop(ap);
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    IP4_ADDR(&dns.ip.u_addr.ip4, 8, 8, 8, 8);
    const uint8_t offerDns = 1;
    const esp_err_t dnsSet = esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    const esp_err_t optionSet = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET,
      ESP_NETIF_DOMAIN_NAME_SERVER, (void *)&offerDns, sizeof(offerDns));
    const esp_err_t dhcpStart = esp_netif_dhcps_start(ap);
    Serial.printf("AP DHCP/DNS: stop=%s dns=%s option=%s start=%s\n",
      esp_err_to_name(dhcpStop), esp_err_to_name(dnsSet),
      esp_err_to_name(optionSet), esp_err_to_name(dhcpStart));
  }
  Serial.printf("Repeater AP %s: %s  password: %s  open http://%s\n",
    ok ? "started" : "failed", apName.c_str(), apPassword.c_str(), WiFi.softAPIP().toString().c_str());
}

void enableNapt() {
  if (naptEnabled || WiFi.status() != WL_CONNECTED) return;
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap && esp_netif_napt_enable(ap) == ESP_OK) {
    naptEnabled = true;
    Serial.println("NAPT enabled: repeater clients routed to upstream Wi-Fi");
  } else {
    Serial.println("NAPT enable failed; upstream is connected but routing is unavailable");
  }
}

bool connectWifi() {
  if (savedSsid.isEmpty()) return false;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  startSettingsAP();
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  showMessage("Подключение к Wi-Fi", savedSsid, "Подождите...");
  Serial.printf("Connecting to Wi-Fi: %s\n", savedSsid.c_str());
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    lastError = "Wi-Fi connection failed";
    Serial.println(lastError);
    return false;
  }
  Serial.printf("Wi-Fi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  enableNapt();
  // Start with the configured zone even if the weather/geocoding service is
  // unreachable. Krasnoyarsk is the default; an explicit IANA zone can be set
  // from the local settings page.
  configureTimezone(timezoneName);
  return true;
}

bool httpGetJson(const String &url, JsonDocument &doc) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    lastError = "Could not start HTTP request";
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    lastError = String("Weather service HTTP ") + code;
    http.end();
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    lastError = String("Bad weather response: ") + err.c_str();
    return false;
  }
  return true;
}

bool httpGetText(const String &url, String &body) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    lastError = "Could not start weather request";
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    lastError = String("Weather service HTTP ") + code;
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  body.trim();
  return !body.isEmpty();
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED || cityName.isEmpty()) return false;
  // Throttle failed lookups too, so an unreachable API does not cause a new
  // request on every main-loop iteration.
  lastWeatherMs = millis();
  lastError = "";
  DynamicJsonDocument geo(6144);
  const String geoUrl = String("http://geocoding-api.open-meteo.com/v1/search?name=") +
    urlEncode(cityName) + "&count=1&language=ru&format=json";
  if (httpGetJson(geoUrl, geo)) {
    JsonArray results = geo["results"].as<JsonArray>();
    if (!results.isNull() && results.size() > 0) {
      JsonObject place = results[0];
      resolvedTimezone = place["timezone"] | "";
      if (timezoneName == "auto" && !resolvedTimezone.isEmpty())
        configureTimezone(resolvedTimezone);
      displayCity = String((const char *)(place["name"] | cityName.c_str()));
      const char *country = place["country"] | "";
      if (*country) displayCity += String(", ") + country;
    }
  } else {
    Serial.printf("Geocoding request failed; using city name directly: %s\n", lastError.c_str());
    lastError = "";
  }

  // Open-Meteo's forecast host refuses connections from this ESP32. wttr.in's
  // compact text format is key-free and avoids large JSON responses.
  const String weatherUrl = String("http://wttr.in/") + urlEncode(cityName) +
    "?format=%t%7C%f%7C%h%7C%w%7C%C";
  String body;
  if (!httpGetText(weatherUrl, body)) {
    Serial.printf("Forecast request failed: %s\n", lastError.c_str());
    return false;
  }
  String fields[5];
  int fieldStart = 0;
  for (int i = 0; i < 4; ++i) {
    const int separator = body.indexOf('|', fieldStart);
    if (separator < 0) {
      lastError = String("Bad weather response: ") + body.substring(0, 40);
      Serial.println(lastError);
      return false;
    }
    fields[i] = body.substring(fieldStart, separator);
    fieldStart = separator + 1;
  }
  fields[4] = body.substring(fieldStart);
  if (fields[0].isEmpty() || fields[2].isEmpty() || fields[4].isEmpty()) {
    lastError = "Incomplete weather response";
    return false;
  }
  temperatureC = atof(fields[0].c_str());
  feelsLikeC = atof(fields[1].c_str());
  humidity = atoi(fields[2].c_str());
  const char *windStart = fields[3].c_str();
  while (*windStart && ((*windStart < '0' || *windStart > '9') && *windStart != '-')) ++windStart;
  windKmh = atof(windStart);
  weatherDescription = fields[4];
  String sky = weatherDescription;
  sky.toLowerCase();
  if (sky.indexOf("thunder") >= 0) { weatherCode = 95; weatherDescription = "Гроза"; }
  else if (sky.indexOf("snow") >= 0) {
    weatherCode = 71;
    weatherDescription = sky.indexOf("light") >= 0 ? "Небольшой снег" : "Снег";
  }
  else if (sky.indexOf("rain") >= 0 || sky.indexOf("drizzle") >= 0 || sky.indexOf("shower") >= 0) {
    weatherCode = 61;
    weatherDescription = sky.indexOf("light") >= 0 ? "Небольшой дождь" : "Дождь";
  }
  else if (sky.indexOf("fog") >= 0 || sky.indexOf("mist") >= 0) { weatherCode = 45; weatherDescription = "Туман"; }
  else if (sky.indexOf("clear") >= 0 || sky.indexOf("sun") >= 0) { weatherCode = 0; weatherDescription = "Ясно"; }
  else if (sky.indexOf("partly") >= 0) { weatherCode = 2; weatherDescription = "Переменная облачность"; }
  else { weatherCode = 3; weatherDescription = "Облачно"; }
  condition = String("Ощущается: ") + String(feelsLikeC, 1) + " C";
  weatherReady = true;
  lastWeatherMs = millis();
  lastError = "";
  Serial.printf("Weather: %.1f C, %s, %d%% humidity, wind %.0f km/h, %s, %s\n",
    temperatureC, weatherDescription.c_str(), humidity, windKmh, condition.c_str(), displayCity.c_str());
  return true;
}

const char *conditionLabel(int code) {
  if (code == 0) return "Clear sky";
  if (code == 1) return "Mostly clear";
  if (code == 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code == 51 || code == 53 || code == 55) return "Drizzle";
  if (code == 56 || code == 57) return "Freezing drizzle";
  if (code == 61 || code == 63 || code == 65) return "Rain";
  if (code == 66 || code == 67) return "Freezing rain";
  if (code == 71 || code == 73 || code == 75 || code == 77) return "Snow";
  if (code == 80 || code == 81 || code == 82) return "Rain showers";
  if (code == 85 || code == 86) return "Snow showers";
  if (code == 95) return "Thunderstorm";
  if (code == 96 || code == 99) return "Thunder / hail";
  return "Weather unavailable";
}

void drawSun(int x, int y) {
  tft.fillCircle(x, y, 17, TFT_YELLOW);
  for (int a = 0; a < 360; a += 45) {
    const float r = a * 0.0174533f;
    tft.drawLine(x + int(23 * cosf(r)), y + int(23 * sinf(r)),
                 x + int(30 * cosf(r)), y + int(30 * sinf(r)), TFT_YELLOW);
  }
}

void drawCloud(int x, int y, uint16_t color) {
  tft.fillCircle(x - 13, y + 2, 13, color);
  tft.fillCircle(x + 2, y - 5, 18, color);
  tft.fillCircle(x + 19, y + 3, 12, color);
  tft.fillRoundRect(x - 23, y, 46, 17, 8, color);
}

void drawWeatherIcon(int code, int x, int y) {
  if (code == 0 || code == 1) { drawSun(x, y); return; }
  if (code == 2) { drawSun(x - 7, y - 6); drawCloud(x + 5, y + 5, TFT_LIGHTGREY); return; }
  if (code == 3 || code == 45 || code == 48) { drawCloud(x, y, TFT_LIGHTGREY); return; }
  drawCloud(x, y - 4, TFT_LIGHTGREY);
  if (code >= 71 && code <= 86) {
    for (int i = -12; i <= 12; i += 12) tft.drawLine(x + i, y + 18, x + i - 3, y + 25, TFT_CYAN);
  } else {
    for (int i = -12; i <= 12; i += 12) tft.drawLine(x + i, y + 17, x + i - 4, y + 26, TFT_BLUE);
    if (code >= 95) tft.drawLine(x + 12, y + 17, x + 5, y + 27, TFT_YELLOW);
  }
}

bool getLocalTimeSafe(struct tm &tmNow) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, &tmNow);
  return true;
}

String dateString(const struct tm &t) {
  static const char *months[] = {"ЯНВ", "ФЕВ", "МАР", "АПР", "МАЙ", "ИЮН", "ИЮЛ", "АВГ", "СЕН", "ОКТ", "НОЯ", "ДЕК"};
  static const char *days[] = {"ВС", "ПН", "ВТ", "СР", "ЧТ", "ПТ", "СБ"};
  char buf[48];
  snprintf(buf, sizeof(buf), "%s, %02d %s %04d", days[t.tm_wday], t.tm_mday, months[t.tm_mon], t.tm_year + 1900);
  return String(buf);
}

String settingsPage() {
  String html = F("<!doctype html><html lang=ru><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>CYD Weather</title><style>body{font:16px system-ui;background:#101827;color:#edf3ff;max-width:520px;margin:24px auto;padding:0 16px}"
    "main{background:#1b2940;padding:22px;border-radius:16px}h1{font-size:24px;margin:0 0 8px}p{color:#bac9df}label{display:block;margin:18px 0 6px}"
    "input{box-sizing:border-box;width:100%;padding:12px;border-radius:9px;border:1px solid #53647e;background:#0f1929;color:white;font-size:16px}"
    "button,a{display:block;margin-top:22px;width:100%;padding:13px;border:0;border-radius:9px;background:#42d6b0;color:#08231e;font-size:17px;font-weight:700;text-align:center;text-decoration:none}</style>");
  html += F("<main><h1>Погода, дата и время</h1><p>Время синхронизируется по сети. Укажите смещение от UTC; для Красноярска — UTC+7.</p>");
  html += F("<form method=post action=/save><label>Сеть Wi-Fi (SSID)</label><input name=ssid required maxlength=32 autocomplete=username value='");
  html += htmlEscape(savedSsid);
  html += F("'><label>Пароль Wi-Fi</label><input name=password type=password maxlength=63 autocomplete=new-password placeholder='Для новой сети введите пароль'>");
  html += F("<p>Если оставить прежнее имя сети и пустой пароль, сохранится текущий пароль.</p>");
  html += F("<label>Имя раздаваемой Wi-Fi сети</label><input name=ap_ssid maxlength=32 value='");
  html += htmlEscape(apName);
  html += F("'><label>Пароль раздаваемой сети (не менее 8 символов)</label><input name=ap_password type=password minlength=8 maxlength=63 placeholder='Пустое поле оставит текущий пароль'>");
  html += F("<p>Подключённые устройства будут выходить в интернет через домашний Wi-Fi.</p>");
  html += F("<label>Город</label><input name=city maxlength=64 value='");
  html += htmlEscape(cityName);
  html += F("'><label>Часовой пояс (UTC±N; Красноярск — UTC+7)</label><input name=timezone maxlength=16 value='");
  html += normalizedTimezoneInput(timezoneName);
  html += F("'><button type=submit>Сохранить и перезапустить</button></form><a href='/'>Статус</a></main></html>");
  return html;
}

String statusPage() {
  String html = F("<!doctype html><html lang=ru><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>CYD Weather</title><style>body{font:16px system-ui;background:#101827;color:#edf3ff;max-width:520px;margin:24px auto;padding:0 16px}"
    "main{background:#1b2940;padding:22px;border-radius:16px}h1{font-size:24px;margin:0 0 8px}p{color:#bac9df}a{color:#42d6b0}</style>");
  html += F("<main><h1>CYD Weather</h1><p>Город: ");
  html += htmlEscape(displayCity.isEmpty() ? cityName : displayCity);
  html += F("</p><p>Время: ");
  struct tm now;
  if (getLocalTimeSafe(now)) {
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S — %d.%m.%Y", &now);
    html += timeBuf;
  } else {
    html += F("ожидаю синхронизацию NTP");
  }
  html += F("</p><p>Wi-Fi: ");
  html += WiFi.status() == WL_CONNECTED ? F("подключён") : F("нет подключения");
  html += F("</p><p>Погода: ");
  html += weatherReady ? F("обновлена") : htmlEscape(lastError.isEmpty() ? String("ожидание данных") : lastError);
  html += F("</p><p>Сеть раздачи: ");
  html += htmlEscape(apName);
  html += F(" (адрес 192.168.4.1)</p><p>Маршрутизация: ");
  html += naptEnabled ? F("активна") : F("нет подключения к домашней сети");
  html += F("</p><p><a href='/settings'>Настройки Wi-Fi, города и времени</a></p></main></html>");
  return html;
}

void beginWebServer() {
  server.on("/", HTTP_GET, []() {
    if (portalMode) server.send(200, "text/html; charset=utf-8", settingsPage());
    else server.send(200, "text/html; charset=utf-8", statusPage());
  });
  server.on("/settings", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", settingsPage());
  });
  server.on("/save", HTTP_POST, []() {
    const String ssid = server.arg("ssid");
    const String pass = server.arg("password");
    const String requestedApSsid = server.arg("ap_ssid");
    const String requestedApPassword = server.arg("ap_password");
    const String city = server.arg("city");
    String tz = server.arg("timezone");
    if ((portalMode && ssid.isEmpty()) || city.isEmpty() ||
        (!requestedApSsid.isEmpty() && requestedApPassword.length() > 0 && requestedApPassword.length() < 8)) {
      server.send(400, "text/plain; charset=utf-8", "Заполните Wi-Fi и город.");
      return;
    }
    const bool wifiSettingsSubmitted = server.hasArg("ssid") && !ssid.isEmpty();
    if (wifiSettingsSubmitted) {
      const bool networkChanged = ssid != savedSsid;
      savedSsid = ssid;
      if (!pass.isEmpty()) savedPassword = pass;
      else if (networkChanged) savedPassword = "";
    }
    cityName = city;
    if (!requestedApSsid.isEmpty()) apName = requestedApSsid;
    if (requestedApPassword.length() >= 8) apPassword = requestedApPassword;
    if (!tz.isEmpty()) timezoneName = tz;
    prefs.begin("cyd-weather", false);
    if (wifiSettingsSubmitted) {
      prefs.putString("ssid", savedSsid);
      prefs.putString("password", savedPassword);
    }
    prefs.putString("city", cityName);
    prefs.putString("timezone", timezoneName);
    prefs.putString("ap_ssid", apName);
    prefs.putString("ap_password", apPassword);
    prefs.end();
    server.send(200, "text/html; charset=utf-8",
      "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><body style='font:18px system-ui;background:#101827;color:white;padding:24px'><h2>Настройки сохранены</h2><p>Плата перезапустится.</p></body>");
    delay(900);
    ESP.restart();
  });
  server.onNotFound([]() { server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); });
  server.begin();
  Serial.println("Settings web server started");
}

void drawDashboard(bool full) {
  struct tm now;
  const bool validTime = getLocalTimeSafe(now);
  if (!full && validTime && now.tm_min == lastMinuteDrawn) return;
  if (!full && !validTime && millis() - lastDrawMs < 10000) return;
  lastDrawMs = millis();
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 320, 38, 0x10A3);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, 0x10A3);
  String city = displayCity.isEmpty() ? cityName : displayCity;
  if (city.length() > 22) city = city.substring(0, 20) + "..";
  drawUtf8(city, 12, 25, TFT_WHITE);
  const String wifiState = WiFi.status() == WL_CONNECTED ? "В СЕТИ" : "НЕТ СЕТИ";
  drawUtf8(wifiState, 320 - 12 - u8g2.getUTF8Width(wifiState.c_str()), 25,
    WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED);

  tft.setTextDatum(MC_DATUM);
  if (validTime) {
    if (!timeReady) {
      char localStamp[40];
      strftime(localStamp, sizeof(localStamp), "%Y-%m-%d %H:%M:%S %Z", &now);
      Serial.printf("Local clock: %s\n", localStamp);
    }
    char clockText[8];
    snprintf(clockText, sizeof(clockText), "%02d:%02d", now.tm_hour, now.tm_min);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(clockText, 160, 79, 7);
    drawUtf8Centered(dateString(now), 160, 137, TFT_CYAN);
    timeReady = true;
    lastMinuteDrawn = now.tm_min;
  } else {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("--:--", 160, 79, 7);
    drawUtf8Centered("Синхронизация времени...", 160, 137, TFT_ORANGE);
  }
  tft.drawFastHLine(12, 145, 296, 0x39E7);

  if (weatherReady) {
    drawWeatherIcon(weatherCode, 57, 183);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    char temp[16];
    snprintf(temp, sizeof(temp), "%.1f C", temperatureC);
    tft.drawString(temp, 100, 170, 4);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    drawUtf8(weatherDescription, 100, 205, TFT_CYAN);
    char details[64];
    snprintf(details, sizeof(details), "Влажность %d%%  Ветер %.0f км/ч", humidity, windKmh);
    drawUtf8(details, 100, 228, TFT_LIGHTGREY);
    drawUtf8Centered(condition, 160, 258, TFT_YELLOW);
  } else {
    drawUtf8Centered("Нет данных о погоде", 160, 190, TFT_ORANGE);
    drawUtf8Centered("Повторяем запрос...", 160, 214, TFT_LIGHTGREY);
  }
  drawUtf8Centered(String("Настройка: ") + apName, 160, 313, TFT_DARKGREY);
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  // Match the working NerdMiner backlight setup: active-high PWM on GPIO21.
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 250);
  tft.init();
#ifdef TFT_INVERSION_ON
  const bool panelInverted = true;
#else
  const bool panelInverted = false;
#endif
  tft.invertDisplay(panelInverted);
  tft.setRotation(1);
  tft.setSwapBytes(true);
  u8g2.begin(tft);
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);
  if (panelInverted) {
    tft.writecommand(0x26); // ILI9341 gamma curve selection, used by the working miner.
    tft.writedata(2);
    delay(120);
    tft.writecommand(0x26);
    tft.writedata(1);
  }
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(false);
  showMessage("Погода", "Запуск...");

  prefs.begin("cyd-weather", true);
  savedSsid = prefs.getString("ssid", "");
  savedPassword = prefs.getString("password", "");
  cityName = prefs.getString("city", "Krasnoyarsk");
  timezoneName = prefs.getString("timezone", "UTC+7");
  apName = prefs.getString("ap_ssid", "");
  apPassword = prefs.getString("ap_password", SETTINGS_AP_PASSWORD);
  prefs.end();

  if (!connectWifi()) {
    startPortal();
    beginWebServer();
    return;
  }
  beginWebServer();
  fetchWeather();
  drawDashboard(true);
}

void loop() {
  server.handleClient();
  if (portalMode) {
    delay(2);
    return;
  }

  if (digitalRead(BOOT_BUTTON) == LOW) {
    if (bootPressMs == 0) bootPressMs = millis();
    else if (millis() - bootPressMs > 2500) {
      bootPressMs = 0;
      startPortal();
      return;
    }
  } else {
    bootPressMs = 0;
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(100);
  } else if (!naptEnabled) {
    enableNapt();
  }
  const uint32_t refreshAfter = weatherReady ? WEATHER_INTERVAL_MS : WEATHER_RETRY_MS;
  if (millis() - lastWeatherMs >= refreshAfter) {
    if (fetchWeather()) drawDashboard(true);
    else if (!weatherReady) drawDashboard(true);
  }
  drawDashboard(false);
  delay(100);
}


