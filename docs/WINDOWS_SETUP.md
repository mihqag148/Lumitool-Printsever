# Windows setup

Current tool: **Lumitool Printsever Setup V3.4**.

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

V3.4 first uses `Add-PrinterPort`. If that fails on a Windows build, it falls back to the Windows `prnport.vbs` script for a RAW custom TCP port with SNMP disabled.

## V3.4 scan behavior

Network discovery runs in a separate hidden PowerShell process. The WinForms UI no longer performs the subnet sweep on the UI thread. The setup app polls progress through temporary JSON files, displays a progress bar, and provides a cancel button. Discovery still checks the setup network, cached IPs, UDP discovery, Windows neighbor data, and active IPv4 /24 networks.

## RAW completion mode

V3.4 treats TCP socket close as the generic completion signal. After creating/updating the queue it disables SNMP status polling on the Standard TCP/IP port and disables bidirectional support on the Windows printer queue. This avoids vendor/status polling keeping an already-transferred job stuck in Printing/Waiting when the ESP print server does not implement that model-specific status protocol.
