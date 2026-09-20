# Current project state

Last updated from the ChatGPT development session on 2026-09-20.

## Source of truth

- Firmware: `firmware/current/Lumitool_Printsever_V7_5.ino`
- Firmware build options: `firmware/current/build_opt.h`
- OTA partition table: `firmware/current/partitions.csv`
- Windows setup: `windows-app/current/Lumitool_Printsever_Setup_V3_2.ps1`
- Windows launcher: `windows-app/current/CHAY_LUMITOOL_PRINTSEVER_SETUP_V3_2.cmd`

## Latest user-visible behavior

Device identity should be `Lumitool-Printsever-XXXX`, where XXXX is the last four hexadecimal characters of the Wi-Fi STA MAC address. Firmware V7.5 sets the Wi-Fi hostname before Wi-Fi starts to avoid default router names such as `esp32s2-xxxxxx-2G`.

The Windows setup utility should discover both the direct setup network and the customer's home LAN. The device setup address is `192.168.10.1`; after joining home Wi-Fi it can receive a DHCP address such as `192.168.1.x`.

Printer mapping is:

- A -> RAW TCP 9101
- B -> RAW TCP 9102
- C -> RAW TCP 9103

## Latest fixes

Windows Setup V3.2 was created after V3.1 failed while creating a Standard TCP/IP port with:

`One or more specified parameters for this operation has an invalid value.`

V3.2 removes the problematic `-SNMP 0` argument from `Add-PrinterPort` and has a `prnport.vbs` fallback.

## Verification status

Important: the current snapshots were developed iteratively.

- Firmware V7.5 source is preserved exactly in this repository, but it was **not compiled in the ChatGPT environment** because Arduino CLI / the exact installed Arduino environment was unavailable.
- Windows Setup V3.2 source is preserved exactly, but at the time of this handoff the user had **not yet reported the result of testing V3.2**.
- Before calling a release production-ready, compile V7.5 with the user's Arduino-ESP32 / EspUsbHost versions and test A/B/C printing, discovery, BOOT actions and OTA on real hardware.

## Arduino environment used by the user

Known settings during development:

- Arduino IDE 2.x
- ESP32 by Espressif Systems 3.3.11
- Board: ESP32S2 Dev Module
- Flash Size: 4 MB
- USB CDC On Boot: Disabled
- Upload Mode: UART0
- PSRAM: Enabled for the current queue design when the target board supports it

## Do not commit

This repository is intentionally public. Do not add customer Wi-Fi credentials, passwords, private keys, signing material, TeamViewer credentials, or production secrets.
