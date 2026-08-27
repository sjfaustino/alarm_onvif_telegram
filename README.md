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
  that camera. Per-camera snapshot burst count (default 1, up to 10) sends that many
  consecutive fresh-fetched photos instead of just one, for when a single frame isn't
  enough to tell why an alert fired.
- TLS to Telegram is certificate-pinned (not `setInsecure()`).
- Per-camera quirks handled via config flags: WS-Security vs. HTTP Basic Auth,
  optional `InitialTerminationTime`/`ReplyTo` (needed by some Xiongmai-derived
  stacks), snapshot URI override, and preferred video profile.
- Periodic Telegram heartbeat (uptime, free heap alongside its lifetime minimum -
  a steady minimum means normal overhead, a minimum that keeps dropping means a
  leak - and per-camera subscription status) so a silently hung, endlessly-retrying,
  or slowly leaking board doesn't go unnoticed.
- Offline camera detection: if a camera goes without answering *any* SOAP request
  for longer than its own offline threshold (per-camera, default 5 minutes), it's
  flagged OFFLINE and an immediate Telegram alert goes out (and another when it
  recovers) - independent of the 6-hour heartbeat, and shown live on the Cameras
  dashboard page.
- Cameras and Telegram recipients are managed at runtime through a built-in
  sidebar dashboard ([hoeken/PsychicHttp](https://github.com/hoeken/PsychicHttp))
  and persisted in NVS - no more editing and reflashing `config.h`/`secrets.h` to
  change either. Cameras can be added, edited, and deleted (edits can rename a
  camera too); a Test Connection button runs a live GetCapabilities/
  GetEventProperties/GetSnapshotUri check against whatever's currently typed in
  the form, before you save or reboot. Camera changes take effect after a
  reboot; Telegram user changes apply immediately.
- Firmware updates over the dashboard: upload a `.bin` built with
  `pio run -e esp32s3` on the Firmware page instead of reflashing over USB. Uses
  the board's dual OTA app partitions - a failed or aborted upload leaves the
  running firmware untouched, and the board only reboots into the new image
  once it's fully received and its checksum verifies.
- Any number of Telegram users, each independently configured for which cameras
  they hear from (specific list or "all, including future ones"), whether they get
  the heartbeat/boot messages, and whether they're allowed to send `/on`, `/off`,
  `/snap`, `/status` commands (which apply per-camera, matched by name or prefix,
  and reply to whoever sent them - `/snap` fetches and sends a fresh photo on
  demand, even from a camera currently muted with `/off`).
- Dashboard login is opt-in but boots disabled: the board comes up with no password
  and a standing banner nagging you to set one, on every page, until you do. Once
  set (Security page), HTTP Basic Auth is required on every dashboard route,
  including the Firmware upload, and takes effect on your very next request - no
  reboot needed.

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
   `default_envs` in `platformio.ini` already picks `esp32s3` if you omit `-e`. Every
   push/PR also builds in CI ([.github/workflows/build.yml](.github/workflows/build.yml))
   and uploads the result as a `firmware-esp32s3` artifact - `firmware.bin` for the
   dashboard's Firmware/OTA page, `firmware.factory.bin` (bootloader+partitions+app
   merged) for a from-scratch `esptool write_flash` onto a blank board - so a ready-to-
   flash build is available from any green run without a local toolchain.

5. **Open the dashboard** at `http://<board's IP>/` (printed in the boot log and the
   Telegram boot message). The sidebar has five sections:
   - **Network** — connection status (SSID, IP, MAC, signal, uptime, mDNS address)
     and editable primary/backup WiFi credentials plus hostname. The hostname makes
     the dashboard reachable at `http://<hostname>.local/` instead of the IP (default
     `cameramonitor.local`). The backup network (optional) is tried if primary
     doesn't connect within 30s; if backup connects, it's promoted to primary (and
     primary demoted to backup) automatically, so future boots try whichever network
     actually works first. Also a DHCP/Static IP toggle - Static shows editable IP
     address/subnet mask/gateway/DNS fields (applies to whichever of primary/backup
     ends up connecting); DHCP shows the board's current live-obtained settings,
     grayed out. Also an editable NTP server + resync interval (default `pool.ntp.org`,
     1 hour) - no port field, since ESP32's SNTP client hardcodes the standard UDP
     port 123 and can't be pointed at a custom one. Saving writes to NVS immediately
     but only takes effect after the
     next reboot - a live change could drop the board off the network with
     no way back to this page if the new credentials are wrong.
   - **Cameras** — add/edit/delete/view, each row showing live subscription status
     (with an OFFLINE flag - see below) and how long ago it last alerted ("never" if
     it hasn't yet). Fill in the device service URL, credentials, alert cooldown
     (minimum seconds between Telegram alerts for that camera, default 30s), offline
     threshold (minutes without a response before it's flagged OFFLINE, default 5),
     snapshots per alert (1-10, default 1 - that many consecutive fresh-fetched photos
     instead of just one, for extra context on why an alert fired), and any per-camera
     quirk flags (the form documents what each one does). A Test
     Connection button runs a live check against whatever's currently in the form
     (GetCapabilities, event service, snapshot URI) without saving anything, so a
     wrong URL/credential shows up before you commit to a reboot. Editing an existing
     camera leaves its password unchanged if you leave that field blank, same as the
     Network page's WiFi password. Adding, editing, or deleting writes to NVS
     immediately but only takes effect after the next reboot.
   - **Telegram Users** — add/edit/delete/view recipients. Each one picks specific
     cameras or "all cameras", and independently toggles heartbeat/boot messages and
     command permission. Takes effect immediately, no reboot needed.
   - **Firmware** — upload a `.bin` (built with `pio run -e esp32s3`) to reflash
     over the network instead of USB. Writes into the currently-inactive OTA app
     partition and only reboots into it once the upload is complete and its
     checksum verifies; a failed or aborted upload leaves the running firmware
     untouched.
   - **Security** — set or change the dashboard's HTTP Basic Auth username/
     password. Empty (the default until you set one) means no login is required
     at all - a red banner saying so shows on every page as a reminder. There's no
     recovery flow if you forget it: getting back in means erasing the board's NVS
     entirely, which also wipes cameras, WiFi, and Telegram users.

   No login is required until you set one on the Security page - until then, anyone
   on your LAN who can reach the board's IP can view and change everything here,
   including camera and WiFi credentials, and can flash arbitrary firmware via the
   Firmware page. Don't forward port 80 to the internet either way.

## Project layout

```
include/
  config.h          # timing constants (committed) - no longer holds per-camera config
  camera_store.h    # CameraConfig struct + NVS load/save (add/delete/view backing)
  telegram_users.h  # TelegramUser struct + NVS load/save (recipients, per-user permissions)
  network_store.h    # WiFi credentials, NVS load/save (editable from the dashboard)
  auth_store.h       # dashboard Basic Auth username/password, NVS load/save
  webserver.h       # sidebar dashboard - Network/Cameras/Users/Firmware/Security (PsychicHttp)
  secrets.h.example # template for secrets.h (copy, fill in, gitignored)
  telegram_ca.h      # Telegram's root CA for TLS pinning (committed, not secret)
  camera.h, telegram.h, onvif_soap.h
src/
  main.cpp          # boot sequence, PSRAM check, WiFi/NTP, per-camera task spawn, heartbeat
  camera.cpp        # ONVIF SOAP calls, event parsing, per-camera FreeRTOS task
  camera_store.cpp   # NVS-backed camera list (load/save/add/delete, one-time seed)
  telegram_users.cpp # NVS-backed Telegram user list (load/save/add/delete, one-time seed)
  network_store.cpp  # NVS-backed WiFi credentials (load/save, one-time seed)
  auth_store.cpp      # NVS-backed dashboard login (load/save)
  webserver.cpp      # dashboard HTML pages and form handlers
  telegram.cpp       # photo/message send paths, multi-recipient fan-out, remote commands
  onvif_soap.cpp     # SOAP envelope building, WS-Security digest, XML helpers
```

## Notes on reliability

- A camera's subscription retry backs off on consecutive failures - starting at
  `RETRY_INTERVAL_MS` (`config.h`) and doubling up to a 5-minute cap
  (`CameraState::retryDelayMs`, `camera.cpp`), resetting the moment a retry
  succeeds - so a camera that's down for a long time doesn't get hammered at a
  steady cadence for the whole outage.
- The Telegram heartbeat reports liveness but can't detect a fully frozen board on
  its own, since a hung `loop()` can't send anything - that's what the ESP32 task
  watchdog (`initWatchdog()` in `main.cpp`) covers: a 90s timeout on `loop()`
  forces a reboot if it stops returning to its top. It only watches `loop()`, not
  the per-camera tasks in `camera.cpp` - those already bound every SOAP call with
  `HTTP_TIMEOUT_MS`, and `cameraSetupSequence` chains several such calls back-to-back
  without a safe point to feed a per-task watchdog without risking a false-positive
  reboot on a merely slow (not hung) camera.
- Event parsing works at the whole-SOAP-response level, not per-message — a batch
  containing both a false `MotionAlarm` and a true `TamperDetector` topic would
  currently trigger on the wrong one. See the comment above `parseEvents()` in
  `src/camera.cpp`.

## License

MIT — see [LICENSE](LICENSE).
