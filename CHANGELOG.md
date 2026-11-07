# Changelog

Notable changes to the firmware. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/). The running firmware reports its
version (from `git describe`) on the dashboard, in Telegram heartbeats and in
config exports - see [Releases](README.md#releases) for how to cut one.

## [Unreleased]

A cleanup release: the code is reorganised and much better tested, and the
firmware behaves the same except where noted below.

### Changed
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
- Native test count: 454 -> 489.

## [1.0.0] - 2026-09-20

Baseline for this changelog. For earlier history, see `git log v1.0.0`.

[Unreleased]: https://github.com/sjfaustino/alarm_onvif_telegram/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/sjfaustino/alarm_onvif_telegram/releases/tag/v1.0.0
