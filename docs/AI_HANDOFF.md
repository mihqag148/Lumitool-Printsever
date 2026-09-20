# AI handoff

This file is the quickest context for another AI or developer.

## Goal

Build a simple commercial Wi-Fi print server around ESP32-S2 for up to three USB printers. The server exposes RAW TCP queues on one IP:

- A = 9101
- B = 9102
- C = 9103

Windows customers should not need to manually create Standard TCP/IP printer ports; the Windows setup utility discovers the device and creates the port/queue using any already-installed printer driver.

## Current source of truth

- `firmware/current/Lumitool_Printsever_V7_5.ino`
- `windows-app/current/Lumitool_Printsever_Setup_V3_2.ps1`

Treat archive files as historical/debug context, not the current implementation.

## Important firmware decisions

- ESP32-S2 native USB host, optional USB 2.0 hub.
- Physical hub ports 1/2/3 map to A/B/C.
- Queue data can use PSRAM when available.
- Web UI includes printer status, queue/log pages, Wi-Fi setup and OTA support.
- Device ID XXXX is the last four hex characters of Wi-Fi STA MAC.
- Network name is `Lumitool-Printsever-XXXX`.
- Discovery identity endpoint: `/device-info`.
- UDP discovery: port 4210, request `LUMITOOL_DISCOVER_V1`.
- BOOT >=3 s: restart. BOOT >=10 s: clear saved Wi-Fi + restart.
- Keep hardware BOOT+RESET ROM download mode unchanged.

## Important Windows decisions

- PowerShell 5.1 + WinForms compatibility matters.
- Must request Administrator rights.
- Do not assume SHOPTIDA SP46: list all installed Windows printer drivers.
- Use RAW ports 9101/9102/9103.
- V3.4 avoids `-SNMP 0` with `Add-PrinterPort`; fallback to `prnport.vbs` when needed.
- Discovery should consider both setup network `192.168.10.x` and the customer's home LAN such as `192.168.1.x`.

## Known caution

The historical firmware revisions were generated iteratively and not every archive revision was compiled against every EspUsbHost library release. Verify the current EspUsbHost API signatures before large refactors. Do not claim physical paper completion merely because bytes were accepted by USB; the strongest generic statement is that data was sent to the printer.

## Public repository

This repository is intentionally public. Never add customer Wi-Fi credentials, private keys, signing material or other secrets.

## RAW completion behavior

Firmware V7.6 closes the Windows RAW TCP connection immediately after the job has been transferred to USB, then logs DONE. Windows Setup V3.4 disables SNMP and queue bidirectional status because generic ESP-to-USB forwarding cannot emulate every printer vendor's status protocol.
