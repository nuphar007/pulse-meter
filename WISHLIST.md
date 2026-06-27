# pulse-meter — Feature Wishlist

Ideas for making the counter work better and be more user-friendly.
Reference items by number (e.g. "let's do #1 and #4"). ⭐ = recommended first.

Status: `[ ]` todo · `[~]` in progress · `[x]` done

> Note: the pulse-counting mechanism (ISR/debounce) is intentionally left untouched —
> it has been validated across many configurations and is considered stable.
>
> Business-value/analytics features (fleet health alerting, payout-rate, daily
> history) live in the **clawmachine-dashboard**, not the firmware — so they're
> not tracked here. Fleet alerting is already built and deployed there.

---

## A. Usability

- [ ] **1. ⭐ Web dashboard + OTA-from-browser** — Redesign the bare config page into a
  clean, mobile-friendly dashboard: live counts, MQTT/WiFi status, uptime, signal
  strength, firmware version, plus an **"Update firmware"** button that triggers OTA
  from the browser (paste a release URL or auto-fetch the latest GitHub release).
  *Solves USB-recovery pain: update a device from your phone, no USB / no Adafruit IO.*
- [ ] **2. ⭐ mDNS hostname** — reach a device at `http://spare.local` instead of its IP.
- [ ] **3. Mask & protect secrets in the web UI** — stop pre-filling the WiFi password /
  AIO key into the page; add a simple login to the config page.

## B. Reliability ("work better")

- [ ] **4. ⭐ Non-blocking reconnect + hardware watchdog** — replace the blocking
  `delay()` WiFi/MQTT connect loops (UI freezes ~45s) with a state machine; add a
  watchdog so a hung device auto-reboots instead of silently dying.
- [ ] **5. Auto-update channel** — device periodically checks the GitHub "latest" release
  and self-updates (or notifies). Whole fleet stays current with no manual commands.
- [ ] **6. Code cleanups** (from the review) — per-loop NVS reads in `loop()`, duplicate
  web-route registration, LCD flicker (`lcd.clear()` every second), help-text newline bug.

## C. Security

- [ ] **7. OTA integrity** — currently `setInsecure()` (no cert check); a MITM could push
  fake firmware. Pin GitHub's cert or verify a firmware checksum before applying.
- [ ] **8. Blank the public WiFi password default** — `"86868686"` is still in the public
  source; rely on NVS / AP-mode provisioning instead.

## D. LCD / polish

- [ ] **9. Richer LCD status** — WiFi signal bars, MQTT-connected icon, uptime.
- [ ] **10. Rollback on failed boot** — use ESP32 OTA rollback so a bad image auto-reverts.

---

### Suggested order
**1 → 4** (usability first, then reliability), with #1 built using a proper
mobile-friendly UI.
