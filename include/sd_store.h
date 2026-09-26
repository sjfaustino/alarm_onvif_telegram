#pragma once
#include <Arduino.h>
#include <vector>
#include "camera_store.h" // CameraConfig
#include "config.h" // SD_RETENTION_DAYS_DEFAULT
#include "snapshot_source.h" // SnapshotSource

// Optional SD snapshot storage. snapshot_history.h chooses between this and
// the PSRAM ring; this module knows nothing about the ring.

// Off by default. Enabling takes effect at the next boot: mounting SPI while
// camera tasks may be writing isn't worth the churn.
struct SdSettings {
  bool enabled = false;
  // Hours between automatic full storage checks; 0 = off (a full walk can be
  // slow on a big card). Applies immediately.
  uint32_t checkIntervalHours = 0;
  // Global retention in days (per-camera override:
  // CameraConfig::retentionDays); 0 = keep forever. Applies immediately.
  uint16_t retentionDays = SD_RETENTION_DAYS_DEFAULT;
};
SdSettings loadSdSettings();
bool saveSdSettings(const SdSettings& settings);

// Mounts the card only if enabled; otherwise never touches the SPI pins. Call
// from setup() after the PSRAM check.
void initSdStorage();

// Enabled and usable. Any I/O failure turns this off for the rest of the boot
// (history falls back to the PSRAM ring).
bool sdActive();

// Storage page status; never re-probes the hardware.
struct SdStatus {
  bool settingEnabled = false;
  bool available = false;   // sdActive()'s value at the time this was read
  String cardTypeName;      // "SD", "SDHC", "MMC", "unknown", or "" if !available - text,
  uint64_t totalBytes = 0;  // 0 if not available
  uint64_t usedBytes = 0;   // 0 if not available
  uint32_t checkIntervalHours = 0; // persisted setting, reported regardless of `available`
  uint16_t retentionDays = SD_RETENTION_DAYS_DEFAULT; // persisted setting, reported regardless of `available`
  // Plain-English reason the card isn't available ("" otherwise), for the
  // Storage page and the boot notice.
  String unavailableReason;
};
SdStatus getSdStatus();

// Cached settings, refreshed by saveSdSettings().
uint32_t sdCheckIntervalHours();

uint16_t sdRetentionDays();

// Takes ownership of jpg (freed even on failure). Prunes this camera's oldest
// files first if needed, capped per call to bound mutex hold time. source is
// encoded in the filename.
bool writeSdSnapshot(const CameraConfig& cfg, uint8_t* jpg, size_t jpgLen, SnapshotSource source);

size_t sdSnapshotCount(const CameraConfig& cfg);

// age 0 = newest. Caller free()s *outBuf on success.
bool readSdSnapshot(const CameraConfig& cfg, size_t age, uint8_t** outBuf, size_t* outLen);

// Motion if out of range or SD inactive.
SnapshotSource sdSnapshotSourceAt(const CameraConfig& cfg, size_t age);

// Newest-first sources from one directory listing.
std::vector<SnapshotSource> sdSnapshotSourcesAll(const CameraConfig& cfg);

// As above plus each file's date (from its name), for the Gallery.
std::vector<SnapshotEntryInfo> sdSnapshotEntriesAll(const CameraConfig& cfg);

// Deletes everything under /snapshots. Not a format; irreversible and not
// atomic.
bool eraseAllSnapshots();

struct SnapshotStorageCheckResult {
  bool ranAtAll = false; // false if SD wasn't active to even attempt this
  bool ok = false;       // true only if the walk completed with zero unreadable entries
  size_t directoriesChecked = 0;
  size_t filesChecked = 0;
  size_t unreadableFiles = 0;
  uint64_t totalBytes = 0;
};

// Opens every stored file to confirm it's readable - not a real fsck. Cost
// scales with history, so it isn't run at boot. Alerts via Telegram, so call
// only once WiFi is up.
SnapshotStorageCheckResult checkSnapshotStorage();

struct QuickSnapshotCheckResult {
  bool ranAtAll = false;
  bool ok = false;
  size_t directoriesChecked = 0;
  size_t unreadableFiles = 0;
};

// Boot-time check: reads only the newest file per camera directory (what an
// interrupted write would damage). Runs before WiFi, so it doesn't alert; the
// boot notice reads lastBootCheckResult() later.
QuickSnapshotCheckResult checkNewestSnapshots();

// ranAtAll is false if SD wasn't active at boot.
QuickSnapshotCheckResult lastBootCheckResult();

// Waits (bounded) for in-flight SD work before a deliberate reboot: FAT isn't
// journaled, so an interrupted write can corrupt the allocation table. A
// timeout is logged and the reboot proceeds.
void waitForSdIdle();

// Appends to /activity.log, the Activity log's persisted mirror. No-op without
// SD; capped at ACTIVITY_LOG_MAX_BYTES.
void appendActivityLogLine(const String& line);

// Whole log file, for the download route.
bool readActivityLogFile(String* outContent);

struct SnapshotRetentionResult {
  bool ranAtAll = false;
  size_t camerasSwept = 0;
  size_t filesDeleted = 0;
};

// Deletes snapshots older than each camera's effective retention (its own, or
// the global one), using filename timestamps (SD mtime is unreliable). Kept
// separate from checkSnapshotStorage so a "check" never deletes.
SnapshotRetentionResult enforceSnapshotRetention(const std::vector<CameraConfig>& cameras,
                                                  uint16_t globalRetentionDays);
