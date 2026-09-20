# Hardware

## Controller

Current target: ESP32-S2 mini / S2 Mini style board with native USB OTG. The native USB D-/D+ path is routed to the onboard USB-C connector.

## USB printer topology

Recommended production topology:

```text
ESP32-S2 USB host -> USB 2.0 hub -> Printer A / Printer B / Printer C
```

Mapping:

- hub physical port 1 -> Printer A -> RAW TCP 9101
- hub physical port 2 -> Printer B -> RAW TCP 9102
- hub physical port 3 -> Printer C -> RAW TCP 9103
- direct printer without a hub -> Printer A / 9101

Printers normally use their own power adapters. A tested powered USB 2.0 hub is preferred for a commercial 3-printer setup.

## Buttons

- BOOT = GPIO0
- RESET = EN/CHIP_PU hardware reset; firmware cannot measure how long RESET is held because the CPU is reset while it is asserted.

Firmware button behavior:

- hold BOOT >= 3 s then release -> restart
- hold BOOT >= 10 s then release -> erase saved Wi-Fi and restart
- hardware ROM download mode remains: hold BOOT -> press RESET -> release RESET -> release BOOT

## Status LED

GPIO15 is used as the onboard status LED in the current firmware.
