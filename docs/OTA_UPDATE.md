# OTA firmware update

Firmware V7.3 and later include a web support/OTA page. Current firmware is V7.5.

Typical URL:

`http://<device-ip>/support`

The support page accepts a compiled application `.bin`, not the `.ino` source file. The initial migration to the OTA partition layout must be flashed over USB once so `partitions.csv` is written. Future updates can then use the web OTA flow.

OTA behavior:

- stop accepting new print jobs
- allow existing jobs to finish
- upload firmware into the inactive OTA slot
- reboot after a successful update

The current support login uses `admin` with the four-character device suffix as the password. This is convenience-level access control, not strong production security. Do not place private signing keys in this public repository.
