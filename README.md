# pulse-meter (Yu.Me Game-Machine Meter)

ESP32 firmware that counts `dollars` and `wins` pulses from game/claw machines,
mirrors them to a `games` output, shows status on a 20x4 LCD, and reports to
**Adafruit IO** over MQTT. Configuration (WiFi, device ID, Adafruit IO
username/key, counters, debounce) is done over WiFi via a built-in web page, and
firmware updates are delivered over-the-air (OTA).

## Repo layout

```
pulse-meter/pulse-meter.ino       # the sketch (single source of truth)
.github/workflows/build.yml       # CI: compile + publish firmware on each tag
```

The firmware version comes from the **git tag** at build time (injected into
`FIRMWARE_VERSION`). The value hard-coded in the sketch is only a local-build
fallback — don't rely on editing it for releases.

## Deploy a new version (the whole workflow)

1. Commit your changes and push to `main`.
   - CI compiles the sketch as a check (no release produced).
2. Cut a release by pushing a version tag:
   ```bash
   git tag v1.52
   git push origin v1.52
   ```
3. GitHub Actions builds `firmware.bin` and attaches it to a new
   **GitHub Release** named `Firmware v1.52`.
4. Copy the release asset URL. It looks like:
   ```
   https://github.com/<you>/pulse-meter/releases/download/v1.52/firmware.bin
   ```
5. Push the OTA command to a device via its Adafruit IO **command feed**
   (`<ioUsername>/feeds/<deviceID>-command`):
   ```
   update:https://github.com/<you>/pulse-meter/releases/download/v1.52/firmware.bin
   ```
   The device downloads the binary, flashes itself, and reboots on the new version.

Confirm afterwards by sending `version` to the same command feed.

## MQTT command reference

Send these to `<ioUsername>/feeds/<deviceID>-command`:

| Command | Action |
|---|---|
| `update:<url>` | OTA update from a firmware `.bin` URL |
| `set:dollars=<n>` / `set:wins=<n>` | Set a counter |
| `set:debounce=<ms>` | Set input debounce |
| `reset` | Zero all counters |
| `status` | Report counters, IP, firmware version |
| `version` | Report firmware version |
| `get:dollars` / `get:wins` / `get:debounce` | Read a value |
| `?` | List commands |

## Web configuration

- On boot the device joins the saved WiFi and serves a config page at its IP.
- If WiFi fails, it starts an access point **`ESP32_Config`** (password
  `12345678`) at `192.168.4.1`.
- The page sets device ID, WiFi credentials, Adafruit IO username/key, counters,
  and debounce. Saving reboots the device.

## Building locally (optional)

Requires [arduino-cli](https://arduino.github.io/arduino-cli/) + the ESP32 core:

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "PubSubClient" "LiquidCrystal_PCF8574"
arduino-cli compile --fqbn esp32:esp32:esp32 ./pulse-meter
```

### Finding your board's FQBN

The CI defaults to `esp32:esp32:esp32` (generic ESP32 Dev Module). If your board
differs, list options with `arduino-cli board listall esp32` and update the
`FQBN` value in `.github/workflows/build.yml`.
