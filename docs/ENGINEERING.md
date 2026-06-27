# pulse-meter — Engineering Reference

Technical documentation of the firmware, hardware, data model, OTA/CI pipeline, and the
fleet-alerting dashboard. For operation, see [USER-GUIDE.md](USER-GUIDE.md).

---

## 1. System overview

```
   ┌──────────────┐   pulses     ┌────────────────────┐
   │ Game machine │ ───────────▶ │  ESP32 pulse-meter  │
   └──────────────┘   $ / wins   │  (pulse-meter.ino)  │
                                  └─────────┬──────────┘
                       WiFi / MQTT (1883)   │
                                            ▼
                                 ┌────────────────────┐
                                 │   Adafruit IO       │  feeds:
                                 │  (MQTT broker +     │  <user>/feeds/<id>-dollar-pulses
                                 │   feed storage)     │  -win-pulses / -status / -command
                                 └─────────┬──────────┘
                          REST (read)      │
                                            ▼
                                 ┌────────────────────┐    SMTP   ┌──────────┐
                                 │ clawmachine-        │ ────────▶ │  Email   │
                                 │ dashboard (Next.js) │  alerts   └──────────┘
                                 └────────────────────┘
                       OTA: firmware.bin pulled from GitHub Releases over HTTPS
```

Two repos:
- **`nuphar007/pulse-meter`** (this repo) — ESP32 firmware + CI/OTA pipeline.
- **`nuphar007/clawmachine-dashboard`** — Next.js fleet dashboard + alerting.

---

## 2. Hardware

| Function | GPIO | Mode |
|---|---|---|
| Dollars input | 25 | `INPUT_PULLUP`, interrupt on `CHANGE` |
| Wins input | 14 | `INPUT_PULLUP`, interrupt on `CHANGE` |
| Reset button | 17 | `INPUT_PULLUP` (3 s hold = zero counters) |
| Games output | 16 | `OUTPUT` (mirrored pulse) |
| Onboard LED | `LED_BUILTIN` (def. 2) | `OUTPUT` (diagnostic) |
| LCD (20×4) | I²C `0x27` | `LiquidCrystal_PCF8574` |

MCU: ESP32 (CI builds for FQBN `esp32:esp32:esp32`, default partition scheme — has
OTA app0/app1).

Libraries: `PubSubClient`, `LiquidCrystal_PCF8574`, plus core libs (`WiFi`, `WebServer`,
`HTTPUpdate`, `HTTPClient`, `WiFiClientSecure`, `ESPmDNS`, `Preferences`, `Ticker`,
`esp_task_wdt`, `esp_ota_ops`).

---

## 3. Pulse counting (do not modify)

The counting path is deliberately isolated and **must not be changed** — it has been
validated across many machine configurations. It lives between the
`==== Pulse counting ... DO NOT MODIFY ====` banners.

- **`debounceHandler()`** — microsecond debounce with rollover-safe math; on a clean
  edge it sets a `*StateHigh` flag and an `*ISRFlag`.
- **`dollarsISR()` / `winsISR()`** — `IRAM_ATTR` ISRs that call `debounceHandler`.
- **`pulseTimerHandler()`** — 100 ms `Ticker` that drains `outputQueueCount` to the
  games output, one mirrored pulse at a time.
- In `loop()`, the `*ISRFlag` blocks increment `dollarsCount` / `winsCount` and enqueue
  `outputQueueCount`. ISRs only *flag*; the heavy work happens in the loop (keeps ISRs
  short). Counters are `volatile unsigned long`; access is wrapped in
  `noInterrupts()/interrupts()`.

Everything else in the firmware is support (connectivity, UI, OTA, persistence) and is
safe to change.

---

## 4. Data model — Adafruit IO feeds

Feed paths are built once at boot from the username + device ID:

```
<ioUsername>/feeds/<deviceID>-dollar-pulses   (cumulative dollars; resets to 0 on empty)
<ioUsername>/feeds/<deviceID>-win-pulses      (cumulative wins)
<ioUsername>/feeds/<deviceID>-status          ("online" heartbeat, every 10 min)
<ioUsername>/feeds/<deviceID>-command         (commands in, "RESP: …" out)
```

Publishing rules (`loop()`):
- Counters are published **on change**, and at most a forced resend every 10 min if
  changed since last send.
- `-status` gets `online` every 10 min.
- The dashboard derives "days since last activity" from the newest data point on the
  dollar/win feeds.

---

## 5. MQTT command protocol

`mqttCallback()` parses text on the command feed. Messages starting with `RESP:` are
ignored (the device's own replies). See the User Guide §7 for the full table. Key ones:

- `update` → `OTA_LATEST_URL`; `update:<url>` → that URL.
- `checkupdate` → `checkAutoUpdate(true)`.
- `set:autoupdate=on|off`, `set:otasecure=on|off` → persisted to NVS.
- `set:dollars=`, `set:wins=`, `set:debounce=` → set + persist + confirm.
- `status` → multi-line report (counts, RSSI, uptime, IP, autoupdate, version).

---

## 6. OTA update (`performOTA`)

Single entry point used by MQTT, the web `/update` route, and auto-update:

1. **Source allowlist (#7):** if `otaSecure`, reject any URL not starting with
   `https://github.com/nuphar007/pulse-meter/releases/`.
2. Announce start (LCD + command feed), feed the watchdog.
3. `WiFiClientSecure` with `setInsecure()` (see note below) + `HTTPC_STRICT_FOLLOW_REDIRECTS`
   (GitHub release URLs 302-redirect to the storage host).
4. `httpUpdate.update()` with `rebootOnUpdate(false)` so we control the reboot.
5. On `HTTP_UPDATE_OK`: set NVS flag `otaPending=true`, then `ESP.restart()`.
6. After reboot, `setup()` reads/clears `otaPending` and (once MQTT is up) publishes
   `RESP: OTA SUCCESS: now running <version>` — the reliable post-reboot confirmation.
   Failures publish `RESP: OTA FAILED (<code>): <reason>`.

**TLS note:** transport uses `setInsecure()` (no cert validation). Cert pinning is
unreliable across GitHub's redirect host and risks bricking OTA, so integrity is gated
on the **source URL allowlist** instead, which defends the realistic threat
(command-feed URL injection). Embedding the ESP32 CA bundle was attempted but the
`setCACertBundle` API/symbol is core-version fragile — see §11.

### Auto-update channel (`checkAutoUpdate`)
- Off by default (`autoUpdateEnabled`). When on, every 6 h it GETs
  `api.github.com/repos/<repo>/releases/latest`, extracts `tag_name` by string scan
  (no JSON lib), and if it differs from `FIRMWARE_VERSION`, calls `performOTA` with the
  tagged `firmware.bin` URL.

---

## 7. Connectivity & reliability

- **Non-blocking reconnect (#4):** `connectToWiFi()` blocks only at boot (bounded 30 s).
  In `loop()`, WiFi relies on `setAutoReconnect(true)` and re-`begin()`s if down ≥30 s.
  MQTT uses `mqttConnectOnce()` — a **single** attempt per 15 s (no retry/delay loops),
  with `setSocketTimeout(5)` to bound the blocking connect. `onMqttConnected()` announces
  boot/OTA once.
- **Watchdog (#4):** task WDT (`esp_task_wdt_reconfigure` + `esp_task_wdt_add(NULL)`),
  `WDT_TIMEOUT_S = 60`, fed at the top of `loop()` and before OTA. A genuine hang reboots
  the device.
- **Boot-loop guard (#10):** NVS `bootCount` increments each boot. ≥4 → boot straight to
  AP recovery instead of looping. After 30 s of healthy runtime, `loop()` clears
  `bootCount` and calls `esp_ota_mark_app_valid_cancel_rollback()`.
- **mDNS (#2):** `MDNS.begin(deviceID)` on connect → `http://<deviceID>.local`.

---

## 8. Persistence — NVS (`Preferences`, namespace `pulseCounter`)

| Key | Type | Meaning |
|---|---|---|
| `deviceID`, `ssid`, `password`, `ioUser`, `aioKey`, `cfgPass` | String | identity, WiFi, AIO creds, web login |
| `autoUpd`, `otaSecure`, `otaPending` | Bool | auto-update, official-only OTA, OTA-success flag |
| `dollarsCount`, `winsCount`, `gamesOutputCount`, `debounceTime` | ULong | counters + debounce |
| `bootCount` | UInt | boot-loop guard |

**Flash-wear note (#6):** the loop compares counters to **RAM shadows**
(`savedDollarsCount` etc.), not NVS — counters are written at most every 10 s after a
change, never read from flash per-iteration.

---

## 9. Web server

All routes are registered once in `startWebServer()` (called from `setup()`):
`/` (GET), `/saveSettings`, `/setCounters`, `/setDebounce`, `/update` (POST). Every
handler calls `requireAuth()` first — HTTP Basic (`admin` + `cfgPass`) when a config
password is set, open otherwise. `handleRoot()` renders a self-contained mobile-friendly
page (inline CSS); secrets are never echoed (placeholders only).

---

## 10. Build, versioning & release (CI/OTA pipeline)

- **Workflow:** `.github/workflows/build.yml`.
  - Push to `main` → compile-only **build-check**.
  - Push a tag `v*` → compile + publish a **GitHub Release** with `firmware.bin` and
    `firmware-<tag>.bin`.
- **Version injection:** `FIRMWARE_VERSION` is wrapped in `#ifndef`. CI compiles with
  `-DFIRMWARE_VERSION="<tag>"` so OTA builds report the exact tag. The in-file fallback
  is only for local Arduino-IDE builds — **bump it to match the tag** you're about to
  release.
- **Local build:**
  ```bash
  arduino-cli core install esp32:esp32
  arduino-cli lib install "PubSubClient" "LiquidCrystal_PCF8574"
  arduino-cli compile --fqbn esp32:esp32:esp32 ./pulse-meter
  ```
- **Cut a release:** `git tag v1.x && git push origin v1.x`, wait for the build to
  publish (~2 min — the binary uploads *after* the tag), then OTA.

### Release → deploy flow
```
edit firmware → bump fallback FIRMWARE_VERSION → push main (build-check)
   → git tag vX → CI builds + publishes Release
   → send `update` (or web button) to a SPARE → verify
   → roll out to fleet (command feed / auto-update)
```

---

## 11. Known limitations & future work

- **OTA TLS is `setInsecure`.** Integrity is via source-URL allowlist, not cert
  validation (CA-bundle API is core-version fragile and risks bricking OTA).
- **Bootloader auto-rollback not enabled.** True hardware rollback needs a custom
  partition/sdkconfig that stock `arduino-cli` doesn't build. The firmware calls
  `esp_ota_mark_app_valid_cancel_rollback()` (forward-compatible) and provides a
  *software* boot-loop→AP guard instead.
- See [`WISHLIST.md`](../WISHLIST.md) for the remaining backlog (e.g. configurable AP
  password, richer LCD glyphs).

---

## 12. Fleet alerting (dashboard side, summary)

In `clawmachine-dashboard` (Next.js 16 / TS):
- `src/lib/alerting.ts` evaluates each machine's **days since last dollar/win activity**
  (via Adafruit IO `/data/last`). Threshold default 1 day; machines silent >30 days are
  treated as decommissioned.
- `POST /api/cron/health-check` (secret-protected) runs the check, emails on
  transitions (silent / recovered), de-duplicated via `data/alert-state.json`. A
  **systemd timer** on the VPS fires it hourly (`deploy/`).
- Settings (threshold, recipient, SMTP) are editable in **Admin → Alerts**, stored in
  `site-config.json` with env-var fallback. `CRON_SECRET` stays server-side.
- Deploy: push to `main` → self-hosted runner pulls, builds, `pm2 restart`.

---

## 13. Contributing / changing the firmware

1. Edit `pulse-meter/pulse-meter.ino`. **Never** touch the pulse-counting block (§3).
2. Bump the fallback `FIRMWARE_VERSION`.
3. Push to `main`; confirm the **build-check** is green (catches API/portability issues
   — historically `WiFiClientSecure` includes, `LED_BUILTIN`, watchdog/CA-bundle APIs).
4. Tag `vX.Y` to release; **test on the spare via OTA** before fleet rollout.
5. Keep ISRs short, avoid blocking `delay()` in `loop()` (the watchdog allows 60 s but
   responsiveness matters), and persist new settings in NVS with a fallback default.
