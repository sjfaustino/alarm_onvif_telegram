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
- Sends a Telegram alert on motion, tamper, and video signal-loss events, per-camera
  cooldown (one shared budget across all three) to avoid spam. Motion alerts include
  a photo, JPEG buffered once in PSRAM and resent to every Telegram user subscribed
  to that camera, with a per-camera snapshot burst count (default 1, up to 10) that
  sends that many consecutive fresh-fetched photos instead of just one, for when a
  single frame isn't enough to tell why an alert fired. Tamper alerts try for a
  single photo too (falling back to text-only if one isn't available); signal-loss
  is always text-only, since by definition there's no usable video feed at that
  moment. Any ONVIF event topic this project doesn't otherwise recognize (there's no
  single standardized topic name for person/vehicle detection across vendors) is
  logged - to Serial and the Activity page - instead of silently dropped, so support
  for it can be added deliberately once you know what your camera actually sends.
  Motion that keeps firing while the cooldown from the last photo is still running
  doesn't just vanish - each suppressed event is counted, and once the cooldown
  ends a single follow-up text reports how many more happened and over how many
  seconds (measured to the last one actually suppressed, not the cooldown length
  itself), so sustained motion is still visible without a full photo alert per event.
- Per-camera recurring daily quiet hours (Cameras page edit form) mute motion
  Telegram alerts during a configured window - tamper and signal-loss alerts stay
  always-on regardless, since those are security/connectivity-relevant, not
  "noise". Motion during the window is still detected, cooldown-gated, and
  recorded (Activity log entry + snapshot history), just not sent. Leaving both
  the start and end time at the default `00:00` means no active window (quiet
  hours needs a real, non-zero-width window to do anything) - the opposite
  default would let checking the enable box alone silently and permanently kill
  every motion alert for that camera. Falls back to sending alerts normally if
  the board's clock hasn't synced yet, rather than risk misjudging the window
  against a near-epoch time.
- Per-camera "no motion" watchdog (hours, 0 = off, max 168h/1 week) alerts if a
  camera hasn't seen *any* real motion in over that long - catches a dead PIR or
  a camera knocked to face the wrong way, which otherwise looks identical to a
  quiet day. Re-arms automatically once motion resumes, so it only fires once
  per stretch of silence. The dashboard warns if this is set shorter than the
  camera's own quiet-hours window (above) - motion still resets this watchdog's
  clock during quiet hours (only the Telegram *send* is suppressed), so a short
  watchdog window would otherwise fire a false alert every quiet period.
- Per-camera timelapse capture (minutes, 0 = off, max 1440min/24h) stores a
  fresh snapshot on its own interval regardless of motion - kept in whichever
  snapshot-history store is active (see below), never sent to Telegram. Useful
  for confirming a camera's
  still alive between motion events, or building a day timelapse from the SD
  history.
- TLS to Telegram is certificate-pinned (not `setInsecure()`).
- Per-camera quirks handled via config flags: WS-Security vs. HTTP Basic Auth,
  optional `InitialTerminationTime`/`ReplyTo` (needed by some Xiongmai-derived
  stacks), snapshot URI override, and preferred video profile.
- Periodic Telegram heartbeat (uptime, free heap alongside its lifetime minimum -
  a steady minimum means normal overhead, a minimum that keeps dropping means a
  leak - and per-camera subscription status) so a silently hung, endlessly-retrying,
  or slowly leaking board doesn't go unnoticed.
- That lifetime-minimum free heap number gets a timestamped trail, not just a
  bare figure: every time it drops to a new record low (checked every `loop()`
  tick - cheap, `ESP.getMinFreeHeap()` is a running watermark the IDF already
  tracks on its own), an Activity log entry is written, so a slow leak or a
  sudden allocation burst (a multi-camera TLS spike, say) can be correlated
  against whatever else was happening around the same time. Also sends a
  one-time Telegram alert the first time a new low crosses a genuinely
  concerning threshold (20KB free internal RAM, `HEAP_LOW_WARN_BYTES` -
  `WiFiClientSecure`/mbedTLS allocate from this same pool) - real allocation-
  failure territory, not a sanity nicety.
- Offline camera detection: if a camera goes without answering *any* SOAP request
  for longer than its own offline threshold (per-camera, default 5 minutes), it's
  flagged OFFLINE and an immediate Telegram alert goes out (and another when it
  recovers) - independent of the 6-hour heartbeat, and shown live on the Cameras
  dashboard page. The Cameras page also shows how many times each camera's
  subscription has had to be re-established since boot ("N reconnect(s) since
  boot", omitted when zero) - a camera that flaps (drops and reconnects
  repeatedly but is back up by the time you happen to look) reads identically to
  a rock-solid one in the live subscribed/OFFLINE status alone, so this is the
  one place that history is actually visible instead of silently resetting on
  every successful reconnect.
- Stuck-subscription detection: OFFLINE (above) only catches a camera that stops
  answering entirely - but a camera that answers every ONVIF call with a fault
  (wrong credentials after a camera-side password change, a firmware update that
  broke eventing, a WS-Security mismatch) keeps counting as "contact" and never
  goes OFFLINE, while never actually holding a subscription either, so it can't
  report a single motion/tamper/signal-loss event. A separate alert catches this:
  if a camera has been responding but unable to hold a subscription for longer
  than its own offline threshold, it fires once (re-arming once it subscribes
  again), so this failure mode isn't silently invisible until you happen to
  notice "NOT subscribed" in a heartbeat.
- A "Test all cameras" button on the Cameras page checks reachability and
  ONVIF event-service response (GetCapabilities/GetEventProperties - not a
  full subscription test, to avoid disrupting the real subscription each
  enabled camera's own monitoring task already holds) once per already-saved
  *enabled* camera (not whatever's currently typed into the Add/Edit form) - a
  quick way to see which cameras, if any, broke after a network change, without
  clicking through each one by hand. Runs on a background task, so the rest of
  the dashboard stays responsive while it works - reload the page to see
  results once ready.
- A "Search network for cameras" button on the Cameras page sends a
  WS-Discovery multicast probe on the local network segment and lists what
  answers, like an NVR's own camera search - click Add next to a result to
  prefill the Add-camera form below with its device service URL and a
  best-effort name (parsed from the reply's Scopes, if the camera includes
  one). WS-Discovery never carries credentials, so username/password still
  need to be typed in by hand. Also runs on a background task (the listen
  window is a few seconds by design, to give slower cameras time to answer).
  Multicast discovery only reaches devices on the same network segment as the
  board - a camera on a different VLAN/subnet, or one that doesn't support
  WS-Discovery at all, won't show up here even if it's reachable directly by
  URL; add those manually instead.
- Cameras and Telegram recipients are managed at runtime through a built-in
  sidebar dashboard ([hoeken/PsychicHttp](https://github.com/hoeken/PsychicHttp))
  and persisted in NVS - no more editing and reflashing `config.h`/`secrets.h` to
  change either. Cameras can be added, edited, and deleted (edits can rename a
  camera too); a Test Connection button runs a live GetCapabilities/
  GetEventProperties/GetSnapshotUri check against whatever's currently typed in
  the form, before you save or reboot. Telegram user changes always apply
  immediately. Camera changes apply immediately too when the edited camera was
  already running and stays enabled (the task reconnects with the new settings
  within about a tick of saving) or when the edit enables a previously-disabled
  camera (its task starts live); a brand new camera, or disabling one that's
  currently running, still needs a reboot - the dashboard tells you which
  happened after you save. The Cameras page also shows a small strip of the most
  recent snapshots per camera (motion, tamper, or an on-demand `/snap`), backed
  by whichever of the two snapshot-history stores below is active.
- Snapshot history is stored either in a small PSRAM ring (last 5 per camera,
  lost on reboot - the default, no extra hardware needed) or, if you add an
  optional SD card (System > Storage page), on the card instead - far more
  history, and it survives a reboot. Entirely opt-in and absence-tolerant: off
  by default, and even if enabled, a missing/undetected module or card at boot
  just falls back to the PSRAM ring with a clear status message - camera
  monitoring itself is never affected either way. SD storage is per-camera
  directory, capped both by a free-space reserve (prunes that camera's own
  oldest files first, adaptive to whatever card size is actually inserted -
  the same "check real free resources, don't guess a fixed number" approach
  the PSRAM ring uses) and by a per-camera file-count ceiling (so one chatty
  camera can't crowd out a quiet camera's history on the same card). The
  Storage page also has a "check storage" pass (confirms every stored file is
  still readable - not a full filesystem check, this project's SD support has
  no fsck/chkdsk equivalent) and an "erase all snapshot history" action
  (deletes only what this project itself wrote, not a low-level card format).
  Any unreadable file found by that pass sends a Telegram alert (to every
  systemMessages-subscribed user) and logs an Activity page entry, same as a
  runtime SD write/read failure does. The same full check can also run on a
  schedule instead of only on demand - the Storage page's "Automatic full
  storage check every N hour(s)" field (0 = off, the default) - since its
  cost scales with total history stored (not bounded like the boot check
  below), it isn't turned on by default, and a large history can briefly
  delay a camera's own SD write while it runs; set it (e.g. daily) only if
  you want that tradeoff. The interval takes effect immediately on save, no
  reboot needed. A lighter version of the same readability check - only each
  camera's *newest* file, not the whole history - runs automatically once at
  boot, right after mounting, regardless of the scheduled-check setting:
  bounded cost regardless of how much history is stored, and it targets the
  specific failure mode a reboot interrupted mid-write would actually
  produce. That boot-time check can't send its own Telegram alert (it runs
  before WiFi connects), so any problem it finds instead gets folded into
  the existing boot notification once WiFi is up. A deliberate reboot
  (`/reset`, the Maintenance page, or a firmware update) now also waits for
  any in-flight SD write to finish first - `ESP.restart()` doesn't wait for
  other tasks on its own, and FAT isn't a journaling filesystem, so cutting
  a write off mid-flight can leave more than just that one file.
  Retaining every sent snapshot at all (in either store) is a real, ongoing
  memory/storage cost that scales with camera count and snapshot size, unlike
  the very first version of this feature (where a sent snapshot was freed
  immediately after upload) - the PSRAM ring checks actual free PSRAM before
  keeping each one and just stops retaining new snapshots if that would leave
  less than one more full-size fetch's worth free, same adaptive idea SD
  storage's own pruning uses.
- A small in-memory Activity log (Activity page) - the most recent ~40 events
  (motion alerts, offline/online transitions, on/off changes including timed
  ones, live config reloads, boot) with a relative timestamp, for a quick "what
  happened recently" view without a serial cable. That in-memory view resets on
  reboot, same as the rest of this board's runtime state - but when SD storage is
  active, every event is also appended (with a real timestamp, not just uptime)
  to `/activity.log` on the card, capped at 64KB (wipes and starts fresh once
  exceeded, same bounded-cost approach the snapshot pruning uses), downloadable
  in full from the Activity page.
- A "Gallery" page for browsing a camera's stored snapshot history beyond the
  Cameras page's 5-entry Preview strip - most useful with SD storage active
  (far more history than the PSRAM ring holds), showing up to 30 thumbnails per
  camera per page load.
- The running build's exact version - "YYYYMMDD.HHMM" (year-first so two
  versions sort correctly), the real wall-clock time it was built, computed
  fresh by `scripts/generate_build_version.py` on every `pio run`/upload,
  not something anyone has to remember to bump by hand -
  is shown alongside every "Camera Monitor" label: the dashboard title/
  sidebar/login prompt, the Telegram heartbeat and boot-online messages, the
  Firmware page (its own "Version" row, next to the existing build date/time),
  and the config export header. Useful for confirming which exact build is
  actually running, especially after an OTA update.
- Firmware, Maintenance, and Storage live under a "System" submenu in the sidebar.
  Firmware updates over the dashboard: upload a `.bin` built with
  `pio run -e esp32s3` on the Firmware page instead of reflashing over USB. Uses
  the board's dual OTA app partitions - a failed or aborted upload leaves the
  running firmware untouched, and the board only reboots into the new image
  once it's fully received and its checksum verifies. That only catches a
  corrupted transfer, though - if a checksum-valid image itself crashes or
  hangs on boot, ESP-IDF's app rollback (enabled in this board's sdkconfig)
  automatically reverts to the previous working partition on the next reset;
  `main.cpp`'s `setup()` confirms the new image healthy near the end of its
  own run, not gated on WiFi connecting (a network outage during an update
  shouldn't roll back otherwise-good firmware). The Firmware page also shows
  NVS usage (% of entries used) - this project has silently hit NVS's practical
  size ceiling before (see `camera_store.cpp`'s history), so a visible warning
  above 80% is meant to catch that before it happens again, not after. The
  Maintenance page has a manual reboot button (confirmation popup first) for
  when you just want the board to restart without a firmware change.
- Any number of Telegram users, each independently configured for which cameras
  they hear from (specific list or "all, including future ones"), whether they get
  the heartbeat/boot messages, and three independent command permissions:
  `canCommand` (`/on`, `/off`, `/status` - toggle/view per-camera alerts),
  `canSnap` (`/snap` - fetch and send a fresh photo on demand, even from a
  camera currently muted with `/off`), and `canReset` (`/reset` - reboot the
  board immediately) - a user can have any combination, since pulling a live
  photo, silencing alerts, and rebooting the board are three different kinds
  of trust. `canReset` is **not** granted to the auto-seeded Admin user by
  default, even though `canCommand`/`canSnap` are - it has to be turned on
  deliberately from the dashboard, since it's disruptive rather than just
  informational/control. Each user's Chat ID must be unique too, not just
  their display name - two users sharing one Chat ID would double-send every
  alert to that physical account and make command-permission resolution
  non-deterministic, so the dashboard rejects a save that would create one.
  Commands are matched by camera name or
  prefix - or the literal word "all" in place of a name, which applies to every
  enabled camera at once (`/off all 30` mutes everything for 30 minutes; `/snap
  all` fetches a fresh photo from every camera in one go) - and reply to whoever
  sent them. `/on`/`/off` accept an optional trailing timer - a number of minutes,
  max 20160 (14 days - see `MAX_DURATION_MINUTES`'s own comment for why: past
  that, the millis()-wraparound due-check this timer relies on stops being
  reliable) (`/off D01 30`), or a 24h clock time (`/off D01 23:00`, tomorrow if
  that time has already passed today) - after which the camera automatically
  reverts to the opposite state; omitted entirely is permanent, as before.
  `/status` reports each
  camera's ON/OFF state plus OFFLINE and any pending timer. `/health` reports
  board uptime, free heap (current and worst-case-ever), free PSRAM, NVS usage,
  WiFi signal strength, and SD storage status in one message - a quick "is
  the board OK" check without opening the dashboard. `/log [N]` replies with the
  N most recent Activity log entries (default 10, capped at 40) - a quick "what
  happened recently" check without opening the dashboard. `/on`, `/off`, or
  `/snap` sent with no camera name at all shows a tappable inline-keyboard
  picker instead (one button per enabled camera plus "All") - handy from a
  phone when typing/remembering an exact camera name is more friction than
  tapping a button; permanent on/off/snap only, no duration timer via buttons.
  `/help` replies with the full command list plus the sender's own
  `canCommand`/`canSnap`/`canReset` permissions, so the syntax doesn't have to
  be looked up here every time.
- Dashboard login is opt-in but boots disabled: the board comes up with no password
  and a standing banner nagging you to set one, on every page, until you do. Once
  set (Security page), HTTP Basic Auth is required on every dashboard route,
  including the Firmware upload, and takes effect on your very next request - no
  reboot needed. Login attempts are rate-limited per source IP (5 consecutive
  failures locks that IP out, starting at 30s and doubling on repeat offenses up
  to 30 minutes) - HTTP Basic Auth has no throttling of its own, so without this
  a wrong-password guess would otherwise cost an attacker nothing. The Security
  page also has a config export/backup - a plain-text download of every camera,
  Telegram user, and network setting (no passwords - those still have to be
  re-entered by hand), for reconstructing the tedious parts of the configuration
  if NVS is ever erased or a board gets replaced. A matching Import restores from
  a previously exported file - a real file upload (not a size-limited form field,
  so it scales to as many cameras/users as you actually have), REPLACING whichever
  of the four sections (cameras, Telegram users, network, SD settings) the file
  actually contains; a section missing from the file is left untouched. A file
  with two cameras or two Telegram users sharing a name (or two users sharing a
  Chat ID) is rejected outright for that section rather than importing an
  ambiguous pair - the same rule the Add-camera/Add-user forms already enforce
  one at a time. Every import automatically saves a one-slot backup of whatever
  was stored just before it, downloadable from the same page, so importing the
  wrong file is undoable by importing that backup back. Imported cameras/network
  always have blank passwords (never in an export) - re-enter them before
  rebooting, since network settings missing the WiFi password will otherwise
  strand the board off the network entirely. Takes effect after a reboot, same
  as any other bulk camera/network change.

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

**A microSD card is optional.** A generic SPI breakout module (SCK/MISO/MOSI/CS +
power) wired to the pins in `include/config.h` (`SD_CS_PIN`/`SD_SCK_PIN`/
`SD_MISO_PIN`/`SD_MOSI_PIN` — **verify and adjust these for your actual wiring
before flashing**; they're common ESP32-S3 SPI2 defaults, not guaranteed for your
specific board) and enabled on the System > Storage dashboard page turns on
larger, reboot-persistent snapshot history (see Features above). Nothing else in
this project uses SPI. Entirely optional - everything works exactly as it always
has, on the PSRAM-only ring, without one.

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
     port 123 and can't be pointed at a custom one. And an optional POSIX TZ string
     (e.g. `WET0WEST,M3.5.0/1,M10.5.0` for mainland Portugal - look yours up at
     [nayarsystems/posix_tz_db](https://github.com/nayarsystems/posix_tz_db)) for
     local time in Telegram alert photo captions, DST included automatically since
     the rule carries its own DST dates - the board's system clock itself always
     stays true UTC regardless (ONVIF's WS-Security timestamps require it; see
     `isoTimeNow()` in `src/onvif_soap.cpp`, which reads UTC directly and ignores
     this setting entirely). Leave it blank to keep captions in UTC. Saving writes to
     NVS immediately but only takes effect after the
     next reboot - a live change could drop the board off the network with
     no way back to this page if the new credentials are wrong. A "Search WiFi
     networks" button scans for nearby networks and lists what's found (signal
     strength, open/encrypted) with an Add link per network that fills in the
     Primary SSID field below (the password still needs to be typed by hand).
     Runs on a background task, same as the Cameras page's search/test-all
     buttons - the scan briefly pauses the board's own WiFi traffic while it
     hops channels, so the dashboard may stall for a moment around when it runs.
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
     cameras or "all cameras", and independently toggles heartbeat/boot messages,
     `/on`/`/off`/`/status` permission, and `/snap` permission (separately, since
     they're different kinds of trust). Takes effect immediately, no reboot needed.
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
  event_log_store.h  # thread-safe global wrapper around lib/event_log's ring buffer
  camera_tasks.h      # spawnCameraTask() - exposed from main.cpp so a dashboard edit can
                       # start a camera's task live, without a reboot
  sd_store.h          # optional SD card mechanics (settings, mount, write/list/read/prune,
                       # erase-all, readability check) - thread-safe, hardware-dependent
  snapshot_history.h  # picks SD (sd_store.h) vs the PSRAM ring (camera.h) per camera - the
                       # one place that decision is made
  background_job.h    # BackgroundJob<T> - mutex-guarded start/finish/status wrapper shared by
                       # every "click a button, run on a task, poll for the result" dashboard
                       # action (Test all cameras, Search network, WiFi scan, Test Connection,
                       # send test message, storage check/erase)
  webserver.h       # sidebar dashboard - Network/Cameras/Users/Activity/Gallery/System (Firmware,
                     # Maintenance, Storage)/Security (PsychicHttp)
  webserver_network.h, webserver_cameras.h, webserver_users.h, webserver_activity.h,
  webserver_gallery.h, webserver_firmware.h, webserver_maintenance.h, webserver_storage.h,
  webserver_security.h
                     # each panel's own rendering/form-handling
  secrets.h.example # template for secrets.h (copy, fill in, gitignored)
  telegram_ca.h      # Telegram's root CA for TLS pinning (committed, not secret)
  build_version.h    # extern FIRMWARE_VERSION - its own translation unit (build_version.cpp)
                      # specifically so the build-timestamp value that changes on every single
                      # build doesn't force a full rebuild of everything that includes config.h
  generated_build_version.h # created/rewritten fresh before every build (scripts/generate_build_version.py,
                             # a pre: extra_script - runs before any compilation) - gitignored, not
                             # tracked at all; nothing to keep in sync since a build always creates it first
  camera.h, telegram.h, onvif_soap.h
src/
  main.cpp          # boot sequence, PSRAM check, WiFi/NTP, per-camera task spawn, heartbeat,
                      # reboot-reason reporting, OTA rollback confirmation
  camera.cpp        # ONVIF SOAP calls, event parsing, per-camera FreeRTOS task, live config reload
  camera_store.cpp   # NVS-backed camera list (load/save/add/delete, one-time seed)
  telegram_users.cpp # NVS-backed Telegram user list (load/save/add/delete, one-time seed)
  network_store.cpp  # NVS-backed WiFi credentials (load/save, one-time seed)
  auth_store.cpp      # NVS-backed dashboard login (load/save)
  event_log_store.cpp # thread-safe global event log (FreeRTOS mutex + lib/event_log's ring buffer)
  sd_store.cpp        # SD.h/SPI.h usage lives only here - mount/write/list/read/prune/erase/check
  snapshot_history.cpp # SD-vs-PSRAM-ring dispatch; also owns the PSRAM ring's own logic
                        # (moved out of telegram.cpp when SD support was added)
  webserver.cpp      # routing table, dashboard shell, OTA upload state, login rate-limiting
                       # middleware - see webserver_*.cpp for panels
  webserver_network.cpp, webserver_cameras.cpp, webserver_users.cpp, webserver_activity.cpp,
  webserver_gallery.cpp, webserver_firmware.cpp, webserver_maintenance.cpp, webserver_storage.cpp,
  webserver_security.cpp
                     # each panel's rendering/form-handling, split out of what used to be one
                     # 946-line webserver.cpp
  telegram.cpp       # photo/message send paths, multi-recipient fan-out, remote commands
                       # (including timed /on//off), scheduled-revert checking
  onvif_soap.cpp     # SOAP envelope building, WS-Security digest
  build_version.cpp  # defines FIRMWARE_VERSION from generated_build_version.h - see build_version.h
lib/                 # pure-logic modules with no hardware dependencies, split out of the
                      # files above specifically so they're unit-testable - see test/README.md
  xml_helpers/            # ONVIF response substring parsing + XML escaping
  camera_serialize/       # CameraConfig <-> NVS blob (de)serialization, schema-versioned
  telegram_user_serialize/ # TelegramUser <-> NVS blob (de)serialization, schema-versioned
  telegram_parse/         # Telegram JSON escaping, /on,/off,/snap camera-name matching, and
                           # /on,/off timer-token parsing (minutes or HH:MM)
  telegram_multipart/     # multipart/form-data body construction for Telegram's sendPhoto
  network_serialize/      # WifiCredentials <-> NVS blob (de)serialization, schema-versioned -
                           # same shape as camera_serialize/telegram_user_serialize, passwords
                           # never included
  config_import_parse/    # parses the Security page's exported config text back into
                           # CameraConfig/TelegramUser/WifiCredentials/SdSettings for Import
  background_job_state/   # pure start/finish/failed-to-start transition rules behind
                           # BackgroundJob<T> (include/background_job.h)
  subscription_health/    # alert-once/re-arm decision logic behind the stuck-subscription
                           # alert (telegram.cpp's checkSubscriptionHealth)
  heap_health/             # new-record-low/threshold-alert decision logic behind the free-heap
                           # trail (main.cpp's checkHeapHealth)
  backoff/                # the doubling-with-a-cap retry delay formula (shared by main.cpp,
                           # camera.cpp, and webserver.cpp's login rate-limiter)
  quiet_hours/              # recurring daily do-not-disturb window predicate (start/end minute
                             # of day, handles the overnight-wraparound case)
  event_log/               # fixed-capacity ring buffer backing the Activity page
  snapshot_storage/         # SD directory-name collision avoidance + prune-decision logic for
                             # sd_store.cpp - the parts of SD support worth unit testing without
                             # real hardware
  nvs_chunk/                # splits/reassembles a long NVS string value across several smaller
                             # keys, byte-granular (not record-aware) - see camera_store.cpp's
                             # comment for the field-truncation bug this exists to prevent
  format_utils/             # formatUptime/formatElapsedSince, htmlEscape, urlEncode
  camera_parse/             # ONVIF GetProfiles/NotificationMessage parsing - motion/tamper/
                             # signal-loss classification
  webserver_html/           # shared Edit/Delete row-actions HTML fragment
test/                 # native unit tests for lib/* - `pio test -e native`, no hardware needed
```

## Testing

```sh
pio test -e native
```

Runs the unit tests in `test/` against the pure-logic modules in `lib/` -
compiles and executes on your own machine (or CI), no ESP32 board involved.
This does **not** exercise ONVIF/Telegram network calls, NVS itself, the web
server, or any FreeRTOS/task behavior - see `test/README.md` for exactly
what is and isn't covered, and why. CI runs this on every push/PR
alongside the firmware build.

## Notes on reliability

- A camera's subscription retry backs off on consecutive failures - starting at
  `RETRY_INTERVAL_MS` (`config.h`) and doubling up to a cap
  (`CameraState::retryDelayMs`, `camera.cpp`), resetting the moment a retry
  succeeds - so a camera that's down for a long time doesn't get hammered at a
  steady cadence for the whole outage. That cap is never a flat 5 minutes - it's
  half this camera's own `offlineThresholdMs`, clamped to `[RETRY_INTERVAL_MS,
  300000ms]` (`detectorSafeBackoffCapMs`, `lib/backoff`) - so with the default
  5-minute offline threshold the real cap is 2.5 minutes. Deliberately not a
  flat cap: a retry backoff that drifted slower than its own offline-alert
  threshold once caused a real false OFFLINE alert in the field (a camera still
  answering, just on a slower retry cadence than the alert threshold expected) -
  see that function's own comment for the full story.
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
- Every background-task button (Test all cameras, Search network, WiFi scan, Test
  Connection, send test message, storage check/erase) checks whether its
  underlying FreeRTOS task actually started before telling you it did - genuine
  memory pressure can make task creation itself fail, and the dashboard now says
  so (and rolls the button back to retryable) instead of leaving it silently
  stuck as "running" forever or claiming success it never achieved. The two
  routes that trigger a reboot (a firmware update, and the Maintenance page's
  reboot button) log clearly to Serial on the same kind of failure, since by
  then the page has already told you a reboot is coming and there's no way to
  correct that response after the fact.

## License

MIT — see [LICENSE](LICENSE).
