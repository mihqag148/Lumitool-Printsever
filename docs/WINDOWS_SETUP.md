# Windows setup

Current tool: **Lumitool Printsever Setup V3.2**.

The tool requires Administrator permission because it creates Windows printer ports and queues. It supports any printer model as long as the correct Windows driver is already installed.

## Port mapping

- Printer A -> RAW TCP 9101
- Printer B -> RAW TCP 9102
- Printer C -> RAW TCP 9103

## Installation flow

1. Install the printer manufacturer's Windows driver.
2. Run `CHAY_LUMITOOL_PRINTSEVER_SETUP_V3_2.cmd`.
3. Discover or enter the Lumitool device IP.
4. Select the correct installed driver.
5. Install Printer A, B, or C.
6. The tool creates a Standard TCP/IP RAW port and Windows printer queue.

V3.2 first uses `Add-PrinterPort`. If that fails on a Windows build, it falls back to the Windows `prnport.vbs` script for a RAW custom TCP port with SNMP disabled.
