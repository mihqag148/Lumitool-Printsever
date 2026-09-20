/*
  ESP PRINT SERVER V7.2 - COMPACT UI + 3 PRINTER QUEUES
  Target: ESP32-S2 + USB 2.0 hub + USB Printer Class printers

  NETWORK
    Printer A -> RAW TCP 9101
    Printer B -> RAW TCP 9102
    Printer C -> RAW TCP 9103

  USB
    Direct printer (no hub) -> Printer A / TCP 9101
    Hub physical port 1     -> Printer A / TCP 9101
    Hub physical port 2     -> Printer B / TCP 9102
    Hub physical port 3     -> Printer C / TCP 9103

  WEB
    Setup AP : ESP Print Server XXXX
    Setup IP : 192.168.10.1
    Local    : http://esp-print-server-xxxx.local
    /jobs    : live per-printer queue + job history
    /log     : live system log
    /log.txt : download log

  WINDOWS RAW FEEDBACK
    - The TCP connection stays open while a job waits in this server's queue.
    - The TCP connection is closed only after the job data has been transferred
      successfully to the target USB printer.
    - Therefore Windows normally keeps the job as Printing until this server
      actually finishes forwarding it.
    - RAW 9100 cannot expose this server's exact queue position/job list back
      to standard Windows Spooler. The web /jobs page is the authoritative queue.
    - Full Windows "paper out/offline" feedback is driver/printer dependent.
      V7 reads USB Printer Class GET_PORT_STATUS for the web UI when supported.

  JOB QUEUE
    - Up to 3 simultaneous jobs per printer: 1 active + 2 waiting/receiving.
    - Incoming jobs are buffered in PSRAM, then printed FIFO.
    - A, B and C are independent: all three printers can print concurrently.
    - Global queue RAM budget is limited to avoid crashes.

  V7.1 COMPILE FIX
    - Adds forward declarations for JobState / PrintJob / PrinterSlot before
      the first function body. This prevents Arduino's auto-prototype generator
      from producing prototypes before those custom types are known.

  IMPORTANT
    Keep the SAME build_opt.h from V6 in this sketch folder:
      -DESP_USB_HOST_MAX_DEVICES=4

    Arduino Tools:
      USB CDC On Boot = Disabled

  HARDWARE
    ESP32-S2 USB-C -> USB-C OTG -> USB 2.0 hub
    ESP32-S2 power -> VBUS + GND
    GPIO15 -> onboard status LED

  LED GPIO15
    solid ON              = normal
    slow blink            = no home Wi-Fi
    2 quick blinks/pause  = USB/printer runtime error
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "EspUsbHost.h"
#include "esp_heap_caps.h"

// ============================================================
// General
// ============================================================
static const uint8_t LED_PIN = 15;
static const bool LED_ACTIVE_LOW = false;

static const uint32_t JOB_RX_IDLE_MS = 2200;
static const uint32_t EMPTY_CLIENT_TIMEOUT_MS = 5000;
static const uint32_t PRINTER_STATUS_POLL_MS = 1500;

static const uint8_t MAX_JOBS_PER_PRINTER = 3;  // active + 2 waiting
static const size_t USB_SEND_CHUNK = 4096;

// Queue memory limits.
// Board from earlier upload: ESP32-S2FNR2 with 2 MB embedded PSRAM.
static const size_t PSRAM_JOB_MAX_BYTES = 512UL * 1024UL;
static const size_t PSRAM_TOTAL_QUEUE_BYTES = 1400UL * 1024UL;
static const size_t DRAM_JOB_MAX_BYTES = 64UL * 1024UL;
static const size_t DRAM_TOTAL_QUEUE_BYTES = 160UL * 1024UL;

IPAddress AP_IP(192, 168, 10, 1);
IPAddress AP_GW(192, 168, 10, 1);
IPAddress AP_MASK(255, 255, 255, 0);

String macSuffix;
String apSsid;
String hostName;
String localUrl;

// ============================================================
// Storage / network
// ============================================================
Preferences prefs;
String savedSsid;
String savedPass;

WebServer web(80);
DNSServer dns;

WiFiServer serverA(9101);
WiFiServer serverB(9102);
WiFiServer serverC(9103);

bool tcpStarted = false;
bool mdnsStarted = false;
uint32_t lastWifiAttempt = 0;

// ============================================================
// USB
// ============================================================
EspUsbHost usb;
bool usbDeviceChanged = false;
bool fatalUsbHostError = false;
String globalLastError = "None";

// ============================================================
// Logging
// ============================================================
static const size_t LOG_MAX_CHARS = 22000;
String systemLog;

// ============================================================
// Arduino .ino preprocessor compatibility
// ============================================================
// Arduino auto-generates function prototypes before the first function body.
// These forward declarations make the custom types visible to those prototypes.
enum JobState : uint8_t;
struct PrintJob;
struct JobHistory;
struct PrinterSlot;

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

// ============================================================
// Jobs / queue
// ============================================================
enum JobState : uint8_t {
  JOB_FREE = 0,
  JOB_RECEIVING,
  JOB_QUEUED,
  JOB_PRINTING
};

struct PrintJob {
  JobState state;
  uint32_t id;
  WiFiClient client;
  IPAddress sourceIp;

  uint8_t *data;
  size_t length;
  size_t capacity;
  size_t sent;
  bool inPsram;

  uint32_t createdMs;
  uint32_t firstByteMs;
  uint32_t lastActivityMs;
};

struct JobHistory {
  uint32_t id;
  char printer;
  IPAddress sourceIp;
  uint32_t bytes;
  uint32_t durationMs;
  uint32_t finishedMs;
  bool success;
  String result;
};

static const uint8_t HISTORY_COUNT = 18;
JobHistory historyItems[HISTORY_COUNT];
uint8_t historyWriteIndex = 0;
uint32_t nextJobId = 1;

size_t totalQueueAllocated = 0;

// ============================================================
// Printer slots
// ============================================================
struct PrinterSlot {
  char label;
  uint8_t hubPort;
  uint16_t tcpPort;
  WiFiServer *server;

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

  // USB Printer Class GET_PORT_STATUS
  bool portStatusSupported;
  uint8_t portStatusByte;
  bool paperEmpty;
  bool selected;
  bool noError;
  uint32_t lastStatusPollMs;

  PrintJob jobs[MAX_JOBS_PER_PRINTER];

  uint32_t jobsAccepted;
  uint32_t jobsDone;
  uint32_t jobsFailed;
  uint64_t bytesPrinted;
};

PrinterSlot printers[3];

uint8_t netBuffer[2048];

// ============================================================
// Small helpers
// ============================================================
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

String formatBytes(uint64_t bytes) {
  if (bytes >= 1024ULL * 1024ULL) {
    return String((double)bytes / (1024.0 * 1024.0), 1) + " MB";
  }
  if (bytes >= 1024ULL) {
    return String((double)bytes / 1024.0, 1) + " KB";
  }
  return String((unsigned long)bytes) + " B";
}

String jobStateText(JobState s) {
  switch (s) {
    case JOB_RECEIVING: return "ĐANG NHẬN";
    case JOB_QUEUED:    return "ĐANG CHỜ";
    case JOB_PRINTING:  return "ĐANG IN";
    default:            return "-";
  }
}

size_t runtimeJobLimit() {
  return psramFound() ? PSRAM_JOB_MAX_BYTES : DRAM_JOB_MAX_BYTES;
}

size_t runtimeTotalQueueLimit() {
  return psramFound() ? PSRAM_TOTAL_QUEUE_BYTES : DRAM_TOTAL_QUEUE_BYTES;
}

void freeJobBuffer(PrintJob &j) {
  if (j.data) {
    if (j.inPsram) heap_caps_free(j.data);
    else free(j.data);

    if (totalQueueAllocated >= j.capacity) totalQueueAllocated -= j.capacity;
    else totalQueueAllocated = 0;
  }

  j.data = nullptr;
  j.capacity = 0;
  j.length = 0;
  j.sent = 0;
  j.inPsram = false;
}

void resetJob(PrintJob &j) {
  if (j.client) j.client.stop();
  freeJobBuffer(j);

  j.state = JOB_FREE;
  j.id = 0;
  j.sourceIp = IPAddress();
  j.createdMs = 0;
  j.firstByteMs = 0;
  j.lastActivityMs = 0;
}

bool ensureJobCapacity(PrintJob &j, size_t required) {
  if (required <= j.capacity) return true;

  size_t jobLimit = runtimeJobLimit();
  size_t totalLimit = runtimeTotalQueueLimit();

  if (required > jobLimit) return false;

  size_t newCap = j.capacity ? j.capacity : 4096;
  while (newCap < required) {
    size_t next = newCap * 2;
    if (next > jobLimit) next = jobLimit;
    if (next == newCap) break;
    newCap = next;
  }

  if (newCap < required) return false;

  size_t projected =
    totalQueueAllocated - j.capacity + newCap;

  if (projected > totalLimit) return false;

  void *newPtr = nullptr;

  if (psramFound()) {
    if (j.data) {
      newPtr = heap_caps_realloc(
        j.data,
        newCap,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
      );
    } else {
      newPtr = heap_caps_malloc(
        newCap,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
      );
    }
  } else {
    if (j.data) newPtr = realloc(j.data, newCap);
    else newPtr = malloc(newCap);
  }

  if (!newPtr) return false;

  totalQueueAllocated = projected;
  j.data = (uint8_t *)newPtr;
  j.capacity = newCap;
  j.inPsram = psramFound();

  return true;
}

void addHistory(const PrintJob &j,
                const PrinterSlot &p,
                bool success,
                const String &result) {
  JobHistory &h = historyItems[historyWriteIndex];

  h.id = j.id;
  h.printer = p.label;
  h.sourceIp = j.sourceIp;
  h.bytes = (uint32_t)j.length;
  h.durationMs = millis() - j.createdMs;
  h.finishedMs = millis();
  h.success = success;
  h.result = result;

  historyWriteIndex = (historyWriteIndex + 1) % HISTORY_COUNT;
}

void finishJob(PrinterSlot &p,
               PrintJob &j,
               bool success,
               const String &result) {
  addHistory(j, p, success, result);

  if (success) {
    p.jobsDone++;
    p.bytesPrinted += j.length;

    addLog(
      "Printer " + String(p.label) +
      " job #" + String(j.id) +
      " DONE " + String((unsigned long)j.length) +
      " bytes from " + j.sourceIp.toString()
    );
  } else {
    p.jobsFailed++;

    addLog(
      "Printer " + String(p.label) +
      " job #" + String(j.id) +
      " ERROR: " + result
    );
  }

  // This close is the completion signal Windows RAW spooler sees.
  if (j.client) {
    j.client.flush();
    delay(2);
    j.client.stop();
  }

  resetJob(j);
}

PrintJob* findFreeJob(PrinterSlot &p) {
  for (uint8_t i = 0; i < MAX_JOBS_PER_PRINTER; i++) {
    if (p.jobs[i].state == JOB_FREE) return &p.jobs[i];
  }
  return nullptr;
}

PrintJob* findOldestQueuedJob(PrinterSlot &p) {
  PrintJob *best = nullptr;

  for (uint8_t i = 0; i < MAX_JOBS_PER_PRINTER; i++) {
    PrintJob &j = p.jobs[i];

    if (j.state != JOB_QUEUED) continue;

    if (!best || j.id < best->id) best = &j;
  }

  return best;
}

PrintJob* findPrintingJob(PrinterSlot &p) {
  for (uint8_t i = 0; i < MAX_JOBS_PER_PRINTER; i++) {
    if (p.jobs[i].state == JOB_PRINTING) return &p.jobs[i];
  }
  return nullptr;
}

uint8_t countJobs(const PrinterSlot &p, JobState state) {
  uint8_t n = 0;

  for (uint8_t i = 0; i < MAX_JOBS_PER_PRINTER; i++) {
    if (p.jobs[i].state == state) n++;
  }

  return n;
}

uint8_t countAllJobs(const PrinterSlot &p) {
  uint8_t n = 0;

  for (uint8_t i = 0; i < MAX_JOBS_PER_PRINTER; i++) {
    if (p.jobs[i].state != JOB_FREE) n++;
  }

  return n;
}

// ============================================================
// Init printers / identity
// ============================================================
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

  for (uint8_t i = 0; i < 3; i++) {
    PrinterSlot &p = printers[i];

    p.ready = false;
    p.runtimeError = false;

    p.address = 0;
    p.interfaceNumber = 0xFF;
    p.protocol = 0;
    p.portId = 0;
    p.parentAddress = 0;

    p.vid = 0;
    p.pid = 0;
    p.product = "";
    p.manufacturer = "";
    p.lastError = "None";

    p.portStatusSupported = false;
    p.portStatusByte = 0;
    p.paperEmpty = false;
    p.selected = true;
    p.noError = true;
    p.lastStatusPollMs = 0;

    p.jobsAccepted = 0;
    p.jobsDone = 0;
    p.jobsFailed = 0;
    p.bytesPrinted = 0;

    for (uint8_t j = 0; j < MAX_JOBS_PER_PRINTER; j++) {
      p.jobs[j].data = nullptr;
      p.jobs[j].capacity = 0;
      p.jobs[j].inPsram = false;
      p.jobs[j].state = JOB_FREE;
      resetJob(p.jobs[j]);
    }
  }

  for (uint8_t i = 0; i < HISTORY_COUNT; i++) {
    historyItems[i].id = 0;
  }
}

void buildIdentity() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t last16 = (uint16_t)(mac & 0xFFFF);

  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", last16);
  macSuffix = suffix;

  apSsid = "ESP Print Server " + macSuffix;

  String lower = macSuffix;
  lower.toLowerCase();

  hostName = "esp-print-server-" + lower;
  localUrl = "http://" + hostName + ".local";
}

// ============================================================
// LED
// ============================================================
enum LedMode {
  LED_NORMAL_ON,
  LED_NO_HOME_WIFI,
  LED_ERROR_PATTERN
};

LedMode ledMode = LED_NORMAL_ON;

void ledWrite(bool on) {
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? !on : on);
}

bool anyPrinterRuntimeError() {
  for (uint8_t i = 0; i < 3; i++) {
    if (printers[i].runtimeError) return true;
  }
  return false;
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

// ============================================================
// USB printer management
// ============================================================
PrinterSlot* slotByHubPort(uint8_t hubPort) {
  for (uint8_t i = 0; i < 3; i++) {
    if (printers[i].hubPort == hubPort) return &printers[i];
  }
  return nullptr;
}

PrinterSlot* slotByAddress(uint8_t address) {
  for (uint8_t i = 0; i < 3; i++) {
    if (printers[i].address == address && printers[i].address != 0) {
      return &printers[i];
    }
  }
  return nullptr;
}

bool addressStillPresent(uint8_t address) {
  if (address == 0) return false;

  EspUsbHostDeviceInfo d;
  return usb.getDevice(address, d);
}

void setPrinterOffline(PrinterSlot &p,
                       const String &reason,
                       bool runtimeError) {
  if (p.ready) {
    addLog(
      "Printer " + String(p.label) +
      " OFFLINE: " + reason
    );
  }

  p.ready = false;
  p.runtimeError = runtimeError;
  p.address = 0;
  p.interfaceNumber = 0xFF;
  p.protocol = 0;
  p.portId = 0;
  p.parentAddress = 0;
  p.vid = 0;
  p.pid = 0;
  p.product = "";
  p.manufacturer = "";
  p.portStatusSupported = false;
  p.lastError = runtimeError ? reason : "None";

  // A partially transmitted job cannot be resumed safely.
  PrintJob *printing = findPrintingJob(p);
  if (printing) {
    finishJob(p, *printing, false, reason);
  }

  // Queued jobs remain in RAM and will continue after this port reconnects.
}

bool queryPrinterPortStatus(PrinterSlot &p) {
  if (!p.ready || p.address == 0 || p.interfaceNumber == 0xFF) {
    return false;
  }

  uint8_t status = 0;
  size_t actual = 0;

  // USB Printer Class GET_PORT_STATUS:
  // bmRequestType = 0xA1, bRequest = 1, wIndex = interface.
  bool ok = usb.vendorControlTransfer(
    0xA1,
    1,
    0,
    p.interfaceNumber,
    &status,
    1,
    &actual,
    p.address
  );

  if (!ok || actual < 1) {
    p.portStatusSupported = false;
    return false;
  }

  bool oldPaper = p.paperEmpty;
  bool oldSelected = p.selected;
  bool oldNoError = p.noError;

  p.portStatusSupported = true;
  p.portStatusByte = status;
  p.paperEmpty = (status & 0x20) != 0;
  p.selected = (status & 0x10) != 0;
  p.noError = (status & 0x08) != 0;

  if (oldPaper != p.paperEmpty ||
      oldSelected != p.selected ||
      oldNoError != p.noError) {

    String msg = "Printer ";
    msg += p.label;
    msg += " status: ";

    if (p.paperEmpty) msg += "PAPER_EMPTY ";
    if (!p.selected) msg += "NOT_SELECTED ";
    if (!p.noError) msg += "ERROR ";
    if (!p.paperEmpty && p.selected && p.noError) msg += "READY";

    addLog(msg);
  }

  return true;
}

bool printerCanStartJob(const PrinterSlot &p) {
  if (!p.ready) return false;

  if (!p.portStatusSupported) {
    // Some printers return only benign/unsupported status.
    // USB connectivity itself is enough to allow printing.
    return true;
  }

  if (p.paperEmpty) return false;
  if (!p.selected) return false;
  if (!p.noError) return false;

  return true;
}

bool openPrinterForSlot(const EspUsbHostDeviceInfo &device,
                        const EspUsbHostInterfaceInfo &intf,
                        PrinterSlot &p) {
  if (p.ready && p.address == device.address) return true;

  if (p.ready && p.address != device.address) {
    setPrinterOffline(p, "USB address changed", false);
  }

  if (!usb.vendorOpen(
        device.address,
        intf.number,
        ESP_USB_HOST_VENDOR_READ_ON_DEMAND)) {

    String e = "vendorOpen failed: ";
    e += usb.lastErrorName();

    p.runtimeError = true;
    p.lastError = e;

    addLog(
      "Printer " + String(p.label) +
      " " + e
    );

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

  p.lastStatusPollMs = 0;
  queryPrinterPortStatus(p);

  String msg =
    "Printer " + String(p.label) +
    " READY hubPort=" + String(p.hubPort) +
    " portId=0x" + String(p.portId, HEX) +
    " addr=" + String(p.address) +
    " VID:PID=" + String(p.vid, HEX) +
    ":" + String(p.pid, HEX) +
    " iface=" + String(p.interfaceNumber) +
    " proto=" + String(p.protocol) +
    " TCP=" + String(p.tcpPort);

  addLog(msg);
  return true;
}

void scanUsbPrinters() {
  EspUsbHostDeviceInfo devices[ESP_USB_HOST_MAX_DEVICES];
  size_t count = usb.getDevices(
    devices,
    ESP_USB_HOST_MAX_DEVICES
  );

  for (size_t d = 0; d < count; d++) {
    const EspUsbHostDeviceInfo &dev = devices[d];

    if (dev.isHub) continue;

    uint8_t physicalPort = dev.portId & 0x0F;

    // Direct device, no hub -> A.
    if (dev.portId == 0x01) physicalPort = 1;

    PrinterSlot *slot = slotByHubPort(physicalPort);
    if (!slot) continue;

    if (slot->ready && slot->address == dev.address) continue;

    EspUsbHostInterfaceInfo interfaces[ESP_USB_HOST_MAX_INTERFACES];

    size_t interfaceCount = usb.getInterfaces(
      dev.address,
      interfaces,
      ESP_USB_HOST_MAX_INTERFACES
    );

    for (size_t i = 0; i < interfaceCount; i++) {
      if (interfaces[i].interfaceClass != 0x07) continue;

      openPrinterForSlot(
        dev,
        interfaces[i],
        *slot
      );

      break;
    }
  }
}

bool sendPrinterChunk(PrinterSlot &p,
                      PrintJob &j,
                      const uint8_t *data,
                      size_t len) {
  if (!p.ready || !data || len == 0) return false;

  if (!usb.vendorWrite(data, len, p.address)) {
    String e = "USB write failed: ";
    e += usb.lastErrorName();

    p.runtimeError = true;
    p.lastError = e;

    addLog(
      "Printer " + String(p.label) +
      " " + e
    );

    return false;
  }

  return true;
}

void pollPrinterStatuses() {
  uint32_t now = millis();

  for (uint8_t i = 0; i < 3; i++) {
    PrinterSlot &p = printers[i];

    if (!p.ready) continue;

    if (now - p.lastStatusPollMs >= PRINTER_STATUS_POLL_MS) {
      p.lastStatusPollMs = now;
      queryPrinterPortStatus(p);
    }
  }
}

// ============================================================
// Wi-Fi
// ============================================================
void loadWifiSettings() {
  prefs.begin("wifi", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
}

void saveWifiSettings(const String &ssid,
                      const String &pass) {
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

  addLog(
    "Setup AP started: " +
    apSsid +
    " @ " +
    AP_IP.toString()
  );
}

void startPrintServers() {
  if (tcpStarted) return;

  serverA.begin();
  serverB.begin();
  serverC.begin();

  tcpStarted = true;

  addLog(
    "RAW print servers started: "
    "A=9101 B=9102 C=9103"
  );
}

void startMdnsIfNeeded() {
  if (mdnsStarted ||
      WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (MDNS.begin(hostName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;

    addLog(
      "mDNS started: " +
      localUrl
    );
  }
}

void connectSavedWifi() {
  if (savedSsid.length() == 0) return;

  WiFi.setHostname(hostName.c_str());
  WiFi.setSleep(false);

  addLog(
    "Connecting home Wi-Fi: " +
    savedSsid
  );

  WiFi.begin(
    savedSsid.c_str(),
    savedPass.c_str()
  );

  lastWifiAttempt = millis();
}

void maintainWifi() {
  static wl_status_t previous = WL_NO_SHIELD;
  wl_status_t now = WiFi.status();

  if (now != previous) {
    if (now == WL_CONNECTED) {
      addLog(
        "Home Wi-Fi connected: " +
        WiFi.SSID() +
        " IP=" +
        WiFi.localIP().toString()
      );
    } else if (previous == WL_CONNECTED) {
      addLog("Home Wi-Fi disconnected");
      mdnsStarted = false;
    }

    previous = now;
  }

  if (now == WL_CONNECTED) {
    startMdnsIfNeeded();
  } else if (
    savedSsid.length() > 0 &&
    millis() - lastWifiAttempt >= 10000) {

    lastWifiAttempt = millis();

    addLog(
      "Retrying home Wi-Fi: " +
      savedSsid
    );

    WiFi.begin(
      savedSsid.c_str(),
      savedPass.c_str()
    );
  }
}

// ============================================================
// Queue input from PCs
// ============================================================
void acceptNewClient(PrinterSlot &p) {
  if (!p.server) return;

  WiFiClient incoming = p.server->available();
  if (!incoming) return;

  PrintJob *j = findFreeJob(p);

  if (!j) {
    addLog(
      "Printer " + String(p.label) +
      " queue FULL; rejected " +
      incoming.remoteIP().toString()
    );

    incoming.stop();
    return;
  }

  resetJob(*j);

  j->state = JOB_RECEIVING;
  j->id = nextJobId++;
  j->client = incoming;
  j->client.setNoDelay(true);
  j->sourceIp = incoming.remoteIP();
  j->createdMs = millis();
  j->lastActivityMs = millis();

  p.jobsAccepted++;

  addLog(
    "Printer " + String(p.label) +
    " job #" + String(j->id) +
    " accepted from " +
    j->sourceIp.toString()
  );
}

void receiveJobData(PrinterSlot &p,
                    PrintJob &j) {
  if (j.state != JOB_RECEIVING) return;

  int available = j.client.available();

  while (available > 0) {
    size_t wanted =
      available > (int)sizeof(netBuffer)
        ? sizeof(netBuffer)
        : (size_t)available;

    int got = j.client.read(
      netBuffer,
      wanted
    );

    if (got <= 0) break;

    size_t required =
      j.length + (size_t)got;

    if (!ensureJobCapacity(j, required)) {
      String e =
        "Queue memory/full job limit; max job=" +
        formatBytes(runtimeJobLimit());

      addLog(
        "Printer " + String(p.label) +
        " job #" + String(j.id) +
        " ERROR " + e
      );

      finishJob(
        p,
        j,
        false,
        e
      );

      return;
    }

    memcpy(
      j.data + j.length,
      netBuffer,
      got
    );

    if (j.length == 0) {
      j.firstByteMs = millis();
    }

    j.length += got;
    j.lastActivityMs = millis();

    available = j.client.available();

    yield();
  }

  // Sender closed after sending all data.
  if (!j.client.connected()) {
    if (j.length > 0) {
      j.state = JOB_QUEUED;

      addLog(
        "Printer " + String(p.label) +
        " job #" + String(j.id) +
        " queued after client close (" +
        String((unsigned long)j.length) +
        " bytes)"
      );
    } else {
      resetJob(j);
    }

    return;
  }

  // RAW drivers commonly keep the connection open.
  // Idle time marks end-of-job while we keep the socket open
  // until the physical USB transfer completes.
  if (j.length > 0 &&
      millis() - j.lastActivityMs >= JOB_RX_IDLE_MS) {

    j.state = JOB_QUEUED;

    addLog(
      "Printer " + String(p.label) +
      " job #" + String(j.id) +
      " QUEUED " +
      String((unsigned long)j.length) +
      " bytes"
    );

    return;
  }

  if (j.length == 0 &&
      millis() - j.createdMs >= EMPTY_CLIENT_TIMEOUT_MS) {

    addLog(
      "Printer " + String(p.label) +
      " empty connection timeout"
    );

    resetJob(j);
  }
}

void serviceReceivingJobs(PrinterSlot &p) {
  for (uint8_t i = 0;
       i < MAX_JOBS_PER_PRINTER;
       i++) {

    if (p.jobs[i].state == JOB_RECEIVING) {
      receiveJobData(
        p,
        p.jobs[i]
      );
    }
  }
}

// ============================================================
// Queue output to printers
// ============================================================
void processPrinterQueue(PrinterSlot &p) {
  PrintJob *printing = findPrintingJob(p);

  if (!printing) {
    PrintJob *next = findOldestQueuedJob(p);

    if (!next) return;

    if (!printerCanStartJob(p)) {
      // Keep it queued until printer comes back / paper restored.
      return;
    }

    next->state = JOB_PRINTING;
    next->sent = 0;

    addLog(
      "Printer " + String(p.label) +
      " job #" + String(next->id) +
      " PRINTING"
    );

    printing = next;
  }

  if (!p.ready) {
    finishJob(
      p,
      *printing,
      false,
      "Printer disconnected during print"
    );

    return;
  }

  if (p.portStatusSupported &&
      !printerCanStartJob(p)) {

    // A status problem appeared after printing began.
    // Do not send more bytes until it clears.
    return;
  }

  size_t remaining =
    printing->length - printing->sent;

  if (remaining == 0) {
    queryPrinterPortStatus(p);

    finishJob(
      p,
      *printing,
      true,
      "Transferred to USB printer"
    );

    return;
  }

  size_t chunk =
    remaining > USB_SEND_CHUNK
      ? USB_SEND_CHUNK
      : remaining;

  bool ok = sendPrinterChunk(
    p,
    *printing,
    printing->data + printing->sent,
    chunk
  );

  if (!ok) {
    finishJob(
      p,
      *printing,
      false,
      p.lastError
    );

    return;
  }

  printing->sent += chunk;

  // One chunk per printer per loop keeps A/B/C fair and concurrent.
}

// ============================================================
// Web UI
// ============================================================
String printerHealthText(const PrinterSlot &p) {
  if (!p.ready) return "OFFLINE";

  if (!p.portStatusSupported) {
    return "ONLINE";
  }

  if (p.paperEmpty) return "HẾT GIẤY";
  if (!p.selected) return "CHƯA SẴN SÀNG";
  if (!p.noError) return "LỖI";

  return "SẴN SÀNG";
}

String printerHealthClass(const PrinterSlot &p) {
  if (!p.ready) return "bad";

  if (p.portStatusSupported &&
      (p.paperEmpty ||
       !p.selected ||
       !p.noError)) {
    return "warn";
  }

  return "ok";
}

String renderPrinterCard(
  const PrinterSlot &p
) {
  String s;
  s.reserve(1200);

  s += "<div class='printer'>";
  s += "<div class='ph'><span class='badge'>PORT ";
  s += p.label;
  s += "</span><span class='tcp'>";
  s += String(p.tcpPort);
  s += "</span></div>";

  s += "<div class='";
  s += printerHealthClass(p);
  s += "' style='margin:7px 0 5px'>";
  s += printerHealthText(p);
  s += "</div>";

  s += "<div class='small'>Q ";
  s += String(countAllJobs(p));
  s += "/";
  s += String(MAX_JOBS_PER_PRINTER);
  s += " · Chờ ";
  s += String(countJobs(p, JOB_QUEUED));
  s += " · In ";
  s += String(countJobs(p, JOB_PRINTING));
  s += "</div>";

  if (p.ready) {
    s += "<div class='small'>USB ";
    s += String(p.address);
    s += " · Done ";
    s += String(p.jobsDone);
    s += " · ";
    s += formatBytes(p.bytesPrinted);
    s += "</div>";
  } else {
    s += "<div class='small'>Hub ";
    s += String(p.hubPort);
    s += " · chưa có máy</div>";
  }

  if (p.lastError != "None") {
    s += "<div class='bad small'>";
    s += htmlEscape(p.lastError);
    s += "</div>";
  }

  s += "</div>";
  return s;
}

String makePage() {
  String p;
  p.reserve(9500);

  p += F(
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'><title>ESP Print Server</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{font-family:Arial,sans-serif;background:#f3f5f7;margin:0;color:#171717}"
    ".w{max-width:860px;margin:auto;padding:10px}"
    ".top{display:flex;justify-content:space-between;align-items:center;gap:8px;margin:2px 0 7px}"
    "h1{font-size:19px;margin:0}.ver{font-size:10px;color:#777}"
    ".net{background:#fff;border-radius:10px;padding:9px 11px;box-shadow:0 1px 7px #0001;margin-bottom:8px}"
    ".netline{display:flex;flex-wrap:wrap;gap:5px 13px;font-size:12px;align-items:center}"
    ".ok{color:#087f23;font-weight:800}.bad{color:#b71c1c;font-weight:800}.warn{color:#b26a00;font-weight:800}"
    ".cards{display:grid;grid-template-columns:repeat(3,1fr);gap:7px;margin-bottom:8px}"
    ".printer{background:#fff;border-radius:10px;padding:10px;box-shadow:0 1px 7px #0001;min-width:0}"
    ".ph{display:flex;justify-content:space-between;gap:5px;align-items:center}"
    ".badge{background:#111;color:#fff;border-radius:6px;padding:4px 7px;font-size:11px;font-weight:700}"
    ".tcp{font-size:11px;font-weight:700}.small{color:#666;font-size:11px;line-height:1.4}"
    ".actions{display:grid;grid-template-columns:repeat(5,1fr);gap:6px;margin-bottom:8px}"
    "button,.btn{display:block;width:100%;padding:8px 6px;border:0;border-radius:8px;background:#111;color:#fff;text-decoration:none;text-align:center;font-weight:700;font-size:12px;cursor:pointer}"
    ".ghost{background:#fff;color:#111;border:1px solid #ddd}.red{background:#b71c1c}"
    "details{background:#fff;border-radius:10px;padding:0 11px;margin-bottom:7px;box-shadow:0 1px 7px #0001}"
    "summary{padding:10px 0;cursor:pointer;font-weight:700;font-size:12px}"
    ".inside{padding:0 0 10px}.row{display:grid;grid-template-columns:1fr 1fr;gap:6px}"
    "input{width:100%;padding:9px;border:1px solid #ccc;border-radius:8px;font-size:13px}"
    ".foot{font-size:10px;color:#777;text-align:center;padding:3px 0 6px}"
    "@media(max-width:700px){.cards{grid-template-columns:1fr}.actions{grid-template-columns:repeat(2,1fr)}.row{grid-template-columns:1fr}.top{align-items:flex-start}}"
    "</style></head><body><div class='w'>"
  );

  p += "<div class='top'><div><h1>ESP Print Server ";
  p += macSuffix;
  p += "</h1><div class='ver'>V7.2 · 3 Printer Queue</div></div><div class='";
  p += (WiFi.status() == WL_CONNECTED) ? "ok" : "bad";
  p += "'>";
  p += (WiFi.status() == WL_CONNECTED) ? "Wi-Fi ✓" : "Wi-Fi ×";
  p += "</div></div>";

  p += "<div class='net'><div class='netline'>";
  p += "<span><b>IP</b> ";
  p += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "-";
  p += "</span><span><b>Setup</b> 192.168.10.1</span><span><b>Local</b> ";
  p += htmlEscape(localUrl);
  p += "</span></div></div>";

  p += "<div class='cards'>";
  p += renderPrinterCard(printers[0]);
  p += renderPrinterCard(printers[1]);
  p += renderPrinterCard(printers[2]);
  p += "</div>";

  p += F(
    "<div class='actions'>"
    "<a class='btn' href='/jobs'>Jobs</a>"
    "<a class='btn ghost' href='/log'>Log</a>"
    "<a class='btn ghost' href='/log.txt'>Tải log</a>"
    "<a class='btn ghost' href='/scan'>Quét Wi-Fi</a>"
    "<form method='POST' action='/restart' style='margin:0'><button type='submit'>Restart</button></form>"
    "</div>"
  );

  p += F(
    "<details><summary>Wi-Fi</summary><div class='inside'>"
    "<form method='POST' action='/wifi'>"
    "<div class='row'><input name='ssid' placeholder='Tên Wi-Fi' value='"
  );
  p += htmlEscape(savedSsid);
  p += F(
    "'><input name='pass' type='password' placeholder='Mật khẩu Wi-Fi'></div>"
    "<div class='row'><button type='submit'>Lưu & kết nối</button>"
    "<button class='red' formaction='/forget' formmethod='POST'>Quên Wi-Fi</button></div>"
    "</form></div></details>"
  );

  p += "<div class='foot'>PSRAM ";
  p += psramFound() ? "ON" : "OFF";
  p += " · Queue ";
  p += formatBytes(totalQueueAllocated);
  p += "/";
  p += formatBytes(runtimeTotalQueueLimit());
  p += " · Job max ";
  p += formatBytes(runtimeJobLimit());
  p += "</div></div></body></html>";

  return p;
}

String renderLiveJobRow(const PrinterSlot &p,
                        const PrintJob &j) {
  String s;

  s += "<tr><td>#";
  s += String(j.id);
  s += "</td><td>";
  s += p.label;
  s += "</td><td>";
  s += j.sourceIp.toString();
  s += "</td><td><b>";
  s += jobStateText(j.state);
  s += "</b></td><td>";
  s += formatBytes(j.length);

  if (j.state == JOB_PRINTING &&
      j.length > 0) {

    s += "<br><span class='muted'>";
    s += String(
      (unsigned int)(
        (j.sent * 100UL) /
        j.length
      )
    );
    s += "%</span>";
  }

  s += "</td><td>";

  if (j.state == JOB_QUEUED &&
      !printerCanStartJob(p)) {

    if (!p.ready) s += "Chờ máy in";
    else if (p.paperEmpty) s += "Chờ giấy";
    else s += "Chờ máy sẵn sàng";
  } else {
    s += "-";
  }

  s += "</td></tr>";

  return s;
}

String makeJobsPage() {
  String p;
  p.reserve(15000);

  p += F(
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='2'>"
    "<title>Print Jobs</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;background:#f3f5f7;margin:0;color:#171717}"
    ".w{max-width:1000px;margin:auto;padding:16px}"
    ".c{background:#fff;border-radius:14px;padding:16px;margin:12px 0;box-shadow:0 2px 12px #0001}"
    "h1{font-size:22px;margin:3px 0 6px}h2{font-size:17px}"
    ".muted{color:#666;font-size:12px}"
    "table{width:100%;border-collapse:collapse;font-size:13px}"
    "th,td{text-align:left;padding:9px;border-bottom:1px solid #eee;vertical-align:top}"
    "th{background:#f5f6f8}"
    ".ok{color:#087f23}.bad{color:#b71c1c}"
    "a{display:inline-block;background:#111;color:#fff;padding:10px 13px;border-radius:8px;text-decoration:none;margin:3px}"
    "</style></head><body><div class='w'>"
  );

  p += "<h1>Hàng đợi in · ";
  p += macSuffix;
  p += "</h1>";

  p += "<div class='muted'>Tự cập nhật mỗi 2 giây. RAW 9100 không truyền tên tài liệu Windows một cách chuẩn, nên server hiển thị Job ID + IP máy gửi.</div>";

  p += "<p><a href='/'>Trang chính</a><a href='/log'>Log</a></p>";

  for (uint8_t pi = 0; pi < 3; pi++) {
    PrinterSlot &pr = printers[pi];

    p += "<div class='c'><h2>Printer ";
    p += pr.label;
    p += " · TCP ";
    p += String(pr.tcpPort);
    p += " · ";
    p += printerHealthText(pr);
    p += "</h2>";

    p += "<table><tr><th>Job</th><th>Port</th><th>PC</th><th>Trạng thái</th><th>Dữ liệu</th><th>Ghi chú</th></tr>";

    bool any = false;

    for (uint8_t ji = 0;
         ji < MAX_JOBS_PER_PRINTER;
         ji++) {

      if (pr.jobs[ji].state != JOB_FREE) {
        any = true;
        p += renderLiveJobRow(
          pr,
          pr.jobs[ji]
        );
      }
    }

    if (!any) {
      p += "<tr><td colspan='6' class='muted'>Không có job đang chờ.</td></tr>";
    }

    p += "</table></div>";
  }

  p += "<div class='c'><h2>Lịch sử job gần nhất</h2>";
  p += "<table><tr><th>Job</th><th>Printer</th><th>PC</th><th>Bytes</th><th>Kết quả</th><th>Thời gian</th></tr>";

  bool anyHistory = false;

  for (int n = 0; n < HISTORY_COUNT; n++) {
    int idx =
      (int)historyWriteIndex - 1 - n;

    while (idx < 0) idx += HISTORY_COUNT;

    JobHistory &h =
      historyItems[idx];

    if (h.id == 0) continue;

    anyHistory = true;

    p += "<tr><td>#";
    p += String(h.id);
    p += "</td><td>";
    p += h.printer;
    p += "</td><td>";
    p += h.sourceIp.toString();
    p += "</td><td>";
    p += formatBytes(h.bytes);
    p += "</td><td class='";
    p += h.success ? "ok" : "bad";
    p += "'>";
    p += h.success ? "HOÀN TẤT" : "LỖI";
    p += "<br><span class='muted'>";
    p += htmlEscape(h.result);
    p += "</span></td><td>";
    p += String(h.durationMs / 1000.0, 1);
    p += " s</td></tr>";
  }

  if (!anyHistory) {
    p += "<tr><td colspan='6' class='muted'>Chưa có lịch sử.</td></tr>";
  }

  p += "</table></div></div></body></html>";

  return p;
}

void handleRoot() {
  web.send(
    200,
    "text/html; charset=utf-8",
    makePage()
  );
}

void handleJobs() {
  web.send(
    200,
    "text/html; charset=utf-8",
    makeJobsPage()
  );
}

void handleWifiSave() {
  String ssid = web.arg("ssid");
  String pass = web.arg("pass");

  ssid.trim();

  if (ssid.length() == 0) {
    web.send(
      400,
      "text/plain",
      "SSID empty"
    );

    return;
  }

  saveWifiSettings(
    ssid,
    pass
  );

  addLog(
    "Wi-Fi settings saved: " +
    ssid
  );

  mdnsStarted = false;

  WiFi.disconnect(
    false,
    false
  );

  delay(100);

  WiFi.begin(
    savedSsid.c_str(),
    savedPass.c_str()
  );

  lastWifiAttempt = millis();

  web.send(
    200,
    "text/html; charset=utf-8",
    "<html><body style='font-family:Arial;padding:24px'>"
    "<h2>Đã lưu Wi-Fi</h2>"
    "<p>Thiết bị đang kết nối.</p>"
    "<a href='/'>Quay lại</a>"
    "</body></html>"
  );
}

void handleScan() {
  int n = WiFi.scanNetworks(
    false,
    true
  );

  String p =
    "<html><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:Arial;padding:18px'>"
    "<h2>Wi-Fi tìm thấy</h2>";

  if (n <= 0) {
    p += "<p>Không tìm thấy mạng.</p>";
  } else {
    for (int i = 0; i < n; i++) {
      p += "<p><b>";
      p += htmlEscape(
        WiFi.SSID(i)
      );
      p += "</b> · ";
      p += String(
        WiFi.RSSI(i)
      );
      p += " dBm</p>";
    }
  }

  p += "<p><a href='/'>Quay lại</a></p></body></html>";

  WiFi.scanDelete();

  web.send(
    200,
    "text/html; charset=utf-8",
    p
  );
}

String makeLogPage() {
  String p;
  p.reserve(
    systemLog.length() + 3500
  );

  p += F(
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='4'>"
    "<title>System Log</title>"
    "<style>"
    "body{font-family:Arial;background:#f3f5f7;margin:0}"
    ".w{max-width:950px;margin:auto;padding:16px}"
    "a,button{display:inline-block;padding:10px 13px;margin:4px;border:0;border-radius:8px;background:#111;color:#fff;text-decoration:none}"
    "pre{background:#101216;color:#dbe4ec;padding:14px;border-radius:10px;white-space:pre-wrap;word-break:break-word;min-height:400px;font:12px/1.45 Consolas,monospace}"
    ".red{background:#b71c1c}"
    "</style></head><body><div class='w'>"
    "<h2>ESP Print Server Log</h2>"
    "<a href='/'>Trang chính</a>"
    "<a href='/jobs'>Jobs</a>"
    "<a href='/log.txt'>Tải log .txt</a>"
    "<form style='display:inline' method='POST' action='/log/clear'>"
    "<button class='red'>Xóa log</button>"
    "</form>"
    "<pre>"
  );

  p += systemLog.length()
         ? htmlEscape(systemLog)
         : "(Chưa có log)";

  p += "</pre></div></body></html>";

  return p;
}

void handleLogPage() {
  web.send(
    200,
    "text/html; charset=utf-8",
    makeLogPage()
  );
}

void handleLogDownload() {
  String filename =
    "ESP_Print_Server_" +
    macSuffix +
    "_log.txt";

  web.sendHeader(
    "Content-Disposition",
    "attachment; filename=\"" +
    filename +
    "\""
  );

  web.sendHeader(
    "Cache-Control",
    "no-store"
  );

  String h;

  h += "ESP Print Server V7.2\n";
  h += "AP: " + apSsid + "\n";
  h += "Local: " + localUrl + "\n";
  h += "Printer A: TCP 9101\n";
  h += "Printer B: TCP 9102\n";
  h += "Printer C: TCP 9103\n";
  h += "PSRAM: ";
  h += psramFound()
         ? "YES\n"
         : "NO\n";
  h += "Queue allocated: ";
  h += formatBytes(
    totalQueueAllocated
  );
  h += "\nGlobal error: ";
  h += globalLastError;
  h += "\n----------------------------------------\n";

  web.send(
    200,
    "text/plain; charset=utf-8",
    h + systemLog
  );
}

void handleLogClear() {
  systemLog = "";
  addLog("Log cleared");

  web.sendHeader(
    "Location",
    "/log",
    true
  );

  web.send(
    303,
    "text/plain",
    ""
  );
}

void handleForget() {
  addLog("Forget Wi-Fi requested");

  forgetWifiSettings();
  mdnsStarted = false;

  web.send(
    200,
    "text/html; charset=utf-8",
    "<html><body style='font-family:Arial;padding:24px'>"
    "<h2>Đã quên Wi-Fi</h2>"
    "<p>Kết nối lại ESP Print Server và mở 192.168.10.1.</p>"
    "</body></html>"
  );
}

void handleRestart() {
  addLog("Restart requested");

  web.send(
    200,
    "text/plain",
    "Restarting..."
  );

  delay(350);
  ESP.restart();
}

void captiveRedirect() {
  web.sendHeader(
    "Location",
    "http://192.168.10.1/",
    true
  );

  web.send(
    302,
    "text/plain",
    ""
  );
}

void startWeb() {
  web.on(
    "/",
    HTTP_GET,
    handleRoot
  );

  web.on(
    "/jobs",
    HTTP_GET,
    handleJobs
  );

  web.on(
    "/wifi",
    HTTP_POST,
    handleWifiSave
  );

  web.on(
    "/scan",
    HTTP_GET,
    handleScan
  );

  web.on(
    "/log",
    HTTP_GET,
    handleLogPage
  );

  web.on(
    "/log.txt",
    HTTP_GET,
    handleLogDownload
  );

  web.on(
    "/log/clear",
    HTTP_POST,
    handleLogClear
  );

  web.on(
    "/forget",
    HTTP_POST,
    handleForget
  );

  web.on(
    "/restart",
    HTTP_POST,
    handleRestart
  );

  web.on(
    "/generate_204",
    HTTP_ANY,
    captiveRedirect
  );

  web.on(
    "/gen_204",
    HTTP_ANY,
    captiveRedirect
  );

  web.on(
    "/hotspot-detect.html",
    HTTP_ANY,
    captiveRedirect
  );

  web.on(
    "/connecttest.txt",
    HTTP_ANY,
    captiveRedirect
  );

  web.on(
    "/ncsi.txt",
    HTTP_ANY,
    captiveRedirect
  );

  web.onNotFound(
    captiveRedirect
  );

  web.begin();

  addLog("Web server started");
}

// ============================================================
// Setup / loop
// ============================================================
void setup() {
  pinMode(
    LED_PIN,
    OUTPUT
  );

  ledWrite(true);

  Serial.begin(115200);
  delay(700);

  systemLog.reserve(
    LOG_MAX_CHARS + 512
  );

  initPrinterSlots();
  buildIdentity();

  addLog(
    "Boot: ESP Print Server V7.2"
  );

  addLog(
    "Identity: " +
    apSsid +
    " / " +
    localUrl
  );

  if (psramFound()) {
    addLog(
      "PSRAM detected; queue limit " +
      formatBytes(
        runtimeTotalQueueLimit()
      )
    );
  } else {
    addLog(
      "WARNING: PSRAM not detected; queue reduced to " +
      formatBytes(
        runtimeTotalQueueLimit()
      )
    );
  }

  loadWifiSettings();

  startAccessPoint();
  startPrintServers();
  startWeb();

  usb.onDeviceConnected(
    [](const EspUsbHostDeviceInfo &device) {
      String msg =
        "USB connected addr=" +
        String(device.address) +
        " portId=0x" +
        String(device.portId, HEX) +
        " parent=" +
        String(device.parentAddress) +
        " VID:PID=" +
        String(device.vid, HEX) +
        ":" +
        String(device.pid, HEX);

      if (device.isHub) {
        msg += " HUB";
      }

      addLog(msg);
      usbDeviceChanged = true;
    }
  );

  usb.onDeviceDisconnected(
    [](const EspUsbHostDeviceInfo &device) {
      addLog(
        "USB disconnected addr=" +
        String(device.address) +
        " portId=0x" +
        String(device.portId, HEX)
      );

      PrinterSlot *p =
        slotByAddress(
          device.address
        );

      if (p) {
        setPrinterOffline(
          *p,
          "USB disconnected",
          true
        );
      }

      usbDeviceChanged = true;
    }
  );

  if (!usb.begin()) {
    fatalUsbHostError = true;

    String e =
      "USB Host start failed: ";

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

  if (
    usbDeviceChanged ||
    millis() - lastUsbScan >= 1000) {

    usbDeviceChanged = false;
    lastUsbScan = millis();

    scanUsbPrinters();
  }

  // Check for vanished devices even if callback was missed.
  for (uint8_t i = 0; i < 3; i++) {
    PrinterSlot &p = printers[i];

    if (
      p.ready &&
      !addressStillPresent(p.address)) {

      setPrinterOffline(
        p,
        "Device no longer present",
        true
      );
    }
  }

  pollPrinterStatuses();

  // Accept multiple PCs and keep independent queues.
  for (uint8_t i = 0; i < 3; i++) {
    acceptNewClient(
      printers[i]
    );

    serviceReceivingJobs(
      printers[i]
    );
  }

  // A/B/C each send one USB chunk per loop, so they progress concurrently.
  processPrinterQueue(printers[0]);
  processPrinterQueue(printers[1]);
  processPrinterQueue(printers[2]);

  refreshLedMode();
  updateStatusLed();

  delay(1);
}