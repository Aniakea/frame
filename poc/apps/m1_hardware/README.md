# Frame M1 Hardware Firmware

This isolated ESP-IDF application implements the `0.1.0-dev.1` hardware-status vertical slice for
the Waveshare ESP32-S3-RLCD-4.2 N16R8 board. It does not load dynamic plugins.

It checks the ESP32-S3 target, 16 MiB flash, 8 MiB PSRAM, ST7305 display, shared I2C RTC/SHTC3,
active-low KEY, SDMMC TF card transitions, Wi-Fi STA configuration with softAP web provisioning,
native landscape 400x300 display output, and USB Serial/JTAG console output.

Wi-Fi provisioning persists the last credentials as plaintext JSON at `/system/wifi.json` on the
internal `system_fs` partition. This is a user-approved development-board exception (SEC-010,
recorded in `_doc/requirements-v4.2.md` under M1-007): ESP-IDF v6.0.2 requires Flash Encryption or
an HMAC eFuse key to protect NVS encryption keys, and both provisioning steps are irreversible on
the single development board. Production boards must switch to NVS-encrypted credential storage
before shipping. The password never appears in UART output, logs, or status.

On boot the device auto-connects with the saved credentials. When no valid configuration exists, or
after 5 disconnects without an IP address or 60 seconds without obtaining one, it starts an open
setup hotspot `Frame-Setup-XXXX` at `192.168.4.1`. The portal's network list is populated from a
live scan; hidden networks cannot be typed into the browser form — configure those through the
console with `wifi set <ssid> hidden`. Holding KEY for 3 seconds or longer also requests the setup
hotspot; such a hold is a portal command and does not count as a button press.

Build with the exact IDF tag:

```sh
. "$IDF_PATH/export.sh"
cmake --preset default --fresh
cmake --build --preset default
```

The console is available through USB Serial/JTAG at 115200 baud. Useful commands:

```text
status [--json]
wifi set <ssid> [hidden]
wifi clear
wifi reconnect
wifi status
prov status
prov start
prov scan
storage probe
storage status
selftest
```

Do not place real credentials in command transcripts or committed evidence. `wifi set` asks for the
password after parsing the command so it is not stored in command history; terminal software must
also keep local echo disabled. The password is persisted only inside `/system/wifi.json` under the
SEC-010 development-board exception and is never echoed back.
