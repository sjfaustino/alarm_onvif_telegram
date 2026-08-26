# alarm-onvif-telegram

Multi-camera ONVIF motion monitor for ESP32. Subscribes to motion/tamper/signal-loss
events on one or more ONVIF-compliant IP cameras and pushes a Telegram photo alert
(snapshot + caption) whenever motion fires, with a periodic heartbeat so you notice
if the board goes quiet.

Built with [pioarduino](https://github.com/pioarduino/platform-espressif32), a
community fork of PlatformIO's `espressif32` platform that tracks newer
Arduino-ESP32/IDF releases.

## Features

- Polls each enabled camera's ONVIF PullPoint subscription on its own FreeRTOS task
  (parallel, not round-robin) and auto-resubscribes/retries on failure.
- Sends a Telegram photo alert on motion/tamper events, per-camera cooldown to avoid
  spam, JPEG buffered once in PSRAM and resent to every Telegram user subscribed to
  that camera.
- TLS to Telegram is certificate-pinned (not `setInsecure()`).
- Per-camera quirks handled via config flags: WS-Security vs. HTTP Basic Auth,
  optional `InitialTerminationTime`/`ReplyTo` (needed by some Xiongmai-derived
  stacks), snapshot URI override, and preferred video profile.
- Periodic Telegram heartbeat (uptime, free heap, per-camera subscription status)
  so a silently hung or endlessly-retrying board doesn't go unnoticed.
- Cameras and Telegram recipients are managed at runtime through a built-in
  sidebar dashboard ([hoeken/PsychicHttp](https://github.com/hoeken/PsychicHttp))
  and persisted in NVS - no more editing and reflashing `config.h`/`secrets.h` to
  change either. Camera changes take effect after a reboot; Telegram user changes
  apply immediately.
- Any number of Telegram users, each independently configured for which cameras
  they hear from (specific list or "all, including future ones"), whether they get
  the heartbeat/boot messages, and whether they're allowed to send `/on`, `/off`,
  `/status` commands (which apply per-camera and reply to whoever sent them).

## Hardware

**PSRAM is required.** A motion alert can go to more than one Telegram user, so the
snapshot JPEG is buffered once in RAM and resent per recipient - `src/main.cpp`'s
`setup()` checks `ESP.getPsramSize()` and refuses to start (loops forever printing a
warning instead of proceeding) if there isn't any, rather than run degraded and fail
partway through a send later.

Tested on an ESP32-S3 with 8MB embedded octal PSRAM — `[env:esp32s3]`. Also needs a
second core (`src/main.cpp` pins one FreeRTOS task per camera to core 1), which every
PSRAM-equipped ESP32-S3 variant has, so this hasn't been a real constraint in
practice; a single-core PSRAM chip is untested.

## Setup

1. **Clone and copy the templates:**
   ```sh
   cp include/secrets.h.example include/secrets.h
   ```
   `secrets.h` is gitignored — never commit it.

2. **Fill in `include/secrets.h`:**
   - `WIFI_SSID` / `WIFI_PASSWORD`
   - `TELEGRAM_BOT_TOKEN` / `TELEGRAM_CHAT_ID` — message
     [@BotFather](https://t.me/BotFather) to create a bot, then message
     [@userinfobot](https://t.me/userinfobot) (or hit
     `https://api.telegram.org/bot<TOKEN>/getUpdates` after messaging your bot once)
     to get your chat ID. `TELEGRAM_CHAT_ID` becomes the seed for a single "Admin"
     Telegram user (all cameras, heartbeat/boot messages, can command) on first boot -
     add more users, or adjust that one, from the web UI's Telegram Users page after.
   - `CAMERA_SEED[]` — optional, one-time only. Leave it empty to add every camera
     through the web UI after first boot; fill it in only if you're migrating cameras
     already tuned elsewhere and want them pre-populated. It's read exactly once, on
     the very first boot after NVS has no camera data yet, and never again after that.

3. **Fill in `include/telegram_ca.h`** with Telegram's current root CA — instructions
   are in that file's comments (`openssl s_client` one-liner or browser cert export).
   Root CAs don't rotate often, so this is a one-time setup step, not a per-build one.
   The boot log warns you plainly if this is still the placeholder.

4. **Build and upload:**
   ```sh
   pio run -e esp32s3 -t upload
   pio device monitor -b 115200
   ```
   `default_envs` in `platformio.ini` already picks `esp32s3` if you omit `-e`.

5. **Open the dashboard** at `http://<board's IP>/` (printed in the boot log and the
   Telegram boot message). The sidebar has three sections:
   - **Network** — connection status (SSID, IP, MAC, signal, uptime, mDNS address)
     and an editable WiFi SSID/password/hostname. The hostname makes the dashboard
     reachable at `http://<hostname>.local/` instead of the IP (default
     `cameramonitor.local`). Saving writes to NVS immediately but only takes effect
     after the next reboot - a live change could drop the board off the network with
     no way back to this page if the new credentials are wrong.
   - **Cameras** — add/delete/view. Fill in the device service URL, credentials,
     alert cooldown (minimum seconds between Telegram alerts for that camera, default
     30s), and any per-camera quirk flags (the form documents what each one does).
     Adding or deleting writes to NVS immediately but only takes effect after the
     next reboot.
   - **Telegram Users** — add/delete/view recipients. Each one picks specific
     cameras or "all cameras", and independently toggles heartbeat/boot messages and
     command permission. Takes effect immediately, no reboot needed.

   No login is required — anyone on your LAN who can reach the board's IP can view
   and change this, including camera and WiFi credentials. Don't forward port 80 to
   the internet.

## Project layout

```
include/
  config.h          # timing constants (committed) - no longer holds per-camera config
  camera_store.h    # CameraConfig struct + NVS load/save (add/delete/view backing)
  telegram_users.h  # TelegramUser struct + NVS load/save (recipients, per-user permissions)
  network_store.h    # WiFi credentials, NVS load/save (editable from the dashboard)
  webserver.h       # sidebar dashboard - Network + Cameras + Telegram Users (PsychicHttp)
  secrets.h.example # template for secrets.h (copy, fill in, gitignored)
  telegram_ca.h      # Telegram's root CA for TLS pinning (committed, not secret)
  camera.h, telegram.h, onvif_soap.h
src/
  main.cpp          # boot sequence, PSRAM check, WiFi/NTP, per-camera task spawn, heartbeat
  camera.cpp        # ONVIF SOAP calls, event parsing, per-camera FreeRTOS task
  camera_store.cpp   # NVS-backed camera list (load/save/add/delete, one-time seed)
  telegram_users.cpp # NVS-backed Telegram user list (load/save/add/delete, one-time seed)
  network_store.cpp  # NVS-backed WiFi credentials (load/save, one-time seed)
  webserver.cpp      # dashboard HTML pages and form handlers
  telegram.cpp       # photo/message send paths, multi-recipient fan-out, remote commands
  onvif_soap.cpp     # SOAP envelope building, WS-Security digest, XML helpers
```

## Notes on reliability

- A camera's subscription is retried on a fixed interval (`RETRY_INTERVAL_MS` in
  `config.h`) if it drops — this doesn't currently back off, so a camera that's down
  for a long time will be retried at a steady cadence rather than escalating delays.
- The Telegram heartbeat reports liveness but can't detect a fully frozen board on
  its own, since a hung `loop()` can't send anything. If you need actual hang
  recovery, pair it with an ESP32 task watchdog (`esp_task_wdt`) that forces a reboot.
- Event parsing works at the whole-SOAP-response level, not per-message — a batch
  containing both a false `MotionAlarm` and a true `TamperDetector` topic would
  currently trigger on the wrong one. See the comment above `parseEvents()` in
  `src/camera.cpp`.

## License

MIT — see [LICENSE](LICENSE).
