# Changelog

## Firmware

- V6: first 3-port A/B/C RAW mapping design.
- V7.0: independent per-printer buffered queues and printer status work.
- V7.1: Arduino auto-prototype compile fix.
- V7.2: compact web UI.
- V7.3: BOOT long-press actions and web OTA support.
- V7.4: Lumitool branding and stronger LAN discovery identity.
- V7.5: device name `Lumitool-Printsever-XXXX`, suffix from Wi-Fi STA MAC, hostname configured before Wi-Fi starts.

## Windows setup

- V1: SP46-oriented first setup tool.
- V2: generic installed-driver selection and persistent debug logging.
- V2.1: Windows PowerShell 5.1 WinForms compatibility fix (`PlaceholderText`).
- V2.2: discovery type compatibility fix.
- V3: Lumitool naming and firmware identity/discovery integration.
- V3.1: expanded local-network discovery.
- V3.2: Standard TCP/IP port creation fix; removed invalid SNMP=0 usage and added `prnport.vbs` fallback.
