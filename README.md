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
  spam, with the JPEG buffered in PSRAM or streamed through internal RAM depending
  on what the board has.
- TLS to Telegram is certificate-pinned (not `setInsecure()`).
- Per-camera quirks handled via config flags: WS-Security vs. HTTP Basic Auth,
  optional `InitialTerminationTime`/`ReplyTo` (needed by some Xiongmai-derived
  stacks), snapshot URI override, and preferred video profile.
- Periodic Telegram heartbeat (uptime, free heap, per-camera subscription status)
  so a silently hung or endlessly-retrying board doesn't go unnoticed.
- Credentials are kept out of the committed source (`secrets.h`, gitignored) and
  matched to cameras by name, not by array position.
- Remote mute/unmute per camera via Telegram commands (`/on <name>`, `/off <name>`,
  `/status`), restricted to `TELEGRAM_CHAT_ID` and persisted across reboots in NVS.
  A muted camera keeps polling ONVIF and its subscription stays alive - only the
  Telegram photo send is suppressed.

## Hardware

Tested on:
- Classic dual-core ESP32 (no PSRAM) — `[env:esp32dev]`
- ESP32-S3 with 8MB embedded octal PSRAM — `[env:esp32s3]`

Both PSRAM presence and core count are detected/handled at runtime (see the
comments in `include/config.h` and `src/telegram.cpp`), so the only per-board
difference is the PlatformIO build environment you flash. A single-core board
(e.g. ESP32-C3) is **not currently supported** — `src/main.cpp` pins one FreeRTOS
task per camera to core 1, which requires a second core.

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
     to get your chat ID.
   - `CAMERA_SECRETS[]` — one `{ name, user, pass }` entry per camera. **`name` must
     exactly match** that camera's `name` in `include/config.h`'s `CAMERAS[]` — that's
     how credentials get matched up at boot, not the order of the array. A mismatch
     is logged loudly (and reported over Telegram) rather than silently sending the
     wrong password.

3. **Fill in `include/telegram_ca.h`** with Telegram's current root CA — instructions
   are in that file's comments (`openssl s_client` one-liner or browser cert export).
   Root CAs don't rotate often, so this is a one-time setup step, not a per-build one.
   The boot log warns you plainly if this is still the placeholder.

4. **Configure your cameras** in `include/config.h`'s `CAMERAS[]`. Each field is
   documented in the comment block above the array — the ones you're most likely to
   need to flip are `useWSSecurity`, `includeInitialTerminationTime`/
   `includeReplyToAnonymous`, and `snapshotUriOverride` if `GetSnapshotUri` comes back
   with a broken address for your camera.

5. **Build and upload:**
   ```sh
   pio run -e esp32s3 -t upload    # or -e esp32dev
   pio device monitor -b 115200
   ```
   `default_envs` in `platformio.ini` picks `esp32s3` if you omit `-e`.

## Project layout

```
include/
  config.h          # timing constants + per-camera CAMERAS[] (committed)
  secrets.h.example # template for secrets.h (copy, fill in, gitignored)
  telegram_ca.h      # Telegram's root CA for TLS pinning (committed, not secret)
  camera.h, telegram.h, onvif_soap.h
src/
  main.cpp          # boot sequence, WiFi/NTP, per-camera task spawn, heartbeat
  camera.cpp        # ONVIF SOAP calls, event parsing, per-camera FreeRTOS task
  telegram.cpp       # photo/message send paths (buffered vs. streamed)
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
