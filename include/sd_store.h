#pragma once
#include <Arduino.h>
#include "camera_store.h" // CameraConfig

// Optional SD-backed snapshot history - thread-safe global wrapper around
// the SD/SPI mechanics (config.h's SD_* pin/tuning constants), entirely
// separate from the PSRAM ring fallback (snapshot_history.h decides which
// of the two backs a given camera's history and falls back to the ring
// when this one isn't active). This module knows nothing about the ring.

// Persisted opt-in toggle - off by default, so the board boots exactly as
// it always has (PSRAM ring only) until someone opts in via the Storage
// page and reboots. No live enable/disable: SD.begin()/end() while a
// camera task might be mid-write is real SPI-bus-ownership churn nothing
// else here does, so toggling this just updates the persisted preference,
// applied on the next boot.
struct SdSettings {
  bool enabled = false;
  // Hours between automatic full storage checks (checkSnapshotStorage(),
  // from main.cpp's loop()) - 0 disables it. Off by default: unlike the
  // bounded boot-time check, a full walk's cost scales with total history
  // stored, so an unattended default could surprise someone with a slow
  // check on a large card. Unlike `enabled`, this takes effect
  // immediately on save - just a millis() gate in loop(), no SPI involved.
  uint32_t checkIntervalHours = 0;
};
SdSettings loadSdSettings();
bool saveSdSettings(const SdSettings& settings);

// Attempts to mount the SD card ONLY if the persisted setting is enabled -
// call once from main.cpp's setup(), after the PSRAM gate. Doesn't touch
// the SPI bus/GPIOs at all if disabled. Logs which of three outcomes
// happened (disabled / enabled but no module or card responded / mounted) -
// the middle case falls back to the PSRAM ring exactly like the first.
void initSdStorage();

// True only if SD support is enabled AND currently believed usable - not
// just "was mounted at boot". Any SD I/O failure after boot (card
// removed, contact issue, corruption) flips this to false for the rest of
// the session, falling back to the PSRAM ring instead of silently
// dropping every future snapshot. No automatic recovery - re-checked only
// on the next reboot.
bool sdActive();

// Status for the dashboard Storage page - reflects whatever
// initSdStorage()/subsequent I/O has determined so far; doesn't re-probe
// hardware itself.
struct SdStatus {
  bool settingEnabled = false;
  bool available = false;   // sdActive()'s value at the time this was read
  String cardTypeName;      // "SD", "SDHC", "MMC", "unknown", or "" if !available - text,
                             // not SD.h's sdcard_type_t, so this header doesn't need SD.h
  uint64_t totalBytes = 0;  // 0 if not available
  uint64_t usedBytes = 0;   // 0 if not available
  uint32_t checkIntervalHours = 0; // persisted setting, reported regardless of `available`
};
SdStatus getSdStatus();

// The persisted automatic-full-check interval (SdSettings::checkIntervalHours),
// cached at boot and refreshed by saveSdSettings() - main.cpp's loop()
// reads this every tick to decide whether to run checkSnapshotStorage().
// 0 = disabled.
uint32_t sdCheckIntervalHours();

// Takes ownership of jpg (caller must not free() it) - writes it as this
// camera's newest snapshot on SD, pruning its own oldest files first if
// needed (SD_FREE_SPACE_RESERVE_BYTES / SD_MAX_FILES_PER_CAMERA, config.h;
// pruning is per-camera, not a global walk, and capped per call via
// SD_PRUNE_MAX_FILES_PER_WRITE to bound how long this holds the SD
// mutex). Returns false (having still freed jpg) on any failure - the
// caller doesn't retry; see sdActive() for why a failure here also flips
// it off for anything after this one.
bool writeSdSnapshot(const CameraConfig& cfg, uint8_t* jpg, size_t jpgLen);

// How many snapshots this camera currently has on SD.
size_t sdSnapshotCount(const CameraConfig& cfg);

// Reads the age-th most recent snapshot (0 = newest) into a freshly
// allocated buffer (PSRAM-preferred) - caller must free() it on success.
bool readSdSnapshot(const CameraConfig& cfg, size_t age, uint8_t** outBuf, size_t* outLen);

// Recursively deletes everything this project has ever written to SD (all
// cameras' directories) - a logical wipe of this project's own files, NOT
// a low-level FAT reformat (Arduino-ESP32's SD library doesn't expose
// FatFs's f_mkfs() without reaching into internals this project won't
// depend on). Never touches anything outside /snapshots. Irreversible;
// not atomic (a failure partway through may leave some files deleted).
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
// opens and reports a size - the closest thing to an integrity check
// available without a real fsck (FatFs doesn't expose one). Only verifies
// this project's own files are individually readable, not FAT metadata
// itself - said plainly on the Storage page too, so it isn't mistaken for
// a real chkdsk.
//
// Cost scales with total history stored (unbounded by design, the whole
// point of SD over the fixed-size ring), so not run automatically at
// boot - see checkNewestSnapshots() below for the bounded check
// initSdStorage() actually runs.
//
// Broadcasts a Telegram alert and logs to the Activity page if any file
// is unreadable - safe to call directly, since every caller (the Storage
// page's button, and main.cpp's loop() when sdCheckIntervalHours() > 0)
// only runs once WiFi is already up.
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
// /snapshots, opens and reads only the NEWEST file's size, not the whole
// history. Cost is bounded by camera count, not total history stored,
// unlike checkSnapshotStorage()'s full walk - safe to run unconditionally
// at boot. Targets the failure mode a reboot interrupted mid-write would
// actually produce: the newest file is most likely affected. Doesn't need
// the camera list - just walks whatever subdirectories already exist, so
// it works before g_cameras is loaded (this runs early in main.cpp's
// setup(), ahead of that).
//
// Deliberately does NOT call sendTelegramMessage() itself, unlike
// checkSnapshotStorage() above: this runs before WiFi is connected, so a
// send attempted here would just silently fail. The result is cached
// instead - see lastBootCheckResult() below, which main.cpp reads once
// WiFi is up, folding a warning into the existing boot Telegram message
// rather than attempting a second, premature send.
QuickSnapshotCheckResult checkNewestSnapshots();

// The result of the most recent checkNewestSnapshots() call - see that
// function's comment for why the alert is deferred to here instead of
// sent from within it. {ranAtAll: false} if SD wasn't active at boot, so
// a caller should only warn when ranAtAll is true AND ok is false.
QuickSnapshotCheckResult lastBootCheckResult();

// Blocks (up to a bounded timeout) until no SD operation is in flight,
// then returns - call this immediately before any deliberate reboot
// (/reset, the Maintenance page, a firmware update) if a camera task
// could be mid-write. ESP.restart() doesn't wait for other tasks - a
// camera task interrupted mid-write or mid-prune isn't just a lost
// snapshot: FAT isn't a journaling filesystem, so a write or multi-file
// pass cut off partway through can leave an inconsistent allocation table,
// not just a truncated file. Every SD-touching function here already
// takes the same internal mutex this waits on, so acquiring it guarantees
// nothing was mid-operation at that instant. No-op if SD isn't active. A
// timeout (something stuck) is logged but does NOT block the reboot - a
// wedged SD operation is itself a reason to reboot, not to refuse to.
void waitForSdIdle();

// Appends one line (a timestamp + event text, caller-formatted) to
// /activity.log on SD - the persisted mirror of the in-memory Activity
// log (event_log_store.h/logEvent), so history survives a reboot. No-op
// if !sdActive(). Bounded by ACTIVITY_LOG_MAX_BYTES (config.h - see its
// "wipe and restart" behavior once exceeded). Best-effort: a failure just
// logs to Serial and marks SD unavailable for the session (markSdFailed,
// same as every other SD I/O failure here).
void appendActivityLogLine(const String& line);

// Reads the whole (size-capped) /activity.log back into one String, for
// the dashboard's download route. Returns false (outContent untouched) if
// !sdActive() or the file doesn't exist/can't be opened.
bool readActivityLogFile(String* outContent);
