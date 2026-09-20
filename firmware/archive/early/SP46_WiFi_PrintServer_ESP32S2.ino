/*
  SHOPTIDA SP46 Wi-Fi Print Server
  ESP32-S2 Mini + USB Host

  Windows/macOS -> Wi-Fi -> TCP RAW 9100 -> ESP32-S2 -> USB -> SP46

  USB wiring:
    D19 -> 22R -> USB D-
    D20 -> 22R -> USB D+
    5V  -> USB VBUS
    GND -> USB GND

  Required library: EspUsbHost by tanakamasayuki
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "EspUsbHost.h"

// ===================== WIFI =====================
const char* WIFI_SSID = "TEN_WIFI";
const char* WIFI_PASS = "MAT_KHAU_WIFI";
const char* HOST_NAME = "sp46";
static const uint16_t PRINT_PORT = 9100;

// ===================== USB ======================
EspUsbHost usb;

static bool usbDeviceChanged = false;
static bool printerReady = false;
static uint8_t printerAddress = 0;
static uint8_t printerInterface = 0xFF;
static uint8_t printerProtocol = 0;
static uint16_t printerVid = 0;
static uint16_t printerPid = 0;

// ===================== NETWORK =================
WiFiServer printServer(PRINT_PORT);
WiFiClient printClient;

static uint32_t totalJobs = 0;
static uint64_t totalBytes = 0;
static uint8_t netBuffer[2048];

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
      uint8_t iface   = interfaces[i].number;
      uint8_t proto   = interfaces[i].interfaceProtocol;

      Serial.println();
      Serial.println("========================================");
      Serial.println("USB PRINTER CLASS FOUND");
      Serial.printf("Address   : %u\n", address);
      Serial.printf("VID:PID   : %04X:%04X\n", devices[d].vid, devices[d].pid);
      Serial.printf("Interface : %u\n", iface);
      Serial.printf("Class     : 0x%02X\n", interfaces[i].interfaceClass);
      Serial.printf("Subclass  : 0x%02X\n", interfaces[i].interfaceSubClass);
      Serial.printf("Protocol  : 0x%02X\n", proto);
      Serial.println("========================================");

      if (!usb.vendorOpen(
            address,
            iface,
            ESP_USB_HOST_VENDOR_READ_ON_DEMAND)) {
        Serial.printf("Cannot claim printer interface. USB error: %s\n",
                      usb.lastErrorName());
        return false;
      }

      usb.vendorSetAutoZlp(true, address);

      printerAddress   = address;
      printerInterface = iface;
      printerProtocol  = proto;
      printerVid       = devices[d].vid;
      printerPid       = devices[d].pid;
      printerReady     = true;

      Serial.println("SP46 USB interface opened.");
      Serial.printf("Bulk OUT endpoint : 0x%02X\n", usb.vendorOutEndpoint(address));
      Serial.printf("Bulk IN endpoint  : 0x%02X\n", usb.vendorInEndpoint(address));
      Serial.printf("OUT packet size   : %u bytes\n", usb.vendorOutPacketSize(address));
      Serial.println("READY TO PRINT.");
      Serial.println();
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

bool sendToPrinter(const uint8_t* data, size_t len) {
  if (!printerReady || !data || len == 0) return false;

  if (!usb.vendorWrite(data, len, printerAddress)) {
    Serial.printf("USB write failed: %s\n", usb.lastErrorName());
    printerReady = false;
    printerAddress = 0;
    printerInterface = 0xFF;
    return false;
  }

  totalBytes += len;
  return true;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOST_NAME);
  WiFi.setSleep(false);

  Serial.printf("Connecting Wi-Fi: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 30000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection FAILED.");
    Serial.println("Check WIFI_SSID / WIFI_PASS and restart.");
    return;
  }

  Serial.println("Wi-Fi connected.");
  Serial.print("IP address : ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin(HOST_NAME)) {
    MDNS.addService("pdl-datastream", "tcp", PRINT_PORT);
    Serial.printf("mDNS       : %s.local\n", HOST_NAME);
  }

  printServer.begin();
  Serial.printf("RAW server : TCP port %u\n", PRINT_PORT);
}

void handlePrintClient() {
  if (!printClient || !printClient.connected()) {
    if (printClient) printClient.stop();

    WiFiClient newClient = printServer.available();
    if (newClient) {
      printClient = newClient;
      printClient.setNoDelay(true);
      totalJobs++;

      Serial.println();
      Serial.println("----------------------------------------");
      Serial.printf("Print job #%lu connected from %s\n",
                    (unsigned long)totalJobs,
                    printClient.remoteIP().toString().c_str());

      if (!printerReady) {
        Serial.println("WARNING: printer is not ready.");
      }
    }
    return;
  }

  if (!printerReady) {
    delay(2);
    return;
  }

  int available = printClient.available();
  while (available > 0 && printClient.connected()) {
    size_t wanted = available > (int)sizeof(netBuffer)
                      ? sizeof(netBuffer)
                      : (size_t)available;

    int got = printClient.read(netBuffer, wanted);
    if (got <= 0) break;

    if (!sendToPrinter(netBuffer, (size_t)got)) {
      Serial.println("Print job stopped because USB write failed.");
      printClient.stop();
      return;
    }

    available = printClient.available();
    yield();
  }

  if (!printClient.connected()) {
    Serial.printf("Print job #%lu finished. Total forwarded: %llu bytes\n",
                  (unsigned long)totalJobs,
                  (unsigned long long)totalBytes);
    printClient.stop();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" SHOPTIDA SP46 WIFI PRINT SERVER");
  Serial.println(" ESP32-S2 + USB Host + TCP 9100");
  Serial.println("========================================");
  Serial.println("USB pins: D19 = D-, D20 = D+");
  Serial.println();

  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &device) {
    Serial.printf("USB connected: addr=%u VID=%04X PID=%04X product=\"%s\"\n",
                  device.address,
                  device.vid,
                  device.pid,
                  device.product);
    usbDeviceChanged = true;
  });

  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device) {
    Serial.printf("USB disconnected: addr=%u\n", device.address);

    if (printerReady && device.address == printerAddress) {
      printerReady = false;
      printerAddress = 0;
      printerInterface = 0xFF;
      printerProtocol = 0;
      printerVid = 0;
      printerPid = 0;
      Serial.println("Printer OFFLINE.");
    }

    usbDeviceChanged = true;
  });

  if (!usb.begin()) {
    Serial.printf("USB Host start failed: %s\n", usb.lastErrorName());
  } else {
    Serial.println("USB Host started.");
  }

  connectWiFi();
  Serial.println();
  Serial.println("Waiting for SHOPTIDA SP46...");
}

void loop() {
  static uint32_t lastUsbScan = 0;
  static uint32_t lastWifiRetry = 0;

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiRetry >= 5000) {
      lastWifiRetry = millis();
      Serial.println("Wi-Fi lost. Reconnecting...");
      WiFi.reconnect();
    }
  }

  if (!printerReady &&
      (usbDeviceChanged || millis() - lastUsbScan >= 1000)) {
    usbDeviceChanged = false;
    lastUsbScan = millis();
    findAndOpenPrinter();
  }

  if (printerReady && !printerStillPresent()) {
    printerReady = false;
    printerAddress = 0;
    printerInterface = 0xFF;
    Serial.println("Printer no longer present.");
  }

  if (WiFi.status() == WL_CONNECTED) {
    handlePrintClient();
  }

  delay(1);
}