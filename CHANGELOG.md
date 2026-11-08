# Changelog

Notable changes to the firmware. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The running firmware reports its
version (from `git describe`) on the dashboard, in Telegram heartbeats and in
config exports - see [Releases](README.md#releases) for how to cut one.

## [Unreleased]

### Added
- Model defaults: a newly added camera is asked for its manufacturer and model,
  and known settings for it are filled in where the add form was left at the
  default. Entries so far:
  - Vatilon: `/cgi-bin/snapshot.cgi` and 500 ms polls.
  - REOLINK RLC-520A: InitialTerminationTime.
  - Tips in the notes for the Vatilon H46, Tapo TC65 and Vstarcam
    "IP Camera".

### Changed
- Event polls now ask the camera to hold the request for up to 5 seconds
  instead of 1, so while the camera holds them they run back to back. An event used to
  wait up to about a second for the next poll; now it arrives as it happens.
  The poll interval is now the minimum time between polls.
- A camera whose snapshot URL wants Digest login now gets it on the first
  request, using the last challenge, instead of being refused first.
- Each snapshot fetch logs its size, time and login type to the serial log.

## [1.1.0] - 2026-11-07

The code is reorganised and much better tested, and running it on a live board
with 14 cameras turned up the crash, memory, subscription and storage fixes
below.

**Upgrading:** on its first boot, 1.1.0 moves the camera list from NVS to a
LittleFS file and erases the NVS copy. Downgrading to 1.0.0 afterwards would
find no cameras and re-seed them from `secrets.h`, so take a config export
first if you might go back.

### Changed
- `HumanShapeDetect` (person detection on Vatilon cameras) now triggers person
  alerts like `PeopleDetect`.
- Snapshot URLs that answer Basic auth with a Digest challenge now get a Digest
  login.
- A camera with no snapshot address (Tapo cameras offer none over ONVIF) now
  sends its motion alert as text, with the same caption plus "(no photo
  available)". It used to send nothing at all.
- Boot no longer waits 5 seconds for a serial terminal; alerts start sooner after
  every reboot (`BOOT_SETTLE_MS`, 250 ms, leaves time for SD and the RTC).
- A camera task that stops making progress for 10 minutes now triggers a
  Telegram alert, and another when it recovers.
- The firmware version now comes from `git describe` (e.g. `1.1.0`, or
  `1.1.0-3-gabc1234` between releases) instead of a build timestamp, so a
  running board says exactly which commit it was built from.
- The config export's header, the Security page and the import result now
  say that exports contain each camera's username and password (they always
  did - that's how an import restores working cameras) and that WiFi passwords
  are never exported. Previously they claimed no passwords were exported and
  asked for camera passwords to be re-entered after an import.

### Added
- Remote diagnostics, off unless `DEBUG_TOKEN` is set: `/debug/status`, `/debug/log`
  and crash dump download, with `scripts/board.py` as a client. The board also
  records failed allocations and saves a heap-low record that survives reboots.

### Fixed
- Snapshots from cameras that require Digest auth still failed with HTTP 401: the Digest retry never sent its header, because
  HTTPClient drops an added Authorization header while Basic credentials
  are set.
- A camera list save that failed part-way (NVS nearly full) left old and new
  pieces mixed, which corrupted one camera's record so that camera vanished.
  The list now lives in a LittleFS file on the unused 1.5 MB data
  partition, replaced only once the new copy is fully written, and moves
  there from NVS on the first boot. A failed save changes nothing and says
  so, instead of "a camera with that name already exists".
- Editing a camera whose task had stopped (no username/password) said
  "applying live" but changed nothing until a reboot. The save now restarts
  it, and the table shows such a camera as STOPPED, and a just-added one as
  STARTING rather than "reboot to apply".
- With 8 cameras enabled, free internal RAM fell to 6.7 KB during a Telegram
  photo send. Camera task stacks (10 KB each) now live in PSRAM, leaving internal
  RAM for TLS. The Telegram user list is kept in RAM so those tasks never read
  flash.
- A camera whose poll connections kept dropping stayed "subscribed" and
  delivered nothing, polling the dead subscription forever. Five polls in a row
  with no usable reply now trigger a resubscribe, with backoff.
- A crash (lwIP assert "Required to lock TCPIP core functionality", caught in a
  core dump from the live board) when an NTP lookup was pending as the board's
  IP state changed. SNTP is now only given an already-resolved address, retried
  every 5 minutes if the lookup fails or syncing stops.
- A camera that accepted a subscription but refused every poll stayed marked
  subscribed while delivering no events, if its fault mentioned
  `PullMessagesResponse` in a header. It now resubscribes with backoff, and raises
  the "not subscribed" alert if polls keep failing.
- A huge outage threshold or pulse duration on the Hardware pages wrapped to a
  tiny value instead of being capped.

### Internal
- `telegram.cpp` (2,500 lines) split into transport, snapshot fetch, alerts
  and commands; `main.cpp` (1,300 lines) split into camera tasks, WiFi
  connection, time sync, boot checks and health checks. Config export/import
  moved out of the web layer into `config_backup.cpp`.
- Comments trimmed from 35% to 15% of source lines, keeping the reasons
  (incidents, races, limits) and dropping narration. Stale comments that
  contradicted the code were corrected.
- The camera and Telegram user forms moved into natively tested libraries
  (`lib/camera_form`, `lib/user_form`). The camera form is now one field table
  that drives both the HTML and the parsing/clamping; both forms are built
  from shared input helpers in `lib/webserver_html`. Tests submit the rendered
  HTML back through the parser, the way a browser would.
- Hardware page parsing and validation (`lib/hardware_form`) and the limits applied
  to stored camera settings (`lib/camera_clamps`) moved where tests can reach them.
- Library versions and the CI's PlatformIO version are pinned; CI also runs weekly.
- `startWebServer` (750 lines) split: each dashboard page now registers its own
  routes. Form labels and pin fallbacks now come from `config.h` constants.
- `scripts/check_comment_only.sh` proves a change touched only comments.
- Native test count: 454 -> 565.

## [1.0.0] - 2026-09-20

Baseline for this changelog. For earlier history, see `git log v1.0.0`.

[Unreleased]: https://github.com/sjfaustino/alarm_onvif_telegram/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/sjfaustino/alarm_onvif_telegram/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/sjfaustino/alarm_onvif_telegram/releases/tag/v1.0.0
