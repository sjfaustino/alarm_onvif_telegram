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
- The firmware version now comes from `git describe` (e.g. `1.1.0`, or
  `1.1.0-3-gabc1234` between releases) instead of a build timestamp, so a
  running board says exactly which commit it was built from.
- The config export's header, the Security page and the import result now
  say that exports contain each camera's username and password (they always
  did - that's how an import restores working cameras) and that WiFi passwords
  are never exported. Previously they claimed no passwords were exported and
  asked for camera passwords to be re-entered after an import.

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
  from shared input helpers in `lib/webserver_html`. Golden tests pin their
  exact HTML.
- `scripts/check_comment_only.sh` proves a change touched only comments.
- Native test count: 454 -> 489.

## [1.0.0] - 2026-09-20

Baseline for this changelog. For earlier history, see `git log v1.0.0`.

[Unreleased]: https://github.com/sjfaustino/alarm_onvif_telegram/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/sjfaustino/alarm_onvif_telegram/releases/tag/v1.0.0
