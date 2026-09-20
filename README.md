# Lumitool Printsever

Public source repository for the Lumitool Printsever project.

## Current versions

- Firmware: **V7.5** (`firmware/current/`)
- Windows setup tool: **V3.2** (`windows-app/current/`)
- MCU: ESP32-S2
- USB printer mapping: A=`9101`, B=`9102`, C=`9103`

## What the project does

Lumitool Printsever turns an ESP32-S2 into a Wi-Fi RAW print server for up to 3 USB printers through a USB 2.0 hub. Each physical printer has an independent RAW TCP queue. The Windows setup tool discovers Lumitool devices on the LAN, lets the user select any installed Windows printer driver, and creates the Standard TCP/IP printer port/queue.

## Device identity

Firmware V7.5 uses the Wi-Fi STA MAC address. The device name is:

`Lumitool-Printsever-XXXX`

where `XXXX` is the final 4 hexadecimal characters of the STA MAC address.

Discovery methods include HTTP `/device-info`, UDP discovery on port `4210`, mDNS, cached IPs, Windows ARP/neighbor data, and subnet fallback scanning.

## Repository layout

- `firmware/current/` - current ESP32-S2 source and partition/build files
- `firmware/archive/` - historical firmware source snapshots
- `windows-app/current/` - current Windows installer/setup source
- `windows-app/archive/` - historical Windows setup source snapshots
- `docs/` - hardware, build, OTA, discovery, Windows setup and project handoff notes

## Build notes

Arduino IDE / Arduino-ESP32 target used during development:

- Board: ESP32S2 Dev Module
- Flash size: 4 MB
- USB CDC On Boot: Disabled
- Upload Mode: UART0
- PSRAM: Enabled when supported by the board

Keep `build_opt.h` and `partitions.csv` in the same sketch directory as the current `.ino` when building.

## Security note

This repository is intentionally **public**. Do not commit customer Wi-Fi passwords, private signing keys, OTA secrets, production credentials, or other secrets.

## AI handoff

Start with `docs/AI_HANDOFF.md`, then read the current firmware and Windows setup source. The archive folders preserve earlier revisions and debugging history.
