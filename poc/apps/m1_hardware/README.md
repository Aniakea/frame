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

## Buttons

The board's three buttons are all observable:

- **KEY** (GPIO18, active-low): a release under 1 second counts as a short press, 1-3 seconds
  as a long press, and 3 seconds or more requests the setup hotspot (a portal command, not a
  press). Both counters appear on the display and in `status` as `KEY: short/long`.
- **BOOT** (GPIO0, active-low with the internal pull-up): the same release bands, under 1
  second short and 1 second or more long, counted as `BOOT: short/long`. It is a strapping
  pin, so the firmware only reads it at runtime and never drives it, and it has no portal
  binding.
- **RST**: hardwired to the chip reset line, so it cannot be sampled as a GPIO. Instead the
  firmware increments a persistent boot counter once per boot (NVS namespace `frame`, key
  `rst_cnt`) and reports it as `RST: count` / `reset_count`; pressing RST reboots the board
  and increments the counter.

## Timezone

Internal time handling is strictly UTC: the RTC chip, JSONL telemetry, and the machine `utc`
fields all carry UTC with a `Z` suffix. Only human-facing surfaces convert to local time —
China Standard Time (UTC+8, no daylight-saving transitions): the display RTC line, the text
`status` output, and `rtc status` render local time labeled `UTC+8`, and `rtc set` interprets
its argument as UTC+8 local time (the firmware converts it to UTC before writing the chip).

## Real-Time Clock

The PCF85063 RTC shares the I2C bus with the SHTC3. Nothing writes the chip from the factory, so
after a power loss its oscillator-stopped (OS) flag is set and the time reads invalid on the
display (`-`) and in JSONL (`"utc":""`). The firmware now provides two time sources:

- **SNTP:** once the station has an IP address, the firmware starts SNTP against
  `ntp.aliyun.com` and `pool.ntp.org`, and on the first successful sync writes the UTC time to
  the RTC (log line `rtc synced from sntp`). SNTP keeps running; periodic re-syncs update the
  system clock.
- **Console:** `rtc set` writes the chip manually (the argument is UTC+8 local time; see
  [Timezone](#timezone)).

A CR1220 coin cell on the board keeps the RTC running across power-off. Without a battery (or
with an empty one) every boot loses the time and the OS flag returns — install a fresh cell for
persistence across power loss.

Build with the exact IDF tag:

```sh
. "$IDF_PATH/export.sh"
cmake --preset default --fresh
cmake --build --preset default
```

The console is available through USB Serial/JTAG at 115200 baud. Useful commands:

```text
status [--json]
rtc status
rtc raw
rtc set YYYY-MM-DD HH:MM:SS
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
metrics
```

`rtc status` reports validity, unix time, formatted UTC, formatted UTC+8 local time, and the
OS-bit state; `rtc raw` dumps PCF85063 registers 0x00-0x0A with a decode; `rtc set` parses
`YYYY-MM-DD HH:MM:SS` as UTC+8 local time, validates the ranges (calendar round-trip), converts
to UTC, writes the chip, and re-reads it to confirm.

`metrics` prints machine-parseable memory lines: `heap internal free=<u> largest=<u>
min_ever=<u>` and `heap psram ...` (8-bit-capable pools, `min_ever` from the boot-time
watermark), then one `task <name> stack_hwm_bytes=<u>` line per FreeRTOS task, where the
high-water mark is the minimum-ever free stack in bytes (on this ESP-IDF Xtensa port
`StackType_t` is one byte, so the kernel reports the mark directly in bytes).

Do not place real credentials in command transcripts or committed evidence. `wifi set` asks for the
password after parsing the command so it is not stored in command history; terminal software must
also keep local echo disabled. The password is persisted only inside `/system/wifi.json` under the
SEC-010 development-board exception and is never echoed back.
