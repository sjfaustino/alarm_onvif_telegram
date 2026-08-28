#pragma once
#include <Arduino.h>
#include "camera_store.h" // CameraConfig

// Optional SD-backed snapshot history - thread-safe global wrapper around
// the SD/SPI mechanics (config.h's SD_* pin/tuning constants), entirely
// separate from whether/how the PSRAM ring fallback works (that stays
// exactly as it already is - see snapshot_history.h, which is the module
// that actually decides which of the two backs a given camera's history
// and falls back to the ring when this one isn't active). This module
// knows nothing about the ring at all.

// Persisted opt-in toggle - off by default, so the board boots exactly as
// it always has (PSRAM ring only) until someone opts in via the dashboard
// Storage page and reboots. There's no live enable/disable: SD.begin()/
// end() while a camera task might be mid-write is real SPI-bus-ownership
// churn nothing else in this project does today, and these modules aren't
// reliably hot-swappable anyway - so toggling this just updates the
// persisted preference, applied on the next boot.
struct SdSettings {
  bool enabled = false;
  // Hours between automatic full storage checks (checkSnapshotStorage(),
  // run from main.cpp's loop()) - 0 disables the automatic run entirely.
  // Off by default: unlike the bounded boot-time check, a full walk's cost
  // scales with total history stored, so an unattended default could
  // surprise someone with an unexpectedly slow/blocking check on a large
  // card (see checkSnapshotStorage()'s own comment on mutex hold time).
  // Unlike `enabled`, this takes effect immediately on save - it only
  // gates a millis() comparison in loop(), no SPI bus involved.
  uint32_t checkIntervalHours = 0;
};
SdSettings loadSdSettings();
bool saveSdSettings(const SdSettings& settings);

// Attempts to mount the SD card ONLY if the persisted setting is enabled -
// call once from main.cpp's setup(), after the PSRAM gate. Doesn't touch
// the SPI bus/GPIOs at all if disabled. Logs which of three outcomes
// happened (disabled / enabled but no module or card responded / mounted
// successfully) - the middle case falls back to the PSRAM ring exactly
// like the first, just with a different reason logged for it.
void initSdStorage();

// True only if SD support is enabled AND currently believed usable - not
// just "was mounted at boot". Any SD I/O failure after boot (card
// removed, contact issue, corruption) flips this to false for the rest of
// the session: a card degrading mid-session falls back to the PSRAM ring
// from that point on instead of silently dropping every future snapshot.
// No automatic recovery/retry - re-checked only on the next reboot, since
// these modules aren't reliably hot-swappable without a card-detect pin.
bool sdActive();

// Status for the dashboard Storage page - reflects whatever
// initSdStorage()/subsequent I/O has determined so far; doesn't re-probe
// hardware itself.
struct SdStatus {
  bool settingEnabled = false;
  bool available = false;   // sdActive()'s value at the time this was read
  String cardTypeName;      // "SD", "SDHC", "MMC", "unknown", or "" if !available - kept as
                             // text (not SD.h's sdcard_type_t) so this header doesn't need
                             // to include SD.h itself, and neither does anything that
                             // only wants to display status (e.g. webserver_storage.cpp)
  uint64_t totalBytes = 0;  // 0 if not available
  uint64_t usedBytes = 0;   // 0 if not available
  uint32_t checkIntervalHours = 0; // persisted setting, reported regardless of `available`
};
SdStatus getSdStatus();

// The persisted automatic-full-check interval (SdSettings::checkIntervalHours),
// cached at boot and refreshed immediately by saveSdSettings() - main.cpp's
// loop() reads this every tick to decide whether/when to run
// checkSnapshotStorage() automatically. 0 = disabled.
uint32_t sdCheckIntervalHours();

// Takes ownership of jpg (caller must not free() it) - writes it as this
// camera's newest snapshot on SD, pruning that camera's own oldest files
// first if needed (SD_FREE_SPACE_RESERVE_BYTES / SD_MAX_FILES_PER_CAMERA,
// config.h - see their own comments; pruning is deliberately per-camera,
// not a global cross-directory walk, and capped per call via
// SD_PRUNE_MAX_FILES_PER_WRITE to bound how long this holds the internal
// SD mutex). Returns false (having still freed jpg) on any failure - the
// caller (snapshot_history.cpp) doesn't retry or fall back per-incident;
// see sdActive()'s comment for why a failure here also flips sdActive()
// off for anything after this one.
bool writeSdSnapshot(const CameraConfig& cfg, uint8_t* jpg, size_t jpgLen);

// How many snapshots this camera currently has on SD.
size_t sdSnapshotCount(const CameraConfig& cfg);

// Reads the age-th most recent snapshot (0 = newest) into a freshly
// allocated buffer (PSRAM-preferred) - caller must free() it on success.
bool readSdSnapshot(const CameraConfig& cfg, size_t age, uint8_t** outBuf, size_t* outLen);

// Recursively deletes everything this project has ever written to SD (all
// cameras' directories) - NOT a low-level FAT reformat: the Arduino-ESP32
// SD library doesn't expose FatFs's f_mkfs() through its public API
// without reaching into internals (a private diskio driver number) this
// project won't depend on, so this is a logical wipe of this project's
// own files only, not a byte-level "format the card" operation. Never
// touches anything outside /snapshots - safe even if the card has other,
// unrelated files on it. Irreversible; not atomic (a failure partway
// through may leave some files already deleted).
bool eraseAllSnapshots();

// Result of checkSnapshotStorage() below.
struct SnapshotStorageCheckResult {
  bool ranAtAll = false; // false if SD wasn't active to even attempt this
  bool ok = false;       // true only if the walk completed with zero unreadable entries
  size_t directoriesChecked = 0;
  size_t filesChecked = 0;
  size_t unreadableFiles = 0;
  uint64_t totalBytes = 0;
};

// Walks every camera's snapshot directory, confirming each file actually
// opens and reports a size - the closest thing to an integrity check this
// achieves without a real fsck: FatFs (what this project's SD support
// ultimately runs on) doesn't expose one. Doesn't verify FAT metadata/
// structure itself, only that this project's own files are each
// individually readable - said plainly on the Storage page too, so this
// isn't mistaken for a real chkdsk/fsck.
//
// Cost scales with total history stored, which is unbounded by design
// (that's the whole point of SD over the fixed-size PSRAM ring) - not run
// automatically at boot for that reason. See checkNewestSnapshots() below
// for the bounded check initSdStorage() actually runs.
//
// Broadcasts a Telegram alert (sendTelegramMessage, telegram.h) and logs
// to the Activity page (logEvent, event_log_store.h) if any file turns
// out unreadable - safe to call this directly, since every caller (the
// Storage page's "check storage" button, and main.cpp's loop() when
// sdCheckIntervalHours() > 0) only runs once the webserver/WiFi is
// already up.
SnapshotStorageCheckResult checkSnapshotStorage();

// Result of checkNewestSnapshots() below.
struct QuickSnapshotCheckResult {
  bool ranAtAll = false;
  bool ok = false;
  size_t directoriesChecked = 0;
  size_t unreadableFiles = 0;
};

// Lightweight sanity check run once at boot (initSdStorage(), right after
// a successful mount) - for each existing camera directory under
// /snapshots, opens and reads the size of only its NEWEST file, not the
// whole history. Cost is bounded by camera count, not by how much
// history is stored, unlike checkSnapshotStorage()'s full walk - safe to
// run unconditionally at boot without needing to feed the task watchdog
// the way that full walk or a large "/snap all" would. Targets the
// failure mode a reboot interrupted mid-write (see waitForSdIdle, now
// closing that gap going forward) would actually produce: the newest
// file is the one most likely to have been affected. Doesn't need the
// camera list at all - just walks whatever subdirectories already exist,
// so it works even before g_cameras is loaded (this runs early in
// main.cpp's setup(), ahead of that).
//
// Deliberately does NOT call sendTelegramMessage() itself, unlike
// checkSnapshotStorage() above: this runs from initSdStorage(), before
// WiFi is connected (main.cpp's setup() calls it ahead of connectWiFi()),
// so a send attempted from here would just silently fail. The result is
// cached instead - see lastBootCheckResult() below, which main.cpp reads
// once WiFi is actually up, to fold a warning into the existing boot
// Telegram message rather than attempting a second, premature send.
QuickSnapshotCheckResult checkNewestSnapshots();

// The result of the most recent checkNewestSnapshots() call (run once at
// boot from initSdStorage()) - see that function's own comment for why
// the alert has to be deferred to here instead of sent from within it.
// {ranAtAll: false} if SD wasn't active at boot, so a caller should only
// warn when ranAtAll is true AND ok is false.
QuickSnapshotCheckResult lastBootCheckResult();

// Blocks (up to a bounded timeout) until no SD operation is in flight,
// then returns - call this immediately before any *deliberate* reboot
// (ESP.restart(), from /reset, the Maintenance page, or a firmware
// update), if there's any chance a camera task could be mid-write.
// ESP.restart() doesn't wait for other FreeRTOS tasks to finish
// whatever they're doing - a camera task interrupted mid-SD.write() or
// mid-prune (deleting several files) isn't just a lost snapshot: FAT
// (what this project's SD support ultimately runs on) isn't a
// journaling filesystem, so a write or a multi-file prune/erase pass
// cut off partway through can leave an inconsistent allocation table
// (lost clusters, occasionally an unreadable directory), not just a
// truncated file. Every SD-touching function in this module already
// takes the same internal mutex this waits on, so successfully
// acquiring it here guarantees nothing was mid-operation at that
// instant. No-op (returns immediately) if SD isn't active. A timeout
// (something is stuck) is logged but does NOT block the reboot - the
// caller asked for one, and a wedged SD operation is itself a reason to
// reboot, not a reason to refuse to.
void waitForSdIdle();

// Appends one line (a timestamp + event text, caller-formatted) to
// /activity.log on SD - the persisted mirror of the in-memory Activity
// log (event_log_store.h/logEvent), so the "what happened" history
// survives a reboot instead of resetting every time. No-op if !sdActive().
// Bounded by ACTIVITY_LOG_MAX_BYTES (config.h) - see that constant's own
// comment for the "wipe and restart" behavior once exceeded. Best-effort:
// a failure just logs to Serial and marks SD unavailable for the session
// (markSdFailed, same as every other SD I/O failure in this module) -
// never throws or blocks the caller beyond the write itself.
void appendActivityLogLine(const String& line);

// Reads the whole (size-capped) /activity.log back into one String, for
// the dashboard's download route. Returns false (outContent untouched) if
// !sdActive() or the file doesn't exist/can't be opened.
bool readActivityLogFile(String* outContent);
