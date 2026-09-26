#include "sd_store.h"
#include "snapshot_storage.h"
#include "config.h"
#include "telegram.h"        // sendTelegramMessage - see checkSnapshotStorage()/markSdFailed()'s own comments
#include "telegram_i18n.h"
#include "event_log_store.h" // logEvent
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h> // checkSnapshotStorage() - see its own comment
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <time.h>

static const char* NVS_NAMESPACE = "sdstore";
static const char* NVS_KEY_ENABLED = "enabled";
static const char* NVS_KEY_CHECK_HOURS = "checkHours"; // NVS keys are capped at 15 chars
static const char* NVS_KEY_RETENTION_DAYS = "retDays";
static const char* SNAPSHOTS_ROOT = "/snapshots"; // mount-relative - SD's FS methods prepend "/sd" internally

static bool g_sdSettingEnabled = false;         // cached at boot, see initSdStorage()
static uint32_t g_sdCheckIntervalHours = 0;      // cached, see sdCheckIntervalHours()
static uint16_t g_sdRetentionDays = SD_RETENTION_DAYS_DEFAULT; // cached, see sdRetentionDays()
static bool g_sdAvailable = false;              // see sdActive()'s comment
static String g_sdUnavailableReason;            // see SdStatus::unavailableReason's own comment
static SemaphoreHandle_t g_sdMutex = xSemaphoreCreateMutex();
static QuickSnapshotCheckResult g_lastBootCheckResult; // see lastBootCheckResult()'s own comment

SdSettings loadSdSettings() {
  Preferences prefs;
  // Read-write (see loadDashboardAuth).
  prefs.begin(NVS_NAMESPACE, false);
  SdSettings settings;
  settings.enabled = prefs.getBool(NVS_KEY_ENABLED, false);
  settings.checkIntervalHours = prefs.getUInt(NVS_KEY_CHECK_HOURS, 0);
  // Missing key (older settings) reads as the default, not 0 ("keep forever").
  settings.retentionDays = prefs.getUShort(NVS_KEY_RETENTION_DAYS, SD_RETENTION_DAYS_DEFAULT);
  prefs.end();
  return settings;
}

bool saveSdSettings(const SdSettings& settings) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = prefs.putBool(NVS_KEY_ENABLED, settings.enabled) > 0 &&
            prefs.putUInt(NVS_KEY_CHECK_HOURS, settings.checkIntervalHours) > 0 &&
            prefs.putUShort(NVS_KEY_RETENTION_DAYS, settings.retentionDays) > 0;
  prefs.end();
  if (!ok) {
    Serial.println("[sd_store] ERROR: failed to persist the SD storage setting to NVS - it will "
                    "revert to the previous value on the next reboot.");
    return false;
  }
  // These apply immediately, unlike `enabled`.
  g_sdCheckIntervalHours = settings.checkIntervalHours;
  g_sdRetentionDays = settings.retentionDays;
  return true;
}

void initSdStorage() {
  SdSettings settings = loadSdSettings();
  g_sdSettingEnabled = settings.enabled;
  g_sdCheckIntervalHours = settings.checkIntervalHours;
  g_sdRetentionDays = settings.retentionDays;

  if (!g_sdSettingEnabled) {
    Serial.println("[sd_store] SD card storage is disabled - snapshot history uses the PSRAM ring only.");
    return;
  }

  // Log the specific failure reason; the boot notice stays short.
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI)) {
    Serial.println("[sd_store] SD storage is enabled, but no module responded on the configured SPI "
                    "pins - check wiring/CS pin in config.h. Falling back to the PSRAM ring.");
    g_sdUnavailableReason = "no module responded on the configured SPI pins";
    logEvent("SD storage unavailable at boot: " + g_sdUnavailableReason);
    return;
  }

  sdcard_type_t type = SD.cardType();
  if (type == CARD_NONE) {
    Serial.println("[sd_store] SD storage is enabled and a module responded, but no card is "
                    "inserted. Falling back to the PSRAM ring.");
    g_sdUnavailableReason = "no card inserted";
    logEvent("SD storage unavailable at boot: " + g_sdUnavailableReason);
    SD.end();
    return;
  }

  if (!SD.exists(SNAPSHOTS_ROOT) && !SD.mkdir(SNAPSHOTS_ROOT)) {
    Serial.println("[sd_store] SD card detected, but the /snapshots directory could not be created "
                    "- card may be write-protected or corrupted. Falling back to the PSRAM ring.");
    g_sdUnavailableReason = "/snapshots directory could not be created - card may be write-protected or corrupted";
    logEvent("SD storage unavailable at boot: " + g_sdUnavailableReason);
    SD.end();
    return;
  }

  g_sdAvailable = true;
  Serial.printf("[sd_store] SD card mounted: %.1fMB used / %.1fMB total.\n",
                (double)SD.usedBytes() / (1024.0 * 1024.0), (double)SD.totalBytes() / (1024.0 * 1024.0));

  // Bounded (newest file per camera), so safe at every boot. Cached for the
  // boot notice, since WiFi isn't up yet.
  g_lastBootCheckResult = checkNewestSnapshots();
}

QuickSnapshotCheckResult lastBootCheckResult() {
  return g_lastBootCheckResult;
}

bool sdActive() {
  return g_sdSettingEnabled && g_sdAvailable;
}

uint32_t sdCheckIntervalHours() { return g_sdCheckIntervalHours; }
uint16_t sdRetentionDays() { return g_sdRetentionDays; }

SdStatus getSdStatus() {
  SdStatus status;
  status.settingEnabled = g_sdSettingEnabled;
  status.available = g_sdAvailable;
  status.checkIntervalHours = g_sdCheckIntervalHours;
  status.retentionDays = g_sdRetentionDays;
  if (!g_sdAvailable) {
    status.unavailableReason = g_sdUnavailableReason; // "" if settingEnabled is false too - never set otherwise
    return status;
  }

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  switch (SD.cardType()) {
    case CARD_MMC:    status.cardTypeName = "MMC";     break;
    case CARD_SD:     status.cardTypeName = "SD";      break;
    case CARD_SDHC:   status.cardTypeName = "SDHC";    break;
    default:          status.cardTypeName = "unknown"; break;
  }
  status.totalBytes = SD.totalBytes();
  status.usedBytes = SD.usedBytes();
  xSemaphoreGive(g_sdMutex);
  return status;
}

// Any I/O failure turns SD off for the rest of the boot; history falls back to
// the PSRAM ring.
static void markSdFailed(const char* reason) {
  Serial.printf("[sd_store] SD I/O failure (%s) - marking SD unavailable for the rest of this "
                "session, falling back to the PSRAM snapshot ring.\n", reason);
  g_sdAvailable = false;
  // Only reached once tasks run (WiFi is up), and outside the SD mutex, so the
  // blocking send doesn't stall other cameras.
  logEvent(String("SD storage failed (") + reason + ") - falling back to PSRAM history");
  String reasonStr = reason;
  sendTelegramMessage([reasonStr](TelegramLang lang) { return trSdFailure(lang, reasonStr); });
}

// ============================================================
// Layout: /snapshots/<YYYY>/<MM>/<DD>/<camera>/<file>.jpg. Older history in
// the flat /snapshots/<camera>/ layout was left in place, so everything below
// discovers files at any depth via walkAllFiles.
// ============================================================

// Visits every file under dirPath at any depth, passing its path, name, size
// and parent directory name (which identifies the camera in either layout).
// Caller holds g_sdMutex.
static void walkAllFiles(const String& dirPath,
                          const std::function<void(const String& filePath, const String& fileName, uint64_t size,
                                                    const String& parentDirName)>& visit) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  // Collect entries before recursing rather than relying on FS state
  // mid-openNextFile().
  struct Entry {
    String name;
    bool isDir;
    uint64_t size;
  };
  std::vector<Entry> entries;
  File entry = dir.openNextFile();
  while (entry) {
    entries.push_back({String(entry.name()), entry.isDirectory(), (uint64_t)entry.size()});
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  String dirName = dirPath.substring(dirPath.lastIndexOf('/') + 1);
  for (auto& e : entries) {
    String childPath = dirPath + "/" + e.name;
    if (e.isDir) {
      walkAllFiles(childPath, visit);
    } else {
      visit(childPath, e.name, e.size, dirName);
    }
  }
}

// All of a camera's files in any layout - the one source every per-camera
// operation uses. Caller holds g_sdMutex.
static std::vector<SnapshotFileInfo> listAllFilesForCamera(const String& cameraDirName) {
  std::vector<SnapshotFileInfo> files;
  walkAllFiles(SNAPSHOTS_ROOT, [&](const String& filePath, const String& fileName, uint64_t size,
                                    const String& parentDirName) {
    if (parentDirName != cameraDirName) return;
    SnapshotFileInfo info;
    info.name = fileName;
    info.size = size;
    info.path = filePath;
    files.push_back(info);
  });
  return files;
}

// Newest first (age 0 = index 0). Caller holds g_sdMutex.
static std::vector<SnapshotFileInfo> listCameraFilesNewestFirst(const String& cameraDirName) {
  std::vector<SnapshotFileInfo> files = listAllFilesForCamera(cameraDirName);
  std::sort(files.begin(), files.end(), [](const SnapshotFileInfo& a, const SnapshotFileInfo& b) {
    return a.name > b.name; // filenames are timestamp-prefixed - newest-first
  });
  return files;
}

// Returns whether it removed it; failures are ignored (tidiness only).
static bool rmdirIfEmpty(const String& dirPath) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  File entry = dir.openNextFile();
  bool empty = !entry;
  if (entry) entry.close();
  dir.close();
  return empty && SD.rmdir(dirPath);
}

// Removes now-empty ancestors up to (not including) the root, whatever the
// layout depth, so daily folders don't pile up forever.
static void cleanupEmptyAncestors(const String& filePath) {
  String root = SNAPSHOTS_ROOT;
  String dirPath = filePath.substring(0, filePath.lastIndexOf('/'));
  while (dirPath.length() > root.length() && dirPath.startsWith(root)) {
    if (!rmdirIfEmpty(dirPath)) break;
    dirPath = dirPath.substring(0, dirPath.lastIndexOf('/'));
  }
}

// SD.mkdir() creates one level at a time; create each segment in turn.
static bool ensureDirPath(const String& dirPath) {
  int start = 1; // dirPath always starts with "/" (SNAPSHOTS_ROOT does)
  int slash;
  while ((slash = dirPath.indexOf('/', start)) >= 0) {
    String prefix = dirPath.substring(0, slash);
    if (!SD.exists(prefix) && !SD.mkdir(prefix)) return false;
    start = slash + 1;
  }
  if (!SD.exists(dirPath) && !SD.mkdir(dirPath)) return false;
  return true;
}

// Local date, matching buildSnapshotFilename.
static String buildCameraDayDir(const String& cameraDirName) {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y/%m/%d", &tmStruct);
  return String(SNAPSHOTS_ROOT) + "/" + String(buf) + "/" + cameraDirName;
}

// Caller holds g_sdMutex. Creates dayDir and prunes this camera's oldest files
// (across all dates) to fit one more file under the reserve and per-camera
// cap, at most SD_PRUNE_MAX_FILES_PER_WRITE per call. False only if dayDir
// can't be created.
static bool ensureDirAndPrune(const String& cameraDirName, const String& dayDir, size_t newFileSize) {
  if (!ensureDirPath(dayDir)) return false;

  std::vector<SnapshotFileInfo> files = listAllFilesForCamera(cameraDirName);
  std::sort(files.begin(), files.end(), [](const SnapshotFileInfo& a, const SnapshotFileInfo& b) {
    return a.name < b.name; // filenames are timestamp-prefixed - this sorts oldest-first
  });

  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();
  uint64_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
  uint64_t need = SD_FREE_SPACE_RESERVE_BYTES + (uint64_t)newFileSize;
  uint64_t bytesNeeded = (freeBytes < need) ? (need - freeBytes) : 0;

  size_t countOverCeiling =
      (files.size() >= SD_MAX_FILES_PER_CAMERA) ? (files.size() - SD_MAX_FILES_PER_CAMERA + 1) : 0;

  size_t pruneForSpace = filesToPrune(files, bytesNeeded, SD_PRUNE_MAX_FILES_PER_WRITE).size();
  size_t pruneForCount = std::min(countOverCeiling, SD_PRUNE_MAX_FILES_PER_WRITE);
  size_t pruneCount = std::max(pruneForSpace, pruneForCount);

  for (size_t i = 0; i < pruneCount && i < files.size(); i++) {
    if (!SD.remove(files[i].path)) {
      Serial.printf("[sd_store] Could not delete %s while pruning.\n", files[i].path.c_str());
    } else {
      cleanupEmptyAncestors(files[i].path);
    }
  }
  return true;
}

// "<YYYYMMDD-HHMMSS>_<millis>_<source>.jpg": sorts chronologically as text,
// unique within a burst, and carries the source so no metadata file is needed.
static String buildSnapshotFilename(SnapshotSource source) {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmStruct);
  return String(buf) + "_" + String(millis()) + "_" + snapshotSourceLabel(source) + ".jpg";
}

// Old two-field names (no source) and anything odd read as Motion.
static SnapshotSource parseSnapshotSourceFromFilename(const String& name) {
  int firstUnderscore = name.indexOf('_');
  if (firstUnderscore < 0) return SnapshotSource::Motion;
  int secondUnderscore = name.indexOf('_', firstUnderscore + 1);
  if (secondUnderscore < 0) return SnapshotSource::Motion; // old two-field filename, no source suffix
  int dot = name.lastIndexOf('.');
  if (dot < 0 || dot <= secondUnderscore) return SnapshotSource::Motion;
  return snapshotSourceFromLabel(name.substring(secondUnderscore + 1, dot));
}

// Leading "YYYYMMDD", or "" if not 8 digits. Enough for date filtering.
static String parseSnapshotDateFromFilename(const String& name) {
  if (name.length() < 8) return "";
  for (int i = 0; i < 8; i++) {
    if (!isdigit((unsigned char)name[i])) return "";
  }
  return name.substring(0, 8);
}

bool writeSdSnapshot(const CameraConfig& cfg, uint8_t* jpg, size_t jpgLen, SnapshotSource source) {
  if (!sdActive()) { free(jpg); return false; }

  String cameraDirName = sanitizeCameraDirName(cfg.name);
  String dayDir = buildCameraDayDir(cameraDirName);
  bool ok;
  String filePath;

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  ok = ensureDirAndPrune(cameraDirName, dayDir, jpgLen);
  if (ok) {
    filePath = dayDir + "/" + buildSnapshotFilename(source);
    File f = SD.open(filePath, FILE_WRITE);
    if (f) {
      size_t written = f.write(jpg, jpgLen);
      f.close();
      ok = (written == jpgLen);
      // Remove a short write: the truncated file isn't zero-size, so the
      // checks wouldn't flag it, and it would be served as a corrupt JPEG.
      if (!ok) SD.remove(filePath);
    } else {
      ok = false;
    }
  }
  xSemaphoreGive(g_sdMutex);

  if (!ok) {
    Serial.printf("[%s] SD snapshot write failed (%s).\n", cfg.name.c_str(),
                  filePath.length() > 0 ? filePath.c_str() : dayDir.c_str());
    markSdFailed("write");
  }
  free(jpg);
  return ok;
}

size_t sdSnapshotCount(const CameraConfig& cfg) {
  if (!sdActive()) return 0;
  String cameraDirName = sanitizeCameraDirName(cfg.name);

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  size_t count = listAllFilesForCamera(cameraDirName).size();
  xSemaphoreGive(g_sdMutex);
  return count;
}

bool readSdSnapshot(const CameraConfig& cfg, size_t age, uint8_t** outBuf, size_t* outLen) {
  if (!sdActive()) return false;
  String cameraDirName = sanitizeCameraDirName(cfg.name);

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  std::vector<SnapshotFileInfo> files = listCameraFilesNewestFirst(cameraDirName);

  if (age >= files.size()) {
    xSemaphoreGive(g_sdMutex);
    return false;
  }

  File f = SD.open(files[age].path, FILE_READ);
  if (!f) {
    xSemaphoreGive(g_sdMutex);
    markSdFailed("read");
    return false;
  }

  size_t len = f.size();
  uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)malloc(len);
  bool ok = (buf != nullptr) && (f.read(buf, len) == len);
  f.close();
  xSemaphoreGive(g_sdMutex);

  if (!ok) {
    if (buf) free(buf);
    markSdFailed("read");
    return false;
  }

  *outBuf = buf;
  *outLen = len;
  return true;
}

SnapshotSource sdSnapshotSourceAt(const CameraConfig& cfg, size_t age) {
  if (!sdActive()) return SnapshotSource::Motion;
  String cameraDirName = sanitizeCameraDirName(cfg.name);

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  std::vector<SnapshotFileInfo> files = listCameraFilesNewestFirst(cameraDirName);
  SnapshotSource source = SnapshotSource::Motion;
  if (age < files.size()) source = parseSnapshotSourceFromFilename(files[age].name);
  xSemaphoreGive(g_sdMutex);
  return source;
}

std::vector<SnapshotSource> sdSnapshotSourcesAll(const CameraConfig& cfg) {
  std::vector<SnapshotSource> sources;
  if (!sdActive()) return sources;
  String cameraDirName = sanitizeCameraDirName(cfg.name);

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  std::vector<SnapshotFileInfo> files = listCameraFilesNewestFirst(cameraDirName);
  xSemaphoreGive(g_sdMutex);

  // Parsing needs no mutex, only the listing did.
  sources.reserve(files.size());
  for (auto& f : files) sources.push_back(parseSnapshotSourceFromFilename(f.name));
  return sources;
}

std::vector<SnapshotEntryInfo> sdSnapshotEntriesAll(const CameraConfig& cfg) {
  std::vector<SnapshotEntryInfo> entries;
  if (!sdActive()) return entries;
  String cameraDirName = sanitizeCameraDirName(cfg.name);

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  std::vector<SnapshotFileInfo> files = listCameraFilesNewestFirst(cameraDirName);
  xSemaphoreGive(g_sdMutex);

  entries.reserve(files.size());
  for (auto& f : files) {
    SnapshotEntryInfo info;
    info.source = parseSnapshotSourceFromFilename(f.name);
    info.date = parseSnapshotDateFromFilename(f.name);
    entries.push_back(info);
  }
  return entries;
}

// Deletes everything under dirPath at any depth, then dirPath itself.
static bool removeTreeRecursive(const String& dirPath) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return true; // doesn't exist - nothing to do
  }

  struct Entry {
    String name;
    bool isDir;
  };
  std::vector<Entry> entries;
  File entry = dir.openNextFile();
  while (entry) {
    entries.push_back({String(entry.name()), entry.isDirectory()});
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  bool ok = true;
  for (auto& e : entries) {
    String childPath = dirPath + "/" + e.name;
    if (e.isDir) {
      if (!removeTreeRecursive(childPath)) ok = false;
    } else if (!SD.remove(childPath)) {
      ok = false;
    }
  }
  if (!SD.rmdir(dirPath)) ok = false;
  return ok;
}

bool eraseAllSnapshots() {
  if (!sdActive()) return false;

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  // Recreate the root right away.
  bool ok = removeTreeRecursive(SNAPSHOTS_ROOT);
  if (ok) ok = SD.mkdir(SNAPSHOTS_ROOT);
  xSemaphoreGive(g_sdMutex);

  Serial.printf("[sd_store] Erase all snapshot history: %s.\n", ok ? "done" : "completed with errors");
  return ok;
}

SnapshotStorageCheckResult checkSnapshotStorage() {
  SnapshotStorageCheckResult result;
  if (!sdActive()) return result;
  result.ranAtAll = true;
  result.ok = true;

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  std::set<String> dirsSeen; // distinct leaf directories actually holding files - what "directoriesChecked" counts
  walkAllFiles(SNAPSHOTS_ROOT, [&](const String& filePath, const String&, uint64_t, const String&) {
    dirsSeen.insert(filePath.substring(0, filePath.lastIndexOf('/')));
    result.filesChecked++;
    File f = SD.open(filePath, FILE_READ);
    if (!f || f.size() == 0) {
      result.unreadableFiles++;
      result.ok = false;
    } else {
      result.totalBytes += f.size();
    }
    if (f) f.close();
    // The walk is unbounded, so when run from loop() it could trip the 90s
    // watchdog - whose panic reboot is exactly the mid-write corruption
    // waitForSdIdle avoids. Harmless on the web server task.
    esp_task_wdt_reset();
  });
  result.directoriesChecked = dirsSeen.size();
  xSemaphoreGive(g_sdMutex);

  Serial.printf("[sd_store] Storage check: %u director(ies), %u file(s), %u unreadable.\n",
                (unsigned)result.directoriesChecked, (unsigned)result.filesChecked,
                (unsigned)result.unreadableFiles);

  if (!result.ok) {
    logEvent("SD storage check found " + String((unsigned)result.unreadableFiles) + " unreadable file(s)");
    size_t unreadable = result.unreadableFiles;
    size_t checked = result.filesChecked;
    sendTelegramMessage([unreadable, checked](TelegramLang lang) { return trSdCheckWarning(lang, unreadable, checked); });
  }
  return result;
}

SnapshotRetentionResult enforceSnapshotRetention(const std::vector<CameraConfig>& cameras,
                                                  uint16_t globalRetentionDays) {
  SnapshotRetentionResult result;
  if (!sdActive()) return result;
  result.ranAtAll = true;

  time_t now;
  time(&now);

  for (auto& cfg : cameras) {
    // Clamped at use: imported records can carry up to 65535, and the cutoff's
    // 32-bit multiply would overflow into an arbitrary (possibly
    // expire-everything) value.
    uint16_t cameraRetentionDays = cfg.retentionDays;
    if (cameraRetentionDays > SD_RETENTION_MAX_DAYS) cameraRetentionDays = SD_RETENTION_MAX_DAYS;
    uint16_t effectiveDays = cameraRetentionDays != 0 ? cameraRetentionDays : globalRetentionDays;
    if (effectiveDays == 0) continue; // this camera's effective setting is "keep forever"

    String cameraDirName = sanitizeCameraDirName(cfg.name);
    result.camerasSwept++;

    xSemaphoreTake(g_sdMutex, portMAX_DELAY);
    std::vector<SnapshotFileInfo> files = listAllFilesForCamera(cameraDirName);
    std::sort(files.begin(), files.end(), [](const SnapshotFileInfo& a, const SnapshotFileInfo& b) {
      return a.name < b.name; // oldest-first, same as filesToExpire's own expectation
    });
    std::vector<String> toDeleteNames = filesToExpire(files, effectiveDays, now);
    for (auto& name : toDeleteNames) {
      // Names are unique, so this finds the right file in any folder.
      for (auto& f : files) {
        if (f.name != name) continue;
        if (SD.remove(f.path)) {
          result.filesDeleted++;
          cleanupEmptyAncestors(f.path);
        } else {
          Serial.printf("[sd_store] Retention: could not delete %s.\n", f.path.c_str());
        }
        break;
      }
      // As in checkSnapshotStorage.
      esp_task_wdt_reset();
    }
    xSemaphoreGive(g_sdMutex);
  }

  if (result.filesDeleted > 0) {
    Serial.printf("[sd_store] Retention: deleted %u snapshot(s) across %u camera(s).\n",
                  (unsigned)result.filesDeleted, (unsigned)result.camerasSwept);
    // Log only; routine housekeeping.
    logEvent("Retention: deleted " + String((unsigned)result.filesDeleted) +
             " snapshot(s) older than the configured limit");
  }
  return result;
}

QuickSnapshotCheckResult checkNewestSnapshots() {
  QuickSnapshotCheckResult result;
  if (!sdActive()) return result;
  result.ranAtAll = true;
  result.ok = true;

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  // Newest file per camera (across all its folders), so the cost stays one
  // file per camera.
  std::map<String, String> newestNameByCamera;
  std::map<String, String> newestPathByCamera;
  walkAllFiles(SNAPSHOTS_ROOT, [&](const String& filePath, const String& fileName, uint64_t,
                                    const String& parentDirName) {
    auto it = newestNameByCamera.find(parentDirName);
    if (it == newestNameByCamera.end() || fileName > it->second) {
      newestNameByCamera[parentDirName] = fileName;
      newestPathByCamera[parentDirName] = filePath;
    }
  });

  for (auto& kv : newestPathByCamera) {
    result.directoriesChecked++;
    File f = SD.open(kv.second, FILE_READ);
    if (!f || f.size() == 0) {
      result.unreadableFiles++;
      result.ok = false;
      Serial.printf("[sd_store] Boot check: newest file for %s is unreadable or empty (%s).\n",
                    kv.first.c_str(), kv.second.c_str());
    }
    if (f) f.close();
  }
  xSemaphoreGive(g_sdMutex);

  Serial.printf("[sd_store] Boot check: newest snapshot for each of %u camera(s) - %s.\n",
                (unsigned)result.directoriesChecked,
                result.ok ? "all readable" : "problem(s) found, see above");
  return result;
}

void waitForSdIdle() {
  if (!sdActive()) return;

  if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(SD_IDLE_WAIT_TIMEOUT_MS)) == pdTRUE) {
    xSemaphoreGive(g_sdMutex);
  } else {
    // Every SD operation holds this mutex throughout; a timeout means
    // something is stuck, which is itself a reason to allow the reboot.
    Serial.println("[sd_store] waitForSdIdle: timed out waiting for an in-flight SD operation - "
                    "proceeding with the reboot anyway.");
  }
}

static const char* ACTIVITY_LOG_PATH = "/activity.log"; // SD root, sibling to /snapshots - not per-camera

void appendActivityLogLine(const String& line) {
  if (!sdActive()) return;

  bool failed = false;
  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  File f = SD.open(ACTIVITY_LOG_PATH, FILE_APPEND);
  if (!f) {
    failed = true;
  } else {
    f.println(line);
    size_t sz = f.size();
    f.close();
    if (sz > ACTIVITY_LOG_MAX_BYTES) {
      // Past the cap, start a fresh file beginning with this line.
      SD.remove(ACTIVITY_LOG_PATH);
      File fresh = SD.open(ACTIVITY_LOG_PATH, FILE_APPEND);
      if (fresh) {
        fresh.println(line);
        fresh.close();
      } else {
        // Reopen failed after the remove: mark it so the loss gets reported.
        failed = true;
      }
    }
  }
  xSemaphoreGive(g_sdMutex);

  // Outside the mutex, since markSdFailed does a blocking send. Its nested
  // logEvent no-ops because SD is already marked unavailable.
  if (failed) markSdFailed("activity log append");
}

bool readActivityLogFile(String* outContent) {
  if (!sdActive()) return false;

  bool ok = false;
  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  File f = SD.open(ACTIVITY_LOG_PATH, FILE_READ);
  if (f) {
    // Bounded by ACTIVITY_LOG_MAX_BYTES.
    String content;
    content.reserve(f.size());
    while (f.available()) content += (char)f.read();
    f.close();
    *outContent = content;
    ok = true;
  }
  xSemaphoreGive(g_sdMutex);
  return ok;
}
