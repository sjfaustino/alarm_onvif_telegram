#include "sd_store.h"
#include "snapshot_storage.h"
#include "config.h"
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <time.h>

static const char* NVS_NAMESPACE = "sdstore";
static const char* NVS_KEY_ENABLED = "enabled";
static const char* SNAPSHOTS_ROOT = "/snapshots"; // mount-relative - SD's FS methods prepend "/sd" internally

static bool g_sdSettingEnabled = false; // cached at boot, see initSdStorage()
static bool g_sdAvailable = false;      // see sdActive()'s comment
static SemaphoreHandle_t g_sdMutex = xSemaphoreCreateMutex();

SdSettings loadSdSettings() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  SdSettings settings;
  settings.enabled = prefs.getBool(NVS_KEY_ENABLED, false);
  prefs.end();
  return settings;
}

bool saveSdSettings(const SdSettings& settings) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = prefs.putBool(NVS_KEY_ENABLED, settings.enabled) > 0;
  prefs.end();
  if (!ok) {
    Serial.println("[sd_store] ERROR: failed to persist the SD storage setting to NVS - it will "
                    "revert to the previous value on the next reboot.");
  }
  return ok;
}

void initSdStorage() {
  SdSettings settings = loadSdSettings();
  g_sdSettingEnabled = settings.enabled;

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
  // one is safe to run unconditionally here.
  checkNewestSnapshots();
}

bool sdActive() {
  return g_sdSettingEnabled && g_sdAvailable;
}

SdStatus getSdStatus() {
  SdStatus status;
  status.settingEnabled = g_sdSettingEnabled;
  status.available = g_sdAvailable;
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
