# pulse-meter — User & Admin Guide

A practical guide to operating and configuring the Yu.Me pulse-meter devices and
the fleet dashboard. For internals and development, see [ENGINEERING.md](ENGINEERING.md).

---

## 1. What the device does

Each pulse-meter is an ESP32 box fitted to a game/claw machine. It:

- **Counts** two pulse inputs — **dollars** (money in) and **wins** (prizes out).
- **Mirrors** a "games" output pulse for every input pulse.
- **Shows** live status on a 20×4 LCD.
- **Reports** the counters to **Adafruit IO** over WiFi (MQTT) so the
  **dashboard** can show every machine and alert you when one goes quiet.
- **Updates itself** wirelessly (OTA) from new firmware releases.

---

## 2. Reading the LCD

```
Wins: 1234        MQ      <- win count            (MQ = MQTT online, -- = offline)
$: 5678                   <- dollar count
spare -52dBm 3h12m        <- device ID, WiFi signal, uptime
v1.55 192.168.1.50        <- firmware version, IP address
```

- **MQ / --** (top right): connected to Adafruit IO, or not.
- **dBm**: WiFi signal strength (closer to 0 is stronger; -50 great, -80 weak).
- **Uptime**: how long since last reboot.
- The bottom row briefly shows status messages (e.g. `MQTT OK`, `OTA: downloading`)
  then returns to version + IP.

---

## 3. First-time setup (provisioning a new device)

A brand-new device has no WiFi saved, so it starts its own access point:

1. Power on the device. The LCD shows **`AP Mode`** and an IP (`192.168.4.1`).
2. On a phone/laptop, join the WiFi network **`ESP32_Config`** (password **`12345678`**).
3. Open a browser to **`http://192.168.4.1`**.
4. Fill in the **Settings** form (see §5) — at minimum **Device ID**, **WiFi SSID**,
   **WiFi Password**, **Adafruit IO Username**, and **Adafruit IO Key**.
5. Click **Save & reboot**. The device restarts, joins your WiFi, and connects to
   Adafruit IO.

> **Device ID** becomes part of every feed name and must be unique per machine
> (e.g. `us1`, `bk2`). It's automatically lower-cased.

---

## 4. Reaching the config page later

Once a device is on your WiFi, open its page two ways:

- **By name (easiest):** `http://<deviceID>.local` (e.g. `http://us1.local`) — works
  thanks to mDNS.
- **By IP:** the address shown on the LCD bottom row.

If a login box appears, the **config-page password** is set (see §8); username is
**`admin`**.

---

## 5. The web configuration page

The page shows a live status panel (dollars, wins, MQTT, WiFi, uptime, IP) and these
sections:

### Firmware update (OTA)
- A URL field pre-filled with the **latest official release**. Click **Update firmware**
  to pull it. The device reboots if successful.

### Settings
| Field | What it does |
|---|---|
| **Device ID** | Unique machine name; part of the feed paths. Lower-cased. |
| **WiFi SSID / Password** | Network to join. Password blank = keep current. |
| **Adafruit IO Username** | Your Adafruit account username (e.g. `Yume86`). |
| **Adafruit IO Key** | Your AIO key. Blank = keep current. |
| **Config page password** | Set to require a login on this page. Blank = keep current. |
| **Auto-update from GitHub latest** | If ticked, the device checks for new firmware every 6 h and self-updates. Off by default. |
| **Only allow OTA from official GitHub releases** | If ticked, OTA only accepts firmware from your official release URLs (recommended for production). |

Click **Save & reboot** to apply.

### Counters
Manually set the dollar/win counters (e.g. after emptying a machine). Click
**Update counters**.

### Debounce
Sets the input debounce time in milliseconds (default 60). Increase if you see
double-counting; decrease if fast pulses are missed. Click **Update debounce**.

---

## 6. Updating firmware

There are three ways, all wireless:

1. **Web button** — open the device page → **Update firmware** (defaults to latest).
2. **Command feed** (admins) — send `update` (latest) or `update:<url>` to the device's
   command feed (see §7).
3. **Auto-update** — enable per device; it pulls new releases every 6 hours.

Recommended workflow: **test a new release on one spare device first**, confirm it's
healthy, then roll it out.

After an update the device reboots and reports `RESP: OTA SUCCESS: now running vX.XX`
on the command feed.

---

## 7. Admin: controlling devices via the command feed

Admins can control any device by publishing text to its Adafruit IO **command feed**:

```
<AdafruitUsername>/feeds/<deviceID>-command
```
e.g. `Yume86/feeds/us1-command`. The device replies on the same feed with `RESP: …`.

| Command | Action |
|---|---|
| `update` | OTA to the latest official release |
| `update:<url>` | OTA from a specific firmware URL |
| `checkupdate` | Check GitHub for a newer release now |
| `set:autoupdate=on` / `off` | Enable/disable the auto-update channel |
| `set:otasecure=on` / `off` | Restrict OTA to official releases on/off |
| `set:dollars=<n>` | Set the dollar counter |
| `set:wins=<n>` | Set the win counter |
| `set:debounce=<ms>` | Set input debounce |
| `reset` | Zero all counters |
| `status` | Report counters, IP, signal, uptime, version |
| `get:dollars` / `get:wins` / `get:debounce` | Read one value |
| `version` | Report firmware version |
| `on` / `off` | Toggle the onboard LED (diagnostic) |
| `?` | List all commands |

---

## 8. Security

- **Config-page login** — optional. Set a **Config page password** in Settings; the
  whole web interface then requires username `admin` + that password. Off by default.
- **Official-only OTA** — tick **Only allow OTA from official GitHub releases** so a
  device can never be pushed firmware from an untrusted URL. Recommended on for
  production; turn off only to test custom builds.
- **Secrets** (WiFi password, AIO key) are never shown back in the page once set —
  the fields show dots and blank means "keep current".

---

## 9. Recovery & troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| LCD shows **`AP Mode`** | Can't join WiFi. Join `ESP32_Config` / `12345678`, fix the SSID/password. |
| LCD shows **`Boot loop! AP`** | The device rebooted 4× quickly (bad firmware/config). It dropped to AP mode for recovery — reconfigure or re-flash. |
| **`MQTT --`** / `Missing AIO Key` | No/invalid Adafruit IO key, or wrong username. Re-enter in Settings. |
| **`MQTT Fail` / Retry** | WiFi up but Adafruit unreachable or wrong credentials. |
| **`OTA FAILED (… 404)`** | The firmware URL doesn't exist yet (release still building) or is wrong. |
| **`OTA blocked: untrusted URL`** | Official-only OTA is on and the URL isn't an official release. Turn it off to use a custom URL. |
| Counts look doubled / missed | Adjust **Debounce** (higher to fix doubles, lower to catch fast pulses). |
| Totally unresponsive | Hold the **reset button 3 s** zeros counters (not a reboot). Power-cycle to reboot. Last resort: re-flash via USB (Arduino IDE, "Erase All Flash" **disabled** to keep settings). |

---

## 10. The fleet dashboard

The dashboard (separate web app on your server) shows every machine and emails you when
one goes silent.

- **Machine cards** show dollars, wins, online status, and a red **"⚠ N d silent"**
  badge if a machine hasn't counted anything for the configured number of days.
- **Admin → Adafruit IO** — set the account username/key the dashboard reads from
  (must match the devices' account, e.g. `Yume86`).
- **Admin → Alerts** — set the **silence threshold (days)**, the **email recipient**,
  and **SMTP** details, and use **Send test email** to confirm delivery. Email is sent
  once when a machine goes silent and once when it recovers.

> The dashboard runs an hourly background check on the server. If a machine is silent
> beyond the threshold, you get an email and the card shows the badge.

---

## 11. Quick reference

- **Config AP:** `ESP32_Config` / `12345678` → `http://192.168.4.1`
- **Device page:** `http://<deviceID>.local`
- **Command feed:** `<AIOuser>/feeds/<deviceID>-command`
- **Reset counters:** hold the reset button 3 seconds
- **Latest firmware:** https://github.com/nuphar007/pulse-meter/releases
