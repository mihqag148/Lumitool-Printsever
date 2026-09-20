/*
  ESP PRINT SERVER - 3 USB PRINTERS
  Target: ESP32-S2 + self-powered USB 2.0 hub + 3 USB Printer Class printers

  Network mapping:
    USB HUB port A (physical port 1) -> RAW TCP 9101
    USB HUB port B (physical port 2) -> RAW TCP 9102
    USB HUB port C (physical port 3) -> RAW TCP 9103

  PC / macOS / Linux:
    Printer A -> <ESP IP>:9101
    Printer B -> <ESP IP>:9102
    Printer C -> <ESP IP>:9103

  Web:
    AP SSID: ESP print sever XXXX
    Setup IP: 192.168.10.1
    Local:    http://esp-print-sever-xxxx.local

  IMPORTANT FOR ESP32-S2:
    EspUsbHost defaults to only 3 tracked USB devices on S2.
    A hub + 3 printers = 4 USB devices.
    Put build_opt.h in the SAME sketch folder containing exactly:
      -DESP_USB_HOST_MAX_DEVICES=4

  Hardware:
    - ESP32-S2 USB-C -> USB-C OTG -> SELF-POWERED USB 2.0 HUB
    - Hub port 1 -> Printer A
    - Hub port 2 -> Printer B
    - Hub port 3 -> Printer C
    - ESP32-S2 powered separately through VBUS + GND
    - Each printer uses its own normal power adapter

  LED GPIO15:
    solid ON = normal
    slow blink = no home Wi-Fi
    2 quick blinks + pause = USB/printer runtime error
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "EspUsbHost.h"

// ---------------- General ----------------
static const uint8_t LED_PIN = 15;
static const bool LED_ACTIVE_LOW = false;

static const uint32_t PRINT_JOB_IDLE_CLOSE_MS = 2200;
static const uint32_t NO_PRINTER_CLIENT_CLOSE_MS = 3000;

IPAddress AP_IP(192, 168, 10, 1);
IPAddress AP_GW(192, 168, 10, 1);
IPAddress AP_MASK(255, 255, 255, 0);

String macSuffix;
String apSsid;
String hostName;
String localUrl;

// ---------------- Storage ----------------
Preferences prefs;
String savedSsid;
String savedPass;

// ---------------- Network ----------------
WebServer web(80);
DNSServer dns;

WiFiServer serverA(9101);
WiFiServer serverB(9102);
WiFiServer serverC(9103);

bool tcpStarted = false;
bool mdnsStarted = false;
uint32_t lastWifiAttempt = 0;

// ---------------- USB ----------------
EspUsbHost usb;
bool usbDeviceChanged = false;
bool fatalUsbHostError = false;
String globalLastError = "None";

// ---------------- Logging ----------------
static const size_t LOG_MAX_CHARS = 20000;
String systemLog;

String uptimeStamp() {
  uint32_t ms = millis();
  uint32_t sec = ms / 1000UL;
  uint32_t h = sec / 3600UL;
  uint32_t m = (sec % 3600UL) / 60UL;
  uint32_t s = sec % 60UL;
  uint32_t milli = ms % 1000UL;

  char b[28];
  snprintf(b, sizeof(b), "[%03lu:%02lu:%02lu.%03lu] ",
           (unsigned long)h,
           (unsigned long)m,
           (unsigned long)s,
           (unsigned long)milli);
  return String(b);
}

void addLog(const String &msg) {
  String line = uptimeStamp() + msg + "\n";
  systemLog += line;

  if (systemLog.length() > LOG_MAX_CHARS) {
    size_t target = systemLog.length() - (LOG_MAX_CHARS * 3 / 4);
    int cut = systemLog.indexOf('\n', (unsigned int)target);
    if (cut >= 0) systemLog.remove(0, cut + 1);
    else systemLog.remove(0, target);
  }

  Serial.print(line);
}

void setGlobalError(const String &msg) {
  if (globalLastError == msg) return;
  globalLastError = msg;
  if (msg == "None") addLog("Global error cleared");
  else addLog("ERROR: " + msg);
}

// ---------------- Printer slots ----------------
struct PrinterSlot {
  char label;
  uint8_t hubPort;
  uint16_t tcpPort;

  WiFiServer *server;
  WiFiClient client;

  bool ready;
  bool runtimeError;

  uint8_t address;
  uint8_t interfaceNumber;
  uint8_t protocol;
  uint8_t portId;
  uint8_t parentAddress;

  uint16_t vid;
  uint16_t pid;

  String product;
  String manufacturer;
  String lastError;

  uint32_t jobs;
  uint64_t bytes;
  uint32_t currentJobBytes;
  uint32_t lastActivityMs;
  bool currentJobHasData;
};

PrinterSlot printers[3];

uint8_t netBuffer[2048];

void initPrinterSlots() {
  printers[0].label = 'A';
  printers[0].hubPort = 1;
  printers[0].tcpPort = 9101;
  printers[0].server = &serverA;

  printers[1].label = 'B';
  printers[1].hubPort = 2;
  printers[1].tcpPort = 9102;
  printers[1].server = &serverB;

  printers[2].label = 'C';
  printers[2].hubPort = 3;
  printers[2].tcpPort = 9103;
  printers[2].server = &serverC;

  for (int i = 0; i < 3; i++) {
    printers[i].ready = false;
    printers[i].runtimeError = false;
    printers[i].address = 0;
    printers[i].interfaceNumber = 0xFF;
    printers[i].protocol = 0;
    printers[i].portId = 0;
    printers[i].parentAddress = 0;
    printers[i].vid = 0;
    printers[i].pid = 0;
    printers[i].lastError = "None";
    printers[i].jobs = 0;
    printers[i].bytes = 0;
    printers[i].currentJobBytes = 0;
    printers[i].lastActivityMs = 0;
    printers[i].currentJobHasData = false;
  }
}

PrinterSlot* slotByHubPort(uint8_t hubPort) {
  for (int i = 0; i < 3; i++) {
    if (printers[i].hubPort == hubPort) return &printers[i];
  }
  return nullptr;
}

PrinterSlot* slotByAddress(uint8_t address) {
  for (int i = 0; i < 3; i++) {
    if (printers[i].ready && printers[i].address == address) {
      return &printers[i];
    }
  }
  return nullptr;
}

bool anyPrinterRuntimeError() {
  for (int i = 0; i < 3; i++) {
    if (printers[i].runtimeError) return true;
  }
  return false;
}

void clearSlot(PrinterSlot &p, const String &reason, bool markError) {
  if (p.client) p.client.stop();

  if (p.ready || p.address != 0) {
    addLog("Printer " + String(p.label) + " offline: " + reason);
  }

  p.ready = false;
  p.runtimeError = markError;
  p.address = 0;
  p.interfaceNumber = 0xFF;
  p.protocol = 0;
  p.portId = 0;
  p.parentAddress = 0;
  p.vid = 0;
  p.pid = 0;
  p.product = "";
  p.manufacturer = "";
  p.currentJobBytes = 0;
  p.currentJobHasData = false;
  p.lastError = markError ? reason : "None";
}

// ---------------- LED ----------------
enum LedMode {
  LED_NORMAL_ON,
  LED_NO_HOME_WIFI,
  LED_ERROR_PATTERN
};

LedMode ledMode = LED_NORMAL_ON;

void ledWrite(bool on) {
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? !on : on);
}

void refreshLedMode() {
  if (fatalUsbHostError || anyPrinterRuntimeError()) {
    ledMode = LED_ERROR_PATTERN;
  } else if (WiFi.status() != WL_CONNECTED) {
    ledMode = LED_NO_HOME_WIFI;
  } else {
    ledMode = LED_NORMAL_ON;
  }
}

void updateStatusLed() {
  static LedMode previousMode = (LedMode)255;
  static uint8_t phase = 0;
  static uint32_t phaseStarted = 0;

  if (ledMode != previousMode) {
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

  static const uint16_t durations[4] = {120, 120, 120, 1200};
  static const bool states[4] = {true, false, true, false};

  ledWrite(states[phase]);

  if (now - phaseStarted >= durations[phase]) {
    phase = (phase + 1) % 4;
    phaseStarted = now;
  }
}

// ---------------- Identity ----------------
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

// ---------------- USB printer discovery ----------------
bool addressStillPresent(uint8_t address) {
  if (address == 0) return false;
  EspUsbHostDeviceInfo d;
  return usb.getDevice(address, d);
}

bool openPrinterForSlot(const EspUsbHostDeviceInfo &device,
                        const EspUsbHostInterfaceInfo &intf,
                        PrinterSlot &p) {
  if (p.ready && p.address == device.address) return true;

  // If this physical port was previously occupied by another address,
  // forget the stale mapping first.
  if (p.ready && p.address != device.address) {
    clearSlot(p, "USB address changed", false);
  }

  // On-demand mode avoids continuously polling BULK IN.
  // We only need RAW BULK OUT for PC/Mac/Linux printing.
  if (!usb.vendorOpen(device.address,
                      intf.number,
                      ESP_USB_HOST_VENDOR_READ_ON_DEMAND)) {
    String e = "vendorOpen failed: ";
    e += usb.lastErrorName();

    p.runtimeError = true;
    p.lastError = e;
    addLog("Printer " + String(p.label) + " " + e);
    return false;
  }

  usb.vendorSetAutoZlp(true, device.address);

  p.ready = true;
  p.runtimeError = false;
  p.address = device.address;
  p.interfaceNumber = intf.number;
  p.protocol = intf.interfaceProtocol;
  p.portId = device.portId;
  p.parentAddress = device.parentAddress;
  p.vid = device.vid;
  p.pid = device.pid;
  p.product = device.product;
  p.manufacturer = device.manufacturer;
  p.lastError = "None";

  String msg = "Printer ";
  msg += p.label;
  msg += " ready | hub port=";
  msg += String(p.hubPort);
  msg += " portId=0x";
  msg += String(p.portId, HEX);
  msg += " addr=";
  msg += String(p.address);
  msg += " VID:PID=";
  msg += String(p.vid, HEX);
  msg += ":";
  msg += String(p.pid, HEX);
  msg += " iface=";
  msg += String(p.interfaceNumber);
  msg += " proto=";
  msg += String(p.protocol);
  msg += " TCP=";
  msg += String(p.tcpPort);

  addLog(msg);
  return true;
}

void scanUsbPrinters() {
  EspUsbHostDeviceInfo devices[ESP_USB_HOST_MAX_DEVICES];
  size_t count = usb.getDevices(devices, ESP_USB_HOST_MAX_DEVICES);

  for (size_t d = 0; d < count; d++) {
    const EspUsbHostDeviceInfo &dev = devices[d];

    if (dev.isHub) continue;

    // Hub downstream port is lower nibble:
    // 0x11 = hub #1 port 1, 0x12 = hub #1 port 2, ...
    uint8_t physicalPort = dev.portId & 0x0F;

    // Root-connected printer is treated as A for fallback testing.
    if (dev.portId == 0x01) physicalPort = 1;

    PrinterSlot *slot = slotByHubPort(physicalPort);
    if (!slot) continue;

    if (slot->ready && slot->address == dev.address) continue;

    EspUsbHostInterfaceInfo interfaces[ESP_USB_HOST_MAX_INTERFACES];
    size_t interfaceCount =
      usb.getInterfaces(dev.address, interfaces, ESP_USB_HOST_MAX_INTERFACES);

    for (size_t i = 0; i < interfaceCount; i++) {
      if (interfaces[i].interfaceClass != 0x07) continue;

      openPrinterForSlot(dev, interfaces[i], *slot);
      break;
    }
  }
}

bool sendToPrinter(PrinterSlot &p, const uint8_t *data, size_t len) {
  if (!p.ready || p.address == 0 || data == nullptr || len == 0) return false;

  if (!usb.vendorWrite(data, len, p.address)) {
    String e = "USB write failed: ";
    e += usb.lastErrorName();

    p.lastError = e;
    p.runtimeError = true;
    addLog("Printer " + String(p.label) + " " + e);

    p.ready = false;
    p.address = 0;
    return false;
  }

  p.bytes += len;
  return true;
}

// ---------------- Wi-Fi ----------------
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
  WiFi.softAP(apSsid.c_str());

  dns.start(53, "*", AP_IP);

  addLog("Setup AP started: " + apSsid + " @ " + AP_IP.toString());
}

void startPrintServers() {
  if (tcpStarted) return;

  serverA.begin();
  serverB.begin();
  serverC.begin();

  tcpStarted = true;
  addLog("RAW servers started: A=9101 B=9102 C=9103");
}

void startMdnsIfNeeded() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) return;

  if (MDNS.begin(hostName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    addLog("mDNS started: " + localUrl);
  }
}

void connectSavedWifi() {
  if (savedSsid.length() == 0) return;

  WiFi.setHostname(hostName.c_str());
  WiFi.setSleep(false);

  addLog("Connecting home Wi-Fi: " + savedSsid);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  lastWifiAttempt = millis();
}

void maintainWifi() {
  static wl_status_t previous = WL_NO_SHIELD;
  wl_status_t now = WiFi.status();

  if (now != previous) {
    if (now == WL_CONNECTED) {
      addLog("Home Wi-Fi connected: " +
             WiFi.SSID() +
             " IP=" +
             WiFi.localIP().toString());
    } else if (previous == WL_CONNECTED) {
      addLog("Home Wi-Fi disconnected");
      mdnsStarted = false;
    }
    previous = now;
  }

  if (now == WL_CONNECTED) {
    startMdnsIfNeeded();
  } else if (savedSsid.length() > 0 &&
             millis() - lastWifiAttempt >= 10000) {
    lastWifiAttempt = millis();
    addLog("Retrying home Wi-Fi: " + savedSsid);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  }
}

// ---------------- RAW TCP jobs ----------------
void handlePrinterClient(PrinterSlot &p) {
  if (!p.server) return;

  if (!p.client || !p.client.connected()) {
    if (p.client) p.client.stop();

    WiFiClient incoming = p.server->available();
    if (incoming) {
      p.client = incoming;
      p.client.setNoDelay(true);

      p.jobs++;
      p.currentJobBytes = 0;
      p.currentJobHasData = false;
      p.lastActivityMs = millis();

      addLog("Printer " + String(p.label) +
             " job #" + String(p.jobs) +
             " connected from " +
             p.client.remoteIP().toString());
    }
    return;
  }

  if (!p.ready) {
    if (millis() - p.lastActivityMs >= NO_PRINTER_CLIENT_CLOSE_MS) {
      addLog("Printer " + String(p.label) +
             " closing TCP job: printer offline");
      p.client.stop();
    }
    return;
  }

  int available = p.client.available();

  while (available > 0 && p.client.connected()) {
    size_t wanted =
      available > (int)sizeof(netBuffer) ? sizeof(netBuffer) : (size_t)available;

    int got = p.client.read(netBuffer, wanted);
    if (got <= 0) break;

    if (!sendToPrinter(p, netBuffer, (size_t)got)) {
      p.client.stop();
      return;
    }

    p.currentJobBytes += (uint32_t)got;
    p.currentJobHasData = true;
    p.lastActivityMs = millis();

    available = p.client.available();
    yield();
  }

  if (p.currentJobHasData &&
      p.client.connected() &&
      millis() - p.lastActivityMs >= PRINT_JOB_IDLE_CLOSE_MS) {

    addLog("Printer " + String(p.label) +
           " job #" + String(p.jobs) +
           " complete, " + String(p.currentJobBytes) +
           " bytes; closing socket");

    p.client.flush();
    delay(5);
    p.client.stop();

    p.currentJobHasData = false;
    p.currentJobBytes = 0;
  }
}

// ---------------- Web helpers ----------------
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

String slotStatusHtml(const PrinterSlot &p) {
  String s;

  s += "<div class='printer'>";
  s += "<div class='ph'><span class='badge'>PORT ";
  s += p.label;
  s += "</span><b> TCP ";
  s += String(p.tcpPort);
  s += "</b></div>";

  if (p.ready) {
    s += "<div class='online'>ONLINE</div>";
    s += "<div class='small'>USB hub port ";
    s += String(p.hubPort);
    s += " · USB address ";
    s += String(p.address);
    s += " · portId 0x";
    s += String(p.portId, HEX);
    s += "</div>";

    char vp[20];
    snprintf(vp, sizeof(vp), "%04X:%04X", p.vid, p.pid);
    s += "<div class='small'>VID:PID ";
    s += vp;
    s += " · protocol ";
    s += String(p.protocol);
    s += "</div>";

    if (p.product.length()) {
      s += "<div class='small'>Product: ";
      s += htmlEscape(p.product);
      s += "</div>";
    }
  } else {
    s += "<div class='offline'>OFFLINE / EMPTY</div>";
    s += "<div class='small'>Cắm máy in vào cổng ";
    s += p.label;
    s += " của USB hub.</div>";
  }

  s += "<div class='small'>Jobs: ";
  s += String(p.jobs);
  s += " · Bytes: ";
  s += String((unsigned long)p.bytes);
  s += "</div>";

  if (p.lastError != "None") {
    s += "<div class='err'>Last error: ";
    s += htmlEscape(p.lastError);
    s += "</div>";
  }

  s += "</div>";
  return s;
}

String makePage() {
  String p;
  p.reserve(12000);

  p += F(
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'>"
    "<title>ESP Print Server</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;background:#f3f5f7;margin:0;color:#171717}"
    ".w{max-width:820px;margin:auto;padding:16px}"
    ".c{background:#fff;border-radius:14px;padding:16px;margin:12px 0;box-shadow:0 2px 12px #0001}"
    "h1{font-size:23px;margin:3px 0 4px}h2{font-size:17px;margin:0 0 12px}"
    ".muted,.small{color:#666;font-size:13px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
    ".kv{padding:10px;background:#f6f7f9;border-radius:9px;word-break:break-word}"
    ".printer{border:1px solid #e2e5e9;border-radius:12px;padding:13px;margin:10px 0}"
    ".ph{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}"
    ".badge{background:#111;color:#fff;border-radius:8px;padding:5px 9px;font-weight:700}"
    ".online{color:#087f23;font-weight:800;margin:5px 0}.offline{color:#b71c1c;font-weight:800;margin:5px 0}"
    ".err{color:#b71c1c;font-size:12px;margin-top:5px}"
    "input,button{width:100%;box-sizing:border-box;padding:11px;margin:6px 0;border-radius:9px;border:1px solid #ccc;font-size:15px}"
    "button{border:0;background:#111;color:#fff;font-weight:700}.red{background:#b71c1c}"
    "a{color:#0756c7;text-decoration:none}"
    "</style></head><body><div class='w'>"
  );

  p += "<h1>ESP Print Server " + macSuffix + "</h1>";
  p += "<div class='muted'>Firmware V6 · 3 USB printers · RAW 9101/9102/9103</div>";

  p += "<div class='c'><h2>Trạng thái mạng</h2><div class='grid'>";
  p += "<div class='kv'>Wi-Fi nhà<br><b>";
  p += WiFi.status() == WL_CONNECTED ? "Đã kết nối" : "Chưa kết nối";
  p += "</b></div>";

  p += "<div class='kv'>IP mạng nhà<br><b>";
  p += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "-";
  p += "</b></div>";

  p += "<div class='kv'>AP setup<br><b>";
  p += htmlEscape(apSsid);
  p += "</b><br>192.168.10.1</div>";

  p += "<div class='kv'>Local<br><b>";
  p += htmlEscape(localUrl);
  p += "</b></div>";
  p += "</div></div>";

  p += "<div class='c'><h2>Máy in USB</h2>";
  p += slotStatusHtml(printers[0]);
  p += slotStatusHtml(printers[1]);
  p += slotStatusHtml(printers[2]);
  p += "</div>";

  p += F(
    "<div class='c'><h2>Cài Wi-Fi nhà</h2>"
    "<form method='POST' action='/wifi'>"
    "<input name='ssid' placeholder='Tên Wi-Fi' value='"
  );
  p += htmlEscape(savedSsid);
  p += F(
    "'><input name='pass' type='password' placeholder='Mật khẩu Wi-Fi'>"
    "<button type='submit'>Lưu & kết nối</button></form>"
    "<button onclick=\"location.href='/scan'\">Quét Wi-Fi</button>"
    "</div>"
  );

  p += F(
    "<div class='c'><h2>Nhật ký</h2>"
    "<button onclick=\"location.href='/log'\">Xem log</button>"
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
  String ssid = web.arg("ssid");
  String pass = web.arg("pass");
  ssid.trim();

  if (ssid.length() == 0) {
    web.send(400, "text/plain", "SSID empty");
    return;
  }

  saveWifiSettings(ssid, pass);
  addLog("Wi-Fi settings saved: " + ssid);

  mdnsStarted = false;
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  lastWifiAttempt = millis();

  web.send(200, "text/html; charset=utf-8",
           "<html><body style='font-family:Arial;padding:24px'>"
           "<h2>Đã lưu Wi-Fi</h2><p>Thiết bị đang kết nối.</p>"
           "<a href='/'>Quay lại</a></body></html>");
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
      p += "<p><b>";
      p += htmlEscape(WiFi.SSID(i));
      p += "</b> · ";
      p += String(WiFi.RSSI(i));
      p += " dBm</p>";
    }
  }

  p += "<p><a href='/'>Quay lại</a></p></body></html>";
  WiFi.scanDelete();
  web.send(200, "text/html; charset=utf-8", p);
}

String makeLogPage() {
  String p;
  p.reserve(systemLog.length() + 3500);

  p += F(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'><meta http-equiv='refresh' content='4'>"
    "<title>System Log</title><style>"
    "body{font-family:Arial;background:#f3f5f7;margin:0}.w{max-width:950px;margin:auto;padding:16px}"
    "a,button{display:inline-block;padding:10px 13px;margin:4px;border:0;border-radius:8px;background:#111;color:#fff;text-decoration:none}"
    "pre{background:#101216;color:#dbe4ec;padding:14px;border-radius:10px;white-space:pre-wrap;word-break:break-word;min-height:400px;font:12px/1.45 Consolas,monospace}"
    ".red{background:#b71c1c}</style></head><body><div class='w'>"
    "<h2>ESP Print Server Log</h2>"
    "<a href='/'>Trang chính</a><a href='/log.txt'>Tải log .txt</a>"
    "<form style='display:inline' method='POST' action='/log/clear'><button class='red'>Xóa log</button></form>"
    "<pre>"
  );

  p += systemLog.length() ? htmlEscape(systemLog) : "(Chưa có log)";
  p += "</pre></div></body></html>";
  return p;
}

void handleLogPage() {
  web.send(200, "text/html; charset=utf-8", makeLogPage());
}

void handleLogDownload() {
  String filename = "ESP_Print_Server_" + macSuffix + "_log.txt";
  web.sendHeader("Content-Disposition",
                 "attachment; filename=\"" + filename + "\"");
  web.sendHeader("Cache-Control", "no-store");

  String h;
  h += "ESP Print Server V6 - 3 printers\n";
  h += "AP: " + apSsid + "\n";
  h += "Local: " + localUrl + "\n";
  h += "Printer A: TCP 9101\n";
  h += "Printer B: TCP 9102\n";
  h += "Printer C: TCP 9103\n";
  h += "Global error: " + globalLastError + "\n";
  h += "----------------------------------------\n";

  web.send(200, "text/plain; charset=utf-8", h + systemLog);
}

void handleLogClear() {
  systemLog = "";
  addLog("Log cleared");
  web.sendHeader("Location", "/log", true);
  web.send(303, "text/plain", "");
}

void handleForget() {
  addLog("Forget Wi-Fi requested");
  forgetWifiSettings();
  mdnsStarted = false;

  web.send(200, "text/html; charset=utf-8",
           "<html><body style='font-family:Arial;padding:24px'>"
           "<h2>Đã quên Wi-Fi</h2>"
           "<p>Kết nối lại AP và mở 192.168.10.1.</p>"
           "</body></html>");
}

void handleRestart() {
  addLog("Restart requested");
  web.send(200, "text/plain", "Restarting...");
  delay(350);
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
  web.on("/log", HTTP_GET, handleLogPage);
  web.on("/log.txt", HTTP_GET, handleLogDownload);
  web.on("/log/clear", HTTP_POST, handleLogClear);
  web.on("/forget", HTTP_POST, handleForget);
  web.on("/restart", HTTP_POST, handleRestart);

  web.on("/generate_204", HTTP_ANY, captiveRedirect);
  web.on("/gen_204", HTTP_ANY, captiveRedirect);
  web.on("/hotspot-detect.html", HTTP_ANY, captiveRedirect);
  web.on("/connecttest.txt", HTTP_ANY, captiveRedirect);
  web.on("/ncsi.txt", HTTP_ANY, captiveRedirect);
  web.onNotFound(captiveRedirect);

  web.begin();
  addLog("Web server started");
}

// ---------------- Setup / Loop ----------------
void setup() {
  pinMode(LED_PIN, OUTPUT);
  ledWrite(true);

  Serial.begin(115200);
  delay(700);

  systemLog.reserve(LOG_MAX_CHARS + 512);

  initPrinterSlots();
  buildIdentity();

  addLog("Boot: ESP Print Server V6 - 3 printers");
  addLog("Identity: " + apSsid + " / " + localUrl);

  loadWifiSettings();

  startAccessPoint();
  startPrintServers();
  startWeb();

  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &device) {
    String msg = "USB connected addr=";
    msg += String(device.address);
    msg += " portId=0x";
    msg += String(device.portId, HEX);
    msg += " parent=";
    msg += String(device.parentAddress);
    msg += " VID:PID=";
    msg += String(device.vid, HEX);
    msg += ":";
    msg += String(device.pid, HEX);
    if (device.isHub) msg += " HUB";

    addLog(msg);
    usbDeviceChanged = true;
  });

  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device) {
    addLog("USB disconnected addr=" + String(device.address) +
           " portId=0x" + String(device.portId, HEX));

    PrinterSlot *p = slotByAddress(device.address);
    if (p) {
      clearSlot(*p, "USB disconnected", true);
    }

    usbDeviceChanged = true;
  });

  if (!usb.begin()) {
    fatalUsbHostError = true;
    String e = "USB Host start failed: ";
    e += usb.lastErrorName();
    setGlobalError(e);
  } else {
    addLog("USB Host started");
  }

  connectSavedWifi();
}

void loop() {
  static uint32_t lastUsbScan = 0;

  dns.processNextRequest();
  web.handleClient();

  maintainWifi();

  if (usbDeviceChanged || millis() - lastUsbScan >= 1000) {
    usbDeviceChanged = false;
    lastUsbScan = millis();
    scanUsbPrinters();
  }

  // Clean stale printer mappings.
  for (int i = 0; i < 3; i++) {
    if (printers[i].ready &&
        !addressStillPresent(printers[i].address)) {
      clearSlot(printers[i], "Device no longer present", true);
    }
  }

  // Each printer has its own independent RAW queue/socket.
  handlePrinterClient(printers[0]);
  handlePrinterClient(printers[1]);
  handlePrinterClient(printers[2]);

  refreshLedMode();
  updateStatusLed();

  delay(1);
}