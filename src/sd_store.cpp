#include "sd_store.h"
#include "snapshot_storage.h"
#include "config.h"
#include "telegram.h"        // sendTelegramMessage - see checkSnapshotStorage()/markSdFailed()'s own comments
#include "event_log_store.h" // logEvent
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h> // checkSnapshotStorage() - see its own comment
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <algorithm>
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
static SemaphoreHandle_t g_sdMutex = xSemaphoreCreateMutex();
static QuickSnapshotCheckResult g_lastBootCheckResult; // see lastBootCheckResult()'s own comment

SdSettings loadSdSettings() {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(NVS_NAMESPACE, false);
  SdSettings settings;
  settings.enabled = prefs.getBool(NVS_KEY_ENABLED, false);
  settings.checkIntervalHours = prefs.getUInt(NVS_KEY_CHECK_HOURS, 0);
  // getUShort's own default (SD_RETENTION_DAYS_DEFAULT) is what makes this
  // backward-compatible for free: an NVS blob saved before this field
  // existed simply never wrote this key, so it reads back as the sensible
  // default instead of 0 ("keep forever", which nothing chose on purpose).
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
  // checkIntervalHours/retentionDays need no reboot to take effect (unlike
  // `enabled` - see the struct's own comment) - update the caches
  // main.cpp's loop() reads immediately, not just on the next boot.
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

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI)) {
    Serial.println("[sd_store] SD storage is enabled, but no module responded on the configured SPI "
                    "pins - check wiring/CS pin in config.h. Falling back to the PSRAM ring.");
    return;
  }

  sdcard_type_t type = SD.cardType();
  if (type == CARD_NONE) {
    Serial.println("[sd_store] SD storage is enabled and a module responded, but no card is "
                    "inserted. Falling back to the PSRAM ring.");
    SD.end();
    return;
  }

  if (!SD.exists(SNAPSHOTS_ROOT) && !SD.mkdir(SNAPSHOTS_ROOT)) {
    Serial.println("[sd_store] SD card detected, but the /snapshots directory could not be created "
                    "- card may be write-protected or corrupted. Falling back to the PSRAM ring.");
    SD.end();
    return;
  }

  g_sdAvailable = true;
  Serial.printf("[sd_store] SD card mounted: %.1fMB used / %.1fMB total.\n",
                (double)SD.usedBytes() / (1024.0 * 1024.0), (double)SD.totalBytes() / (1024.0 * 1024.0));

  // Bounded (one file per existing camera directory), unlike the full
  // on-demand check - see checkNewestSnapshots' own comment for why this
  // one is safe to run unconditionally here. Cached, not alerted on
  // directly - see lastBootCheckResult()'s own comment for why.
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
  if (!g_sdAvailable) return status;

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

// Marks SD unavailable for the rest of this session - called on any I/O
// failure past the boot-time check, not just there. See sdActive()'s
// comment for why: a card that degrades mid-session should fall back to
// the PSRAM ring from that point on, not silently drop every future
// snapshot.
static void markSdFailed(const char* reason) {
  Serial.printf("[sd_store] SD I/O failure (%s) - marking SD unavailable for the rest of this "
                "session, falling back to the PSRAM snapshot ring.\n", reason);
  g_sdAvailable = false;
  // Safe to call unconditionally: this is only ever reached from
  // writeSdSnapshot/readSdSnapshot, both only reachable once camera tasks
  // are running, which is well after WiFi/the webserver are up - unlike
  // checkNewestSnapshots(), which can't send from its own boot-time call
  // site (see that function's comment). All three call sites are also
  // outside their own g_sdMutex critical section by the time they reach
  // here, so this blocking network call never holds up another camera's
  // SD access.
  logEvent(String("SD storage failed (") + reason + ") - falling back to PSRAM history");
  sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F SD card storage failed (" + String(reason) +
                       ") and has been disabled for the rest of this session - snapshot history is "
                       "back to the PSRAM-only fallback until the next reboot. Check the card/wiring.");
}

// Caller must hold g_sdMutex. Lists dirName's own files (basenames only,
// not full paths - see FS.h's File::name() vs path()), unsorted.
static std::vector<SnapshotFileInfo> listDirFiles(const String& dirName) {
  std::vector<SnapshotFileInfo> files;
  File dir = SD.open(dirName);
  if (!dir || !dir.isDirectory()) return files;

  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      SnapshotFileInfo info;
      info.name = entry.name();
      info.size = (uint64_t)entry.size();
      files.push_back(info);
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  return files;
}

// Caller must hold g_sdMutex. Ensures dirName exists and has enough room
// (free-space reserve + per-camera file-count ceiling, config.h) for one
// more newFileSize-byte file, pruning this camera's own oldest files
// first if not - capped per call via SD_PRUNE_MAX_FILES_PER_WRITE (see
// filesToPrune's own comment on why: bounds how long this holds the
// mutex, blocking every other camera's own writes, during one prune-then-
// write pass). Returns false only if the directory doesn't exist and
// couldn't be created.
static bool ensureDirAndPrune(const String& dirName, size_t newFileSize) {
  if (!SD.exists(dirName) && !SD.mkdir(dirName)) return false;

  std::vector<SnapshotFileInfo> files = listDirFiles(dirName);
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
    String fullPath = dirName + "/" + files[i].name;
    if (!SD.remove(fullPath)) {
      Serial.printf("[sd_store] Could not delete %s while pruning.\n", fullPath.c_str());
    }
  }
  return true;
}

// "<YYYYMMDD-HHMMSS>_<millis>.jpg" - sortable (chronological as a plain
// string, matching how listDirFiles/ensureDirAndPrune rely on filename
// order), and unique even for several shots within the same second (a
// motion burst can fetch multiple snapshots faster than one second apart,
// but never faster than a millisecond apart in practice).
static String buildSnapshotFilename() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmStruct);
  return String(buf) + "_" + String(millis()) + ".jpg";
}

bool writeSdSnapshot(const CameraConfig& cfg, uint8_t* jpg, size_t jpgLen) {
  if (!sdActive()) { free(jpg); return false; }

  String dirName = String(SNAPSHOTS_ROOT) + "/" + sanitizeCameraDirName(cfg.name);
  bool ok;
  String filePath;

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  ok = ensureDirAndPrune(dirName, jpgLen);
  if (ok) {
    filePath = dirName + "/" + buildSnapshotFilename();
    File f = SD.open(filePath, FILE_WRITE);
    if (f) {
      size_t written = f.write(jpg, jpgLen);
      f.close();
      ok = (written == jpgLen);
      // A short write (card nearly full, power dip, bus glitch) leaves a
      // truncated file that would otherwise sit in this camera's
      // directory indistinguishable from a real snapshot - listDirFiles/
      // sdSnapshotCount count it, and it's a nonzero size so neither
      // checkSnapshotStorage nor checkNewestSnapshots' readability check
      // (which only catches f.size()==0) would ever flag it. It would go
      // on to get served as a corrupted JPEG to a /snap or Gallery
      // request the next time SD is active. Remove it rather than leave
      // it for a future boot to discover.
      if (!ok) SD.remove(filePath);
    } else {
      ok = false;
    }
  }
  xSemaphoreGive(g_sdMutex);

  if (!ok) {
    Serial.printf("[%s] SD snapshot write failed (%s).\n", cfg.name.c_str(),
                  filePath.length() > 0 ? filePath.c_str() : dirName.c_str());
    markSdFailed("write");
  }
  free(jpg);
  return ok;
}

size_t sdSnapshotCount(const CameraConfig& cfg) {
  if (!sdActive()) return 0;
  String dirName = String(SNAPSHOTS_ROOT) + "/" + sanitizeCameraDirName(cfg.name);

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  size_t count = listDirFiles(dirName).size();
  xSemaphoreGive(g_sdMutex);
  return count;
}

bool readSdSnapshot(const CameraConfig& cfg, size_t age, uint8_t** outBuf, size_t* outLen) {
  if (!sdActive()) return false;
  String dirName = String(SNAPSHOTS_ROOT) + "/" + sanitizeCameraDirName(cfg.name);

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  std::vector<SnapshotFileInfo> files = listDirFiles(dirName);
  std::sort(files.begin(), files.end(), [](const SnapshotFileInfo& a, const SnapshotFileInfo& b) {
    return a.name > b.name; // newest-first, so age=0 is index 0
  });

  if (age >= files.size()) {
    xSemaphoreGive(g_sdMutex);
    return false;
  }

  String filePath = dirName + "/" + files[age].name;
  File f = SD.open(filePath, FILE_READ);
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

bool eraseAllSnapshots() {
  if (!sdActive()) return false;

  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  bool ok = true;
  File root = SD.open(SNAPSHOTS_ROOT);
  if (root && root.isDirectory()) {
    File camDir = root.openNextFile();
    while (camDir) {
      if (camDir.isDirectory()) {
        String camDirPath = String(SNAPSHOTS_ROOT) + "/" + String(camDir.name());
        // Collect filenames before deleting - removing entries out from
        // under an in-progress openNextFile() walk on this FS layer isn't
        // documented as safe, so this project doesn't rely on it.
        std::vector<SnapshotFileInfo> files = listDirFiles(camDirPath);
        for (auto& file : files) {
          if (!SD.remove(camDirPath + "/" + file.name)) ok = false;
        }
        camDir.close();
        if (!SD.rmdir(camDirPath)) ok = false;
      } else {
        camDir.close();
      }
      camDir = root.openNextFile();
    }
    root.close();
  }
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
  File root = SD.open(SNAPSHOTS_ROOT);
  if (root && root.isDirectory()) {
    File camDir = root.openNextFile();
    while (camDir) {
      if (camDir.isDirectory()) {
        result.directoriesChecked++;
        String camDirPath = String(SNAPSHOTS_ROOT) + "/" + String(camDir.name());
        camDir.close();
        std::vector<SnapshotFileInfo> files = listDirFiles(camDirPath);
        for (auto& file : files) {
          result.filesChecked++;
          File f = SD.open(camDirPath + "/" + file.name, FILE_READ);
          if (!f || f.size() == 0) {
            result.unreadableFiles++;
            result.ok = false;
          } else {
            result.totalBytes += f.size();
          }
          if (f) f.close();
          // History is unbounded by design (that's the whole point of SD
          // over the fixed-size PSRAM ring) - this walk can run long
          // enough on a large card to trip loop()'s 90s task watchdog
          // (main.cpp's initWatchdog()) when called from there (the
          // automatic periodic check, sdCheckIntervalHours). A watchdog
          // panic reboots immediately, without waitForSdIdle()'s
          // in-flight-operation wait - exactly the "reboot cuts off a FAT
          // operation mid-write" corruption risk that function exists to
          // prevent. No-op (harmless) when called from the Storage page's
          // "check storage" button instead, which runs on PsychicHttp's
          // own task - never subscribed to this watchdog in the first
          // place.
          esp_task_wdt_reset();
        }
      } else {
        camDir.close();
      }
      camDir = root.openNextFile();
    }
    root.close();
  }
  xSemaphoreGive(g_sdMutex);

  Serial.printf("[sd_store] Storage check: %u director(ies), %u file(s), %u unreadable.\n",
                (unsigned)result.directoriesChecked, (unsigned)result.filesChecked,
                (unsigned)result.unreadableFiles);

  if (!result.ok) {
    logEvent("SD storage check found " + String((unsigned)result.unreadableFiles) + " unreadable file(s)");
    sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F SD storage check found " +
                         String((unsigned)result.unreadableFiles) + " unreadable file(s) out of " +
                         String((unsigned)result.filesChecked) +
                         " checked. See the dashboard's Storage page or Serial log for details.");
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
    uint16_t effectiveDays = cfg.retentionDays != 0 ? cfg.retentionDays : globalRetentionDays;
    if (effectiveDays == 0) continue; // this camera's effective setting is "keep forever"

    String dirName = String(SNAPSHOTS_ROOT) + "/" + sanitizeCameraDirName(cfg.name);
    result.camerasSwept++;

    xSemaphoreTake(g_sdMutex, portMAX_DELAY);
    std::vector<SnapshotFileInfo> files = listDirFiles(dirName);
    std::vector<String> toDelete = filesToExpire(files, effectiveDays, now);
    for (auto& name : toDelete) {
      if (SD.remove(dirName + "/" + name)) {
        result.filesDeleted++;
      } else {
        Serial.printf("[sd_store] Retention: could not delete %s.\n", (dirName + "/" + name).c_str());
      }
      // Same defensive per-file watchdog reset as checkSnapshotStorage's
      // own walk above - harmless no-op when called from a task never
      // subscribed to the TWDT in the first place (see that function's
      // comment); load-bearing when called from main.cpp's loop().
      esp_task_wdt_reset();
    }
    xSemaphoreGive(g_sdMutex);
  }

  if (result.filesDeleted > 0) {
    Serial.printf("[sd_store] Retention: deleted %u snapshot(s) across %u camera(s).\n",
                  (unsigned)result.filesDeleted, (unsigned)result.camerasSwept);
    // Activity log only, deliberately no Telegram push - routine
    // housekeeping running on a schedule, not something needing attention.
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
  File root = SD.open(SNAPSHOTS_ROOT);
  if (root && root.isDirectory()) {
    File camDir = root.openNextFile();
    while (camDir) {
      if (camDir.isDirectory()) {
        result.directoriesChecked++;
        String camDirPath = String(SNAPSHOTS_ROOT) + "/" + String(camDir.name());
        camDir.close();

        std::vector<SnapshotFileInfo> files = listDirFiles(camDirPath);
        if (!files.empty()) {
          std::sort(files.begin(), files.end(), [](const SnapshotFileInfo& a, const SnapshotFileInfo& b) {
            return a.name > b.name; // newest first - only files[0] is actually checked
          });
          String newestPath = camDirPath + "/" + files[0].name;
          File f = SD.open(newestPath, FILE_READ);
          if (!f || f.size() == 0) {
            result.unreadableFiles++;
            result.ok = false;
            Serial.printf("[sd_store] Boot check: newest file in %s is unreadable or empty (%s).\n",
                          camDirPath.c_str(), newestPath.c_str());
          }
          if (f) f.close();
        }
      } else {
        camDir.close();
      }
      camDir = root.openNextFile();
    }
    root.close();
  }
  xSemaphoreGive(g_sdMutex);

  Serial.printf("[sd_store] Boot check: newest snapshot in each of %u director(ies) - %s.\n",
                (unsigned)result.directoriesChecked,
                result.ok ? "all readable" : "problem(s) found, see above");
  return result;
}

void waitForSdIdle() {
  if (!sdActive()) return;

  if (xSemaphoreTake(g_sdMutex, pdMS_TO_TICKS(SD_IDLE_WAIT_TIMEOUT_MS)) == pdTRUE) {
    xSemaphoreGive(g_sdMutex);
  } else {
    // Every SD-touching function in this module takes g_sdMutex for its
    // whole operation, so failing to acquire it within the timeout means
    // something has genuinely been mid-operation (or wedged) for that
    // whole span - logged, not treated as fatal: the caller asked for a
    // reboot, and a stuck SD operation is itself a reason to grant it,
    // not withhold it.
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
      // Bounded - wipe and start fresh rather than grow forever. The line
      // that just crossed the cap gets re-written into the fresh file
      // (not just discarded with everything before it) - the event it
      // records already happened, so it belongs at the start of the new
      // file, not lost entirely just because it was also the one that
      // tipped the old file over the limit.
      SD.remove(ACTIVITY_LOG_PATH);
      File fresh = SD.open(ACTIVITY_LOG_PATH, FILE_APPEND);
      if (fresh) {
        fresh.println(line);
        fresh.close();
      } else {
        // Reopen failed right after a successful remove - the log file is
        // now simply gone, and without this, `failed` would stay false
        // (only the FIRST open above sets it), so markSdFailed below would
        // never fire and this loss would have no trace anywhere.
        failed = true;
      }
    }
  }
  xSemaphoreGive(g_sdMutex);

  // Released g_sdMutex above BEFORE calling markSdFailed, exactly like
  // writeSdSnapshot/readSdSnapshot/checkSnapshotStorage - it calls
  // logEvent+sendTelegramMessage, a blocking network call that must never
  // happen while holding this mutex, or every other camera's SD write
  // stalls behind it. Recursion-safe too: markSdFailed sets
  // g_sdAvailable=false before calling logEvent, so the nested
  // logEvent -> appendActivityLogLine call immediately no-ops via
  // sdActive() above - one harmless extra log line, not a loop.
  if (failed) markSdFailed("activity log append");
}

bool readActivityLogFile(String* outContent) {
  if (!sdActive()) return false;

  bool ok = false;
  xSemaphoreTake(g_sdMutex, portMAX_DELAY);
  File f = SD.open(ACTIVITY_LOG_PATH, FILE_READ);
  if (f) {
    // Bounded by ACTIVITY_LOG_MAX_BYTES (appendActivityLogLine never lets
    // the file grow past it) - safe as a single in-memory String.
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
