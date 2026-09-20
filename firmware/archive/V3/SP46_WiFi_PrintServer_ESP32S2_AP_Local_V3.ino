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
  lastError = msg;
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

      if (!usb.vendorOpen(address, iface, ESP_USB_HOST_VENDOR_READ_ON_DEMAND)) {
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
}

void startMdnsIfNeeded() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) return;

  if (MDNS.begin(hostName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("pdl-datastream", "tcp", PRINT_PORT);
    mdnsStarted = true;
    Serial.printf("Local: %s\n", localUrl.c_str());
  }
}

void connectSavedWifi() {
  if (savedSsid.length() == 0) return;

  WiFi.setHostname(hostName.c_str());
  WiFi.setSleep(false);

  Serial.printf("Connecting home Wi-Fi: %s\n", savedSsid.c_str());
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  lastWifiAttempt = millis();
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    startMdnsIfNeeded();

    if (!tcpStarted) {
      printServer.begin();
      tcpStarted = true;
      Serial.printf("TCP RAW print server: %u\n", PRINT_PORT);
    }
    return;
  }

  if (savedSsid.length() > 0 && millis() - lastWifiAttempt >= 10000) {
    lastWifiAttempt = millis();
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

  p += "<h1>ESP Print Server " + macSuffix + "</h1>";

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

  p += "<div class='kv'>TCP RAW<br><b>9100</b></div>";

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
  web.send(200, "text/plain", "Restarting...");
  delay(400);
  ESP.restart();
}

void captiveRedirect() {
  web.sendHeader("Location", "http://192.168.10.1/", true);
  web.send(302, "text/plain", "");
}

void startWeb() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/wifi", HTTP_POST, handleWifiSave);
  web.on("/scan", HTTP_GET, handleScan);
  web.on("/forget", HTTP_POST, handleForget);
  web.on("/restart", HTTP_POST, handleRestart);

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
  if (!printClient || !printClient.connected()) {
    if (printClient) printClient.stop();

    WiFiClient c = printServer.available();
    if (c) {
      printClient = c;
      printClient.setNoDelay(true);
      totalJobs++;
      Serial.printf("Print job #%lu from %s\n",
                    (unsigned long)totalJobs,
                    printClient.remoteIP().toString().c_str());
    }
    return;
  }

  if (!printerReady) {
    delay(1);
    return;
  }

  int available = printClient.available();

  while (available > 0 && printClient.connected()) {
    size_t wanted = (available > (int)sizeof(netBuffer))
                      ? sizeof(netBuffer)
                      : (size_t)available;

    int got = printClient.read(netBuffer, wanted);
    if (got <= 0) break;

    if (!sendToPrinter(netBuffer, (size_t)got)) {
      printClient.stop();
      return;
    }

    available = printClient.available();
    yield();
  }
}

// ========================= SETUP / LOOP =========================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  ledWrite(true);

  Serial.begin(115200);
  delay(1000);

  buildIdentity();

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
    usbDeviceChanged = true;
  });

  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device) {
    Serial.printf("USB disconnected: addr=%u\n", device.address);

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