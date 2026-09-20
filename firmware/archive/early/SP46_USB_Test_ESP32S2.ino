#include <Arduino.h>
#include "usb/usb_host.h"
#include "esp_intr_alloc.h"
#include "esp_err.h"

// SP46 USB detector for ESP32-S2
// Arduino-ESP32 3.3.x
// USB native pins on ESP32-S2: GPIO19 = D-, GPIO20 = D+
//
// Wiring:
// ESP32-S2 GPIO19 -> USB D-
// ESP32-S2 GPIO20 -> USB D+
// 5V              -> USB VBUS
// GND             -> USB GND
//
// Open Serial Monitor at 115200 baud.
// Plug the SHOPTIDA SP46 USB cable into the USB-A host socket.
// This sketch only detects and prints USB descriptors.
// It does NOT print yet.

static usb_host_client_handle_t s_client = nullptr;

// Workaround for Arduino-ESP32 3.3.11 USB Host enumeration regression.
// Also harmless on versions where the callback works normally.
static bool enum_filter_cb(const usb_device_desc_t *desc,
                           uint8_t *bConfigurationValue) {
  if (bConfigurationValue && *bConfigurationValue == 0) {
    *bConfigurationValue = 1;
  }

  Serial.printf(
    "\n[USB] Enumerating VID=%04X PID=%04X, config=%u\n",
    desc->idVendor,
    desc->idProduct,
    bConfigurationValue ? *bConfigurationValue : 0
  );

  return true;
}

static void printDeviceStrings(usb_device_handle_t dev_hdl) {
  usb_device_info_t info = {};
  if (usb_host_device_info(dev_hdl, &info) != ESP_OK) {
    return;
  }

  Serial.printf("[USB] Address: %u\n", info.dev_addr);
  Serial.printf("[USB] Active configuration: %u\n", info.bConfigurationValue);

  if (info.str_desc_manufacturer) {
    Serial.print("[USB] Manufacturer: ");
    usb_print_string_descriptor(info.str_desc_manufacturer);
  }

  if (info.str_desc_product) {
    Serial.print("[USB] Product: ");
    usb_print_string_descriptor(info.str_desc_product);
  }

  if (info.str_desc_serial_num) {
    Serial.print("[USB] Serial: ");
    usb_print_string_descriptor(info.str_desc_serial_num);
  }
}

static void inspectDevice(uint8_t address) {
  usb_device_handle_t dev_hdl = nullptr;

  esp_err_t err = usb_host_device_open(s_client, address, &dev_hdl);
  if (err != ESP_OK) {
    Serial.printf("[USB] Cannot open device: %s\n", esp_err_to_name(err));
    return;
  }

  const usb_device_desc_t *dev_desc = nullptr;
  err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);

  if (err != ESP_OK || dev_desc == nullptr) {
    Serial.printf("[USB] Cannot read device descriptor: %s\n", esp_err_to_name(err));
    usb_host_device_close(s_client, dev_hdl);
    return;
  }

  Serial.println();
  Serial.println("================================================");
  Serial.println("       SHOPTIDA SP46 - USB DEVICE DETECTED");
  Serial.println("================================================");
  Serial.printf("VID             : 0x%04X\n", dev_desc->idVendor);
  Serial.printf("PID             : 0x%04X\n", dev_desc->idProduct);
  Serial.printf("Device Class    : 0x%02X\n", dev_desc->bDeviceClass);
  Serial.printf("Device SubClass : 0x%02X\n", dev_desc->bDeviceSubClass);
  Serial.printf("Device Protocol : 0x%02X\n", dev_desc->bDeviceProtocol);
  Serial.printf("Configurations  : %u\n", dev_desc->bNumConfigurations);

  printDeviceStrings(dev_hdl);

  const usb_config_desc_t *cfg = nullptr;
  err = usb_host_get_active_config_descriptor(dev_hdl, &cfg);

  if (err != ESP_OK || cfg == nullptr) {
    Serial.printf("[USB] Cannot read config descriptor: %s\n", esp_err_to_name(err));
    usb_host_device_close(s_client, dev_hdl);
    return;
  }

  Serial.println();
  Serial.println("----- FULL USB DESCRIPTOR -----");
  usb_print_config_descriptor(cfg, nullptr);

  bool printerClassFound = false;
  bool bulkOutFound = false;
  uint8_t printerInterface = 0xFF;
  uint8_t bulkOutEndpoint = 0;
  uint16_t bulkOutMps = 0;

  const usb_standard_desc_t *desc =
    reinterpret_cast<const usb_standard_desc_t *>(cfg);

  int offset = 0;
  bool insidePrinterInterface = false;

  while (true) {
    desc = usb_parse_next_descriptor(desc, cfg->wTotalLength, &offset);
    if (desc == nullptr) break;

    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t *intf =
        reinterpret_cast<const usb_intf_desc_t *>(desc);

      Serial.println();
      Serial.printf(
        "[INTERFACE] num=%u alt=%u class=0x%02X subclass=0x%02X protocol=0x%02X endpoints=%u\n",
        intf->bInterfaceNumber,
        intf->bAlternateSetting,
        intf->bInterfaceClass,
        intf->bInterfaceSubClass,
        intf->bInterfaceProtocol,
        intf->bNumEndpoints
      );

      insidePrinterInterface = (intf->bInterfaceClass == 0x07);

      if (insidePrinterInterface) {
        printerClassFound = true;
        printerInterface = intf->bInterfaceNumber;

        Serial.println("*** USB PRINTER CLASS 0x07 FOUND ***");
      }
    }
    else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const usb_ep_desc_t *ep =
        reinterpret_cast<const usb_ep_desc_t *>(desc);

      const uint8_t transferType = ep->bmAttributes & 0x03;
      const bool directionIn = (ep->bEndpointAddress & 0x80) != 0;

      Serial.printf(
        "[ENDPOINT] 0x%02X  %s  type=%u  MPS=%u\n",
        ep->bEndpointAddress,
        directionIn ? "IN " : "OUT",
        transferType,
        USB_EP_DESC_GET_MPS(ep)
      );

      // Bulk OUT endpoint inside a Printer Class interface
      if (insidePrinterInterface &&
          !directionIn &&
          transferType == USB_BM_ATTRIBUTES_XFER_BULK) {
        bulkOutFound = true;
        bulkOutEndpoint = ep->bEndpointAddress;
        bulkOutMps = USB_EP_DESC_GET_MPS(ep);
      }
    }
  }

  Serial.println();
  Serial.println("================================================");
  Serial.println("                    RESULT");
  Serial.println("================================================");

  if (printerClassFound) {
    Serial.println("USB Printer Class (0x07): YES");
    Serial.printf("Printer interface         : %u\n", printerInterface);

    if (bulkOutFound) {
      Serial.println("Bulk OUT endpoint          : YES");
      Serial.printf("Bulk OUT address           : 0x%02X\n", bulkOutEndpoint);
      Serial.printf("Bulk OUT max packet        : %u bytes\n", bulkOutMps);
      Serial.println();
      Serial.println(">>> GOOD: suitable for the next TCP 9100 -> USB print-server test.");
    } else {
      Serial.println("Bulk OUT endpoint          : NOT FOUND");
      Serial.println();
      Serial.println(">>> Send this Serial output back for checking.");
    }
  } else {
    Serial.println("USB Printer Class (0x07): NO");
    Serial.println();
    Serial.println(">>> Send this Serial output back. The SP46 may use a vendor-specific interface.");
  }

  Serial.println("================================================");
  Serial.println();

  usb_host_device_close(s_client, dev_hdl);
}

static void clientEventCallback(const usb_host_client_event_msg_t *msg,
                                void *arg) {
  switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      Serial.printf("\n[USB] New device, address=%u\n", msg->new_dev.address);
      inspectDevice(msg->new_dev.address);
      break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      Serial.println("\n[USB] Device disconnected.");
      break;

    default:
      break;
  }
}

static void usbLibraryTask(void *arg) {
  while (true) {
    uint32_t eventFlags = 0;
    esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);

    if (err != ESP_OK) {
      Serial.printf("[USB] Library event error: %s\n", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (eventFlags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

static void usbClientTask(void *arg) {
  usb_host_client_config_t clientCfg = {};
  clientCfg.is_synchronous = false;
  clientCfg.max_num_event_msg = 8;
  clientCfg.async.client_event_callback = clientEventCallback;
  clientCfg.async.callback_arg = nullptr;

  esp_err_t err = usb_host_client_register(&clientCfg, &s_client);

  if (err != ESP_OK) {
    Serial.printf("[USB] Client register failed: %s\n", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }

  Serial.println("[USB] Client ready. Plug in SHOPTIDA SP46.");

  while (true) {
    err = usb_host_client_handle_events(s_client, portMAX_DELAY);

    if (err != ESP_OK) {
      Serial.printf("[USB] Client event error: %s\n", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println();
  Serial.println("==============================================");
  Serial.println(" ESP32-S2 USB HOST TEST - SHOPTIDA SP46");
  Serial.println(" GPIO19 = D- | GPIO20 = D+ | USB VBUS = 5V");
  Serial.println("==============================================");

  usb_host_config_t hostCfg = {};
  hostCfg.skip_phy_setup = false;
  hostCfg.root_port_unpowered = false;
  hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  hostCfg.enum_filter_cb = enum_filter_cb;

  esp_err_t err = usb_host_install(&hostCfg);

  if (err != ESP_OK) {
    Serial.printf("[USB] usb_host_install failed: %s\n", esp_err_to_name(err));
    Serial.println("STOP.");
    return;
  }

  Serial.println("[USB] Host installed.");

  xTaskCreate(
    usbLibraryTask,
    "usb_lib",
    4096,
    nullptr,
    3,
    nullptr
  );

  xTaskCreate(
    usbClientTask,
    "usb_client",
    6144,
    nullptr,
    4,
    nullptr
  );
}

void loop() {
  delay(1000);
}