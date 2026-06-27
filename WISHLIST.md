# pulse-meter — Feature Wishlist

Ideas for making the counter work better and be more user-friendly.
Reference items by number (e.g. "let's do #1 and #7"). ⭐ = recommended first.

Status: `[ ]` todo · `[~]` in progress · `[x]` done

> Note: the pulse-counting mechanism (ISR/debounce) is intentionally left untouched —
> it has been validated across many configurations and is considered stable.

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

## C. Business value

- [ ] **7. ⭐ Fleet health alerting** — "machine X hasn't counted in N hours" (likely jam)
  or "machine offline" → push notification. Catch dead machines same-day.
- [ ] **8. Payout-rate metric** — compute & report wins ÷ dollars per machine to see which
  machines pay out too much/little.
- [ ] **9. Daily snapshots** — add NTP time sync and auto-log daily takings at midnight for
  per-day history instead of just a running total.

## D. Security

- [ ] **10. OTA integrity** — currently `setInsecure()` (no cert check); a MITM could push
  fake firmware. Pin GitHub's cert or verify a firmware checksum before applying.
- [ ] **11. Blank the public WiFi password default** — `"86868686"` is still in the public
  source; rely on NVS / AP-mode provisioning instead.

## E. LCD / polish

- [ ] **12. Richer LCD status** — WiFi signal bars, MQTT-connected icon, uptime.
- [ ] **13. Rollback on failed boot** — use ESP32 OTA rollback so a bad image auto-reverts.

---

### Suggested order
**1 → 4 → 7** (usability, then reliability, then business value), with #1 built using a
proper mobile-friendly UI.
