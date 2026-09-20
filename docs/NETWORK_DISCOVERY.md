# Network discovery

## Device name

Firmware V7.5 derives the suffix from the Wi-Fi STA MAC and uses:

`Lumitool-Printsever-XXXX`

The firmware sets the DHCP hostname before Wi-Fi starts so home routers should receive the Lumitool name instead of a default ESP32-S2 hostname.

## Identity endpoint

`GET /device-info` returns JSON containing a Lumitool fingerprint, device ID, hostname, MAC address, IP and printer port/status information. The strong identity marker is:

`LUMITOOL_PRINTSEVER`

## UDP discovery

- port: `4210`
- request text: `LUMITOOL_DISCOVER_V1`
- firmware replies with device JSON

## Windows discovery strategy

The setup tool evolved to check the setup address, cached devices, UDP broadcast, Windows ARP/neighbor information and active local subnets. It should search both the direct setup network and the customer's home LAN.

Setup network is normally `192.168.10.0/24`, with the device at `192.168.10.1`. After joining the customer's Wi-Fi, DHCP may assign an address such as `192.168.1.x`.
