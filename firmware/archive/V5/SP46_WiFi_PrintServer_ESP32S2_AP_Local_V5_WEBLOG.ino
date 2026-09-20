/*
  SHOPTIDA SP46 Wi-Fi Print Server
  ESP32-S2 + USB Host + RAW TCP 9100

  Features:
  - AP setup SSID: "ESP print sever XXXX" (XXXX = last 4 MAC hex chars)
  - AP IP: 192.168.10.1
  - Captive portal DNS -> 192.168.10.1
  - Local hostname: esp-print-sever-xxxx.local
  - Wi-Fi credentials saved in Preferences
  - Web status/debug page
  - Wi-Fi scan + manual SSID/password
  - Forget Wi-Fi + restart
  - LED GPIO15:
      ON solid = normal
      slow blink = no home Wi-Fi
      2 quick blinks + pause = USB/printer error
  - TCP RAW port 9100 -> USB Printer Class BULK OUT

  USB wiring:
    D19 -> 22R -> USB D-
    D20 -> 22R -> USB D+
    VBUS -> USB +5V
    GND  -> USB GND
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "EspUsbHost.h"

// ========================= GENERAL =========================
static const uint16_t PRINT_PORT = 9100;
static const uint8_t LED_PIN = 15;
static const bool LED_ACTIVE_LOW = false;

// AP is always available, so setup/recovery is easy.
IPAddress AP_IP(192, 168, 10, 1);
IPAddress AP_GW(192, 168, 10, 1);
IPAddress AP_MASK(255, 255, 255, 0);

String macSuffix;
String apSsid;
String hostName;        // no spaces, no ".local"
String localUrl;

// ========================= STORAGE =========================
Preferences prefs;
String savedSsid;
String savedPass;

// ========================= NETWORK =========================
WebServer web(80);
DNSServer dns;
WiFiServer printServer(PRINT_PORT);
WiFiClient printClient;

bool mdnsStarted = false;
bool tcpStarted = false;
uint32_t lastWifiAttempt = 0;
uint32_t lastWifiScan = 0;

uint32_t totalJobs = 0;
uint64_t totalBytes = 0;
uint8_t netBuffer[2048];

// Windows RAW 9100 job handling.
// Close the TCP job after a short idle period so Windows Spooler knows
// the job has finished, even when a driver keeps the RAW socket open.
static const uint32_t PRINT_JOB_IDLE_CLOSE_MS = 2200;
uint32_t jobLastActivityMs = 0;
uint32_t currentJobBytes = 0;
bool currentJobHasData = false;

// USB Printer Class protocol 2/3 can be bidirectional.
// Forward BULK IN status/replies from printer back to the TCP client.
uint8_t usbInBuffer[512];

// ========================= USB =========================
EspUsbHost usb;

bool usbDeviceChanged = false;
bool printerReady = false;
bool fatalUsbHostError = false;
bool usbRuntimeError = false;

uint8_t printerAddress = 0;
uint8_t printerInterface = 0xFF;
uint8_t printerProtocol = 0;
uint16_t printerVid = 0;
uint16_t printerPid = 0;

String lastError = "None";
String lastError = "None";

// ========================= WEB LOG =========================
// Keeps recent diagnostic events in RAM. The log is intentionally not
// written continuously to flash, avoiding unnecessary flash wear.
static const size_t LOG_MAX_CHARS = 18000;
String systemLog;

String uptimeStamp() {
  uint32_t ms = millis();
  uint32_t totalSec = ms / 1000UL;
  uint32_t h = totalSec / 3600UL;
  uint32_t m = (totalSec % 3600UL) / 60UL;
  uint32_t s = totalSec % 60UL;
  uint32_t milli = ms % 1000UL;

  char buf[24];
  snprintf(buf, sizeof(buf), "[%03lu:%02lu:%02lu.%03lu] ",
           (unsigned long)h,
           (unsigned long)m,
           (unsigned long)s,
           (unsigned long)milli);
  return String(buf);
}

void addLog(const String &msg) {
  String line = uptimeStamp() + msg + "\n";
  systemLog += line;

  // Trim oldest complete lines when buffer grows too large.
  if (systemLog.length() > LOG_MAX_CHARS) {
    size_t removeTarget = systemLog.length() - (LOG_MAX_CHARS * 3 / 4);
    int newline = systemLog.indexOf('\n', (unsigned int)removeTarget);
    if (newline >= 0) {
      systemLog.remove(0, newline + 1);
    } else {
      systemLog.remove(0, removeTarget);
    }
  }

  Serial.print(line);
}

// ========================= LED =========================
enum LedMode {
  LED_NORMAL_ON,
  LED_NO_HOME_WIFI,
  LED_ERROR_PATTERN
};

LedMode ledMode = LED_NORMAL_ON;

void ledWrite(bool on) {
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? !on : on);
}

void setLastError(const String &msg) {
  if (lastError != msg) {
    lastError = msg;
    if (msg != "None") {
      addLog("ERROR: " + msg);
    } else {
      addLog("Error state cleared");
    }
  }
}

void refreshLedMode() {
  if (fatalUsbHostError || usbRuntimeError) {
    ledMode = LED_ERROR_PATTERN;
  } else if (WiFi.status() != WL_CONNECTED) {
    ledMode = LED_NO_HOME_WIFI;
  } else {
    ledMode = LED_NORMAL_ON;
  }
}

void updateStatusLed() {
  static LedMode previousMode = (LedMode)255;
  static uint32_t phaseStarted = 0;
  static uint8_t phase = 0;

  if (previousMode != ledMode) {
    previousMode = ledMode;
    phase = 0;
    phaseStarted = millis();
  }

  uint32_t now = millis();

  if (ledMode == LED_NORMAL_ON) {
    ledWrite(true);
    return;
  }

  if (ledMode == LED_NO_HOME_WIFI) {
    ledWrite(((now / 700UL) % 2UL) == 0);
    return;
  }

  // Two quick blinks, then pause.
  static const uint16_t durations[] = {120, 120, 120, 1200};
  static const bool states[] = {true, false, true, false};

  ledWrite(states[phase]);
  if (now - phaseStarted >= durations[phase]) {
    phase = (phase + 1) % 4;
    phaseStarted = now;
  }
}

// ========================= HELPERS =========================
String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else out += c;
  }
  return out;
}

String yesNo(bool v) {
  return v ? "OK" : "NO";
}

void buildIdentity() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t last16 = (uint16_t)(mac & 0xFFFF);

  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", last16);
  macSuffix = suffix;

  apSsid = "ESP print sever " + macSuffix;

  String lower = macSuffix;
  lower.toLowerCase();
  hostName = "esp-print-sever-" + lower;
  localUrl = "http://" + hostName + ".local";
}

// ========================= USB PRINTER =========================
bool findAndOpenPrinter() {
  if (printerReady) return true;

  EspUsbHostDeviceInfo devices[ESP_USB_HOST_MAX_DEVICES];
  size_t deviceCount = usb.getDevices(devices, ESP_USB_HOST_MAX_DEVICES);

  for (size_t d = 0; d < deviceCount; d++) {
    EspUsbHostInterfaceInfo interfaces[ESP_USB_HOST_MAX_INTERFACES];
    size_t interfaceCount = usb.getInterfaces(
      devices[d].address,
      interfaces,
      ESP_USB_HOST_MAX_INTERFACES
    );

    for (size_t i = 0; i < interfaceCount; i++) {
      if (interfaces[i].interfaceClass != 0x07) continue; // USB Printer Class

      uint8_t address = devices[d].address;
      uint8_t iface = interfaces[i].number;
      uint8_t proto = interfaces[i].interfaceProtocol;

      EspUsbHostVendorReadMode readMode =
        (proto >= 0x02)
          ? ESP_USB_HOST_VENDOR_READ_CONTINUOUS
          : ESP_USB_HOST_VENDOR_READ_ON_DEMAND;

      if (!usb.vendorOpen(address, iface, readMode)) {
        String e = "Cannot claim printer interface: ";
        e += usb.lastErrorName();
        setLastError(e);
        usbRuntimeError = true;
        return false;
      }

      usb.vendorSetAutoZlp(true, address);

      printerAddress = address;
      printerInterface = iface;
      printerProtocol = proto;
      printerVid = devices[d].vid;
      printerPid = devices[d].pid;
      printerReady = true;
      usbRuntimeError = false;
      setLastError("None");

      addLog("USB printer ready VID:PID=" +
             String(printerVid, HEX) + ":" + String(printerPid, HEX) +
             " interface=" + String(printerInterface) +
             " protocol=" + String(printerProtocol));

      Serial.println();
      Serial.println("USB PRINTER READY");
      Serial.printf("VID:PID   %04X:%04X\n", printerVid, printerPid);
      Serial.printf("Interface %u\n", printerInterface);
      Serial.printf("Protocol  0x%02X\n", printerProtocol);
      Serial.printf("Bulk OUT  0x%02X\n", usb.vendorOutEndpoint(address));
      Serial.printf("OUT MPS   %u\n", usb.vendorOutPacketSize(address));
      return true;
    }
  }

  return false;
}

bool printerStillPresent() {
  if (!printerReady) return false;
  EspUsbHostDeviceInfo dev;
  return usb.getDevice(printerAddress, dev);
}

bool sendToPrinter(const uint8_t *data, size_t len) {
  if (!printerReady || !data || len == 0) return false;

  if (!usb.vendorWrite(data, len, printerAddress)) {
    String e = "USB write failed: ";
    e += usb.lastErrorName();
    setLastError(e);
    usbRuntimeError = true;
    printerReady = false;
    printerAddress = 0;
    printerInterface = 0xFF;
    return false;
  }

  totalBytes += len;
  return true;
}

// ========================= WI-FI =========================
void loadWifiSettings() {
  prefs.begin("wifi", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
}

void saveWifiSettings(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  savedSsid = ssid;
  savedPass = pass;
}

void forgetWifiSettings() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();

  savedSsid = "";
  savedPass = "";

  WiFi.disconnect(true, true);
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);

  // Open AP: easiest for setup. Can add password later for retail units.
  WiFi.softAP(apSsid.c_str());

  dns.start(53, "*", AP_IP);

  Serial.println();
  Serial.println("SETUP AP READY");
  Serial.printf("SSID : %s\n", apSsid.c_str());
  Serial.printf("IP   : %s\n", AP_IP.toString().c_str());
  addLog("Setup AP started: " + apSsid + " @ " + AP_IP.toString());
}

void startMdnsIfNeeded() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) return;

  if (MDNS.begin(hostName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("pdl-datastream", "tcp", PRINT_PORT);
    mdnsStarted = true;
    Serial.printf("Local: %s\n", localUrl.c_str());
    addLog("mDNS started: " + localUrl);
  }
}

void connectSavedWifi() {
  if (savedSsid.length() == 0) return;

  WiFi.setHostname(hostName.c_str());
  WiFi.setSleep(false);

  Serial.printf("Connecting home Wi-Fi: %s\n", savedSsid.c_str());
  addLog("Connecting home Wi-Fi: " + savedSsid);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  lastWifiAttempt = millis();
}

void maintainWifi() {
  static wl_status_t previousStatus = WL_NO_SHIELD;

  wl_status_t nowStatus = WiFi.status();

  if (nowStatus != previousStatus) {
    if (nowStatus == WL_CONNECTED) {
      addLog("Home Wi-Fi connected: " + WiFi.SSID() +
             " IP=" + WiFi.localIP().toString());
    } else if (previousStatus == WL_CONNECTED) {
      addLog("Home Wi-Fi disconnected");
      mdnsStarted = false;
    }
    previousStatus = nowStatus;
  }

  if (nowStatus == WL_CONNECTED) {
    startMdnsIfNeeded();

    if (!tcpStarted) {
      printServer.begin();
      tcpStarted = true;
      Serial.printf("TCP RAW print server: %u\n", PRINT_PORT);
      addLog("RAW TCP print server started on port " + String(PRINT_PORT));
    }
    return;
  }

  if (savedSsid.length() > 0 && millis() - lastWifiAttempt >= 10000) {
    lastWifiAttempt = millis();
    addLog("Retrying home Wi-Fi: " + savedSsid);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  }
}

// ========================= WEB UI =========================
String makePage() {
  String p;
  p.reserve(9000);

  p += F(
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'>"
    "<title>ESP Print Server</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;background:#f3f5f7;margin:0;color:#171717}"
    ".w{max-width:760px;margin:auto;padding:18px}"
    ".c{background:#fff;border-radius:14px;padding:16px;margin:12px 0;box-shadow:0 2px 12px #0001}"
    "h1{font-size:23px;margin:4px 0 14px}"
    "h2{font-size:17px;margin:0 0 12px}"
    ".g{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
    ".kv{padding:9px;background:#f6f7f9;border-radius:9px;word-break:break-word}"
    ".ok{color:#087f23;font-weight:700}.bad{color:#c62828;font-weight:700}"
    "input,select,button{width:100%;box-sizing:border-box;padding:11px;margin:6px 0;border-radius:9px;border:1px solid #ccc;font-size:15px}"
    "button{border:0;background:#111;color:#fff;font-weight:700;cursor:pointer}"
    ".red{background:#b71c1c}.muted{color:#666;font-size:13px}"
    "a{color:#0756c7;text-decoration:none}"
    "</style></head><body><div class='w'>"
  );

  p += "<h1>ESP Print Server " + macSuffix + "</h1><p class='muted'>Firmware V5 · RAW job completion + USB bidirectional + Web Log</p>";

  p += F("<div class='c'><h2>Trạng thái</h2><div class='g'>");

  p += "<div class='kv'>Wi-Fi nhà<br><b>";
  if (WiFi.status() == WL_CONNECTED) {
    p += "<span class='ok'>Đã kết nối</span>";
  } else {
    p += "<span class='bad'>Chưa kết nối</span>";
  }
  p += "</b></div>";

  p += "<div class='kv'>Máy in USB<br><b>";
  p += printerReady ? "<span class='ok'>Đã kết nối</span>" : "<span class='bad'>Chưa nhận</span>";
  p += "</b></div>";

  p += "<div class='kv'>IP mạng nhà<br><b>";
  p += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "-";
  p += "</b></div>";

  p += "<div class='kv'>IP cài đặt<br><b>192.168.10.1</b></div>";

  p += "<div class='kv'>Local link<br><b>";
  p += localUrl;
  p += "</b></div>";

  p += "<div class='kv'>TCP RAW<br><b>9100</b><br><span class='muted'>Auto job close 2.2s</span></div>";

  p += "<div class='kv'>VID:PID<br><b>";
  if (printerReady) {
    char vp[16];
    snprintf(vp, sizeof(vp), "%04X:%04X", printerVid, printerPid);
    p += vp;
  } else p += "-";
  p += "</b></div>";

  p += "<div class='kv'>Jobs / Bytes<br><b>";
  p += String(totalJobs);
  p += " / ";
  p += String((unsigned long long)totalBytes);
  p += "</b></div>";

  p += F("</div><p class='muted'>Lỗi cuối: ");
  p += htmlEscape(lastError);
  p += F("</p></div>");

  p += F(
    "<div class='c'><h2>Cài Wi-Fi nhà</h2>"
    "<form method='POST' action='/wifi'>"
    "<label>SSID</label>"
    "<input name='ssid' id='ssid' placeholder='Tên Wi-Fi' value='"
  );
  p += htmlEscape(savedSsid);
  p += F(
    "'>"
    "<label>Mật khẩu</label>"
    "<input name='pass' type='password' placeholder='Mật khẩu Wi-Fi'>"
    "<button type='submit'>Lưu & kết nối</button>"
    "</form>"
    "<button onclick=\"location.href='/scan'\">Quét Wi-Fi</button>"
    "</div>"
  );

  p += F("<div class='c'><h2>Thông tin thiết bị</h2>");
  p += "<div class='kv'>AP: <b>" + htmlEscape(apSsid) + "</b></div>";
  p += "<div class='kv'>Local: <b>" + htmlEscape(localUrl) + "</b></div>";
  p += "<div class='kv'>MAC suffix: <b>" + htmlEscape(macSuffix) + "</b></div>";
  p += F("</div>");

  p += F(
    "<div class='c'><h2>Nhật ký hệ thống</h2>"
    "<p class='muted'>Xem lỗi USB/Wi-Fi/job in và tải log về dạng TXT.</p>"
    "<button onclick=\"location.href='/log'\">Xem log hệ thống</button>"
    "<button onclick=\"location.href='/log.txt'\">Tải log .txt</button>"
    "</div>"
    "<div class='c'>"
    "<form method='POST' action='/forget'><button class='red' type='submit'>Quên Wi-Fi nhà</button></form>"
    "<form method='POST' action='/restart'><button type='submit'>Khởi động lại</button></form>"
    "</div>"
    "</div></body></html>"
  );

  return p;
}

void handleRoot() {
  web.send(200, "text/html; charset=utf-8", makePage());
}

void handleWifiSave() {
  if (!web.hasArg("ssid")) {
    web.send(400, "text/plain", "Missing SSID");
    return;
  }

  String ssid = web.arg("ssid");
  String pass = web.arg("pass");

  ssid.trim();
  if (ssid.length() == 0) {
    web.send(400, "text/plain", "SSID empty");
    return;
  }

  saveWifiSettings(ssid, pass);
  addLog("Wi-Fi settings saved for SSID: " + ssid);

  mdnsStarted = false;
  WiFi.disconnect(false, false);
  delay(150);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  lastWifiAttempt = millis();

  web.send(
    200,
    "text/html; charset=utf-8",
    "<html><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:Arial;padding:25px'>"
    "<h2>Đã lưu Wi-Fi</h2>"
    "<p>ESP đang kết nối. AP 192.168.10.1 vẫn hoạt động.</p>"
    "<p>Sau khi kết nối thành công, mở local link của thiết bị.</p>"
    "<p><a href='/'>Quay lại</a></p></body></html>"
  );
}

void handleScan() {
  int n = WiFi.scanNetworks(false, true);

  String p =
    "<html><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:Arial;padding:18px'><h2>Wi-Fi tìm thấy</h2>";

  if (n <= 0) {
    p += "<p>Không tìm thấy mạng.</p>";
  } else {
    for (int i = 0; i < n; i++) {
      String ss = htmlEscape(WiFi.SSID(i));
      p += "<p><a href='/?ssid=" + ss + "'>" + ss + "</a> ";
      p += String(WiFi.RSSI(i));
      p += " dBm</p>";
    }
  }

  p += "<p><a href='/'>Quay lại</a></p></body></html>";
  WiFi.scanDelete();

  web.send(200, "text/html; charset=utf-8", p);
}

void handleForget() {
  addLog("Forget home Wi-Fi requested");
  forgetWifiSettings();
  mdnsStarted = false;
  web.send(
    200,
    "text/html; charset=utf-8",
    "<html><body style='font-family:Arial;padding:25px'>"
    "<h2>Đã quên Wi-Fi nhà</h2>"
    "<p>Kết nối lại AP của thiết bị và mở 192.168.10.1.</p>"
    "</body></html>"
  );
}

void handleRestart() {
  addLog("Restart requested from web UI");
  web.send(200, "text/plain", "Restarting...");
  delay(400);
  ESP.restart();
}

void captiveRedirect() {
  web.sendHeader("Location", "http://192.168.10.1/", true);
  web.send(302, "text/plain", "");
}

String makeLogPage() {
  String p;
  p.reserve(systemLog.length() + 4500);

  p += F(
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='4'>"
    "<title>ESP Print Server Log</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;background:#f3f5f7;margin:0;color:#171717}"
    ".w{max-width:900px;margin:auto;padding:18px}"
    ".c{background:#fff;border-radius:14px;padding:16px;margin:12px 0;box-shadow:0 2px 12px #0001}"
    "h1{font-size:22px;margin:4px 0 8px}"
    ".muted{color:#666;font-size:13px}"
    ".actions{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}"
    "a,button{display:inline-block;padding:10px 14px;border-radius:9px;border:0;background:#111;color:#fff;text-decoration:none;font-weight:700;font-size:14px}"
    ".red{background:#b71c1c}"
    "pre{background:#0f1115;color:#d7e1ea;padding:14px;border-radius:10px;overflow:auto;white-space:pre-wrap;word-break:break-word;min-height:360px;font:12px/1.45 Consolas,monospace}"
    "</style></head><body><div class='w'>"
  );

  p += "<h1>System Log · " + htmlEscape(macSuffix) + "</h1>";
  p += "<p class='muted'>Log phiên hiện tại từ lúc khởi động · tự làm mới mỗi 4 giây · ";
  p += String(systemLog.length());
  p += " bytes</p>";

  p += F("<div class='actions'>"
         "<a href='/'>Trang chính</a>"
         "<a href='/log.txt'>Tải log .txt</a>"
         "<a href='/log'>Làm mới</a>"
         "<form method='POST' action='/log/clear' style='margin:0'>"
         "<button class='red' type='submit'>Xóa log</button></form>"
         "</div>");

  p += F("<div class='c'><pre>");
  if (systemLog.length() == 0) {
    p += "(Chưa có log)";
  } else {
    p += htmlEscape(systemLog);
  }
  p += F("</pre></div></div></body></html>");
  return p;
}

void handleLogPage() {
  web.send(200, "text/html; charset=utf-8", makeLogPage());
}

void handleLogDownload() {
  String filename = "ESP_Print_Server_" + macSuffix + "_log.txt";
  web.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  web.sendHeader("Cache-Control", "no-store");

  String header;
  header.reserve(600);
  header += "ESP Print Server Log\n";
  header += "Device: " + apSsid + "\n";
  header += "Local: " + localUrl + "\n";
  header += "AP IP: " + AP_IP.toString() + "\n";
  header += "Home Wi-Fi: ";
  header += (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("Not connected");
  header += "\nHome IP: ";
  header += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("-");
  header += "\nPrinter: ";
  header += printerReady ? "Connected" : "Not connected";
  header += "\nLast error: " + lastError;
  header += "\nJobs: " + String(totalJobs);
  header += "\nBytes forwarded: " + String((unsigned long long)totalBytes);
  header += "\nFirmware: V5 WEBLOG\n";
  header += "----------------------------------------\n";

  web.send(200, "text/plain; charset=utf-8", header + systemLog);
}

void handleLogClear() {
  systemLog = "";
  addLog("Log cleared from web UI");
  web.sendHeader("Location", "/log", true);
  web.send(303, "text/plain", "");
}

void startWeb() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/wifi", HTTP_POST, handleWifiSave);
  web.on("/scan", HTTP_GET, handleScan);
  web.on("/forget", HTTP_POST, handleForget);
  web.on("/restart", HTTP_POST, handleRestart);
  web.on("/log", HTTP_GET, handleLogPage);
  web.on("/log.txt", HTTP_GET, handleLogDownload);
  web.on("/log/clear", HTTP_POST, handleLogClear);

  // Common captive portal probe URLs
  web.on("/generate_204", HTTP_ANY, captiveRedirect);
  web.on("/gen_204", HTTP_ANY, captiveRedirect);
  web.on("/hotspot-detect.html", HTTP_ANY, captiveRedirect);
  web.on("/connecttest.txt", HTTP_ANY, captiveRedirect);
  web.on("/ncsi.txt", HTTP_ANY, captiveRedirect);
  web.onNotFound(captiveRedirect);

  web.begin();
}

// ========================= TCP PRINT =========================
void handlePrintClient() {
  // Accept a new RAW 9100 job when no client is active.
  if (!printClient || !printClient.connected()) {
    if (printClient) {
      printClient.stop();
    }

    WiFiClient c = printServer.available();
    if (c) {
      printClient = c;
      printClient.setNoDelay(true);

      totalJobs++;
      currentJobBytes = 0;
      addLog("Print job #" + String(totalJobs) +
             " connected from " + printClient.remoteIP().toString());
      currentJobHasData = false;
      jobLastActivityMs = millis();

      Serial.printf("Print job #%lu from %s\n",
                    (unsigned long)totalJobs,
                    printClient.remoteIP().toString().c_str());
    }
    return;
  }

  // If a PC connected but the printer is unavailable, do not leave
  // Windows stuck forever in "Printing".
  if (!printerReady) {
    if (millis() - jobLastActivityMs >= 3000) {
      Serial.println("Closing RAW job: printer not ready");
      printClient.stop();
    }
    return;
  }

  // ---------- PC -> USB printer ----------
  int available = printClient.available();

  while (available > 0 && printClient.connected()) {
    size_t wanted = (available > (int)sizeof(netBuffer))
                      ? sizeof(netBuffer)
                      : (size_t)available;

    int got = printClient.read(netBuffer, wanted);
    if (got <= 0) break;

    if (!sendToPrinter(netBuffer, (size_t)got)) {
      Serial.println("Closing RAW job: USB write failed");
      printClient.stop();
      return;
    }

    currentJobBytes += (uint32_t)got;
    currentJobHasData = true;
    jobLastActivityMs = millis();

    available = printClient.available();
    yield();
  }

  // ---------- USB printer -> PC ----------
  // Protocol 2/3 printers may return device/driver status on BULK IN.
  // EspUsbHost buffers continuous IN data; vendorRead() is non-blocking.
  if (printerReady && printerProtocol >= 0x02 && printClient.connected()) {
    size_t inLen = usb.vendorRead(usbInBuffer,
                                  sizeof(usbInBuffer),
                                  printerAddress);

    if (inLen > 0) {
      size_t written = printClient.write(usbInBuffer, inLen);
      if (written > 0) {
        jobLastActivityMs = millis();
      }
    }
  }

  // Windows Standard TCP/IP RAW spoolers/drivers may keep the socket open
  // waiting for a completion/status condition.  Once data has been sent and
  // both directions are idle for a short period, close the connection.
  // That gives Windows a deterministic end-of-job signal.
  if (currentJobHasData &&
      printClient.connected() &&
      millis() - jobLastActivityMs >= PRINT_JOB_IDLE_CLOSE_MS) {

    Serial.printf("Print job #%lu complete: %lu bytes; closing RAW socket\n",
                  (unsigned long)totalJobs,
                  (unsigned long)currentJobBytes);
    addLog("Print job #" + String(totalJobs) +
           " complete, " + String(currentJobBytes) + " bytes");

    printClient.flush();
    delay(5);
    printClient.stop();

    currentJobHasData = false;
    currentJobBytes = 0;
  }
}

// ========================= SETUP / LOOP =========================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  ledWrite(true);

  Serial.begin(115200);
  delay(1000);

  systemLog.reserve(LOG_MAX_CHARS + 512);
  buildIdentity();
  addLog("Boot: ESP Print Server V5 WEBLOG");
  addLog("Device identity: " + apSsid + " / " + localUrl);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" ESP32-S2 SHOPTIDA SP46 PRINT SERVER");
  Serial.println("======================================");
  Serial.printf("AP SSID : %s\n", apSsid.c_str());
  Serial.printf("AP IP   : %s\n", AP_IP.toString().c_str());
  Serial.printf("Local   : %s\n", localUrl.c_str());

  loadWifiSettings();

  startAccessPoint();
  startWeb();

  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &device) {
    Serial.printf("USB connected: addr=%u VID=%04X PID=%04X\n",
                  device.address, device.vid, device.pid);
    addLog("USB device connected addr=" + String(device.address) +
           " VID:PID=" + String(device.vid, HEX) + ":" + String(device.pid, HEX));
    usbDeviceChanged = true;
  });

  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device) {
    Serial.printf("USB disconnected: addr=%u\n", device.address);
    addLog("USB device disconnected addr=" + String(device.address));

    if (printerReady && device.address == printerAddress) {
      printerReady = false;
      printerAddress = 0;
      printerInterface = 0xFF;
      usbRuntimeError = true;
      setLastError("Printer disconnected");
    }

    usbDeviceChanged = true;
  });

  if (!usb.begin()) {
    fatalUsbHostError = true;
    String e = "USB Host start failed: ";
    e += usb.lastErrorName();
    setLastError(e);
    Serial.println(e);
  } else {
    Serial.println("USB Host started");
    addLog("USB Host started");
  }

  connectSavedWifi();
}

void loop() {
  static uint32_t lastUsbScanMs = 0;

  dns.processNextRequest();
  web.handleClient();

  maintainWifi();

  if (!printerReady &&
      (usbDeviceChanged || millis() - lastUsbScanMs >= 1000)) {
    usbDeviceChanged = false;
    lastUsbScanMs = millis();
    findAndOpenPrinter();
  }

  if (printerReady && !printerStillPresent()) {
    printerReady = false;
    printerAddress = 0;
    printerInterface = 0xFF;
    usbRuntimeError = true;
    setLastError("Printer no longer present");
  }

  if (WiFi.status() == WL_CONNECTED && tcpStarted) {
    handlePrintClient();
  }

  refreshLedMode();
  updateStatusLed();

  delay(1);
}