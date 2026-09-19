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

  // logEvent() is safe this early - no network dependency (see its own
  // comment, event_log_store.cpp): purely in-RAM, plus an SD append that
  // immediately no-ops via sdActive() below since SD isn't active yet at
  // any of these three failure points. The Telegram boot notice
  // (main.cpp, trSdNotAvailableAtBoot) deliberately stays short - the
  // detailed reason belongs here, in the Activity log, not repeated to
  // every recipient's phone.
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

  // Bounded (one file per distinct camera directory found), unlike the
  // full on-demand check - see checkNewestSnapshots' own comment for why
  // this one is safe to run unconditionally here. Cached, not alerted on
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
  String reasonStr = reason;
  sendTelegramMessage([reasonStr](TelegramLang lang) { return trSdFailure(lang, reasonStr); });
}

// ============================================================
// Directory layout: /snapshots/<YYYY>/<MM>/<DD>/<camera>/<file>.jpg -
// lets a card be browsed by date on a computer, not just one giant
// per-camera folder. Existing history written before this layout existed
// (a flat /snapshots/<camera>/<file>.jpg) is deliberately left exactly
// where it is - no migration - so every function below that needs "this
// camera's whole file list" has to discover files regardless of how deep
// they're nested, not assume a fixed depth. walkAllFiles is the one place
// that recursion lives; everything else is built on top of it.
// ============================================================

// Recursively visits every FILE (not directory) anywhere under dirPath,
// regardless of nesting depth - handles the old flat <camera>/ layout,
// the new <Y>/<M>/<D>/<camera>/ layout, and any mix of the two on the
// same card transparently, since it never assumes a fixed depth. `visit`
// gets the file's full path, its bare filename, its size, and its
// immediate parent directory's own name (e.g. "D05-Traseiras" for a file
// under either layout) - callers use that last one to tell which camera a
// file belongs to without caring how deep it was found. Caller must hold
// g_sdMutex.
static void walkAllFiles(const String& dirPath,
                          const std::function<void(const String& filePath, const String& fileName, uint64_t size,
                                                    const String& parentDirName)>& visit) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  // Collect entries before recursing/visiting - same "don't mutate/rely on
  // FS state out from under an in-progress openNextFile() walk" caution
  // this project already applies wherever a walk's results feed a delete
  // (this one doesn't delete anything itself, but every caller that does
  // built on top of it already collects its own full list before acting).
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

// Every stored file belonging to cameraDirName, wherever it lives (any
// Year/Month/Day folder, or the legacy flat layout) - the one place every
// per-camera operation below (write/prune, count, read, retention) gets
// its file list from, so none of them need their own opinion about the
// directory layout. Caller must hold g_sdMutex.
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

// Same as listAllFilesForCamera above, sorted newest-first (age=0 is index
// 0) - shared by readSdSnapshot/sdSnapshotSourceAt/sdSnapshotSourcesAll,
// all of which need this camera's files in that order. Caller must hold
// g_sdMutex.
static std::vector<SnapshotFileInfo> listCameraFilesNewestFirst(const String& cameraDirName) {
  std::vector<SnapshotFileInfo> files = listAllFilesForCamera(cameraDirName);
  std::sort(files.begin(), files.end(), [](const SnapshotFileInfo& a, const SnapshotFileInfo& b) {
    return a.name > b.name; // filenames are timestamp-prefixed - newest-first
  });
  return files;
}

// Removes dirPath only if it's currently empty. Returns whether it was
// actually removed - cleanupEmptyAncestors below uses that to stop
// walking upward as soon as a non-empty ancestor is found (nothing above
// it can be empty either, since it still contains that directory).
// Silently no-ops (false) if dirPath doesn't exist or can't be opened -
// this is tidiness, never worth failing the caller's own delete over.
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

// After deleting filePath, removes each ancestor directory that's now
// empty, walking upward until one isn't (or SNAPSHOTS_ROOT itself is
// reached - never removed). Handles both the old flat <camera>/ layout
// (one ancestor above the root) and the new <Y>/<M>/<D>/<camera>/ layout
// (four) without needing to know which shape it's looking at - it just
// keeps walking up while directories keep turning out empty. Otherwise a
// card that's been pruning/expiring daily for a year would accumulate
// hundreds of empty day folders per camera, forever.
static void cleanupEmptyAncestors(const String& filePath) {
  String root = SNAPSHOTS_ROOT;
  String dirPath = filePath.substring(0, filePath.lastIndexOf('/'));
  while (dirPath.length() > root.length() && dirPath.startsWith(root)) {
    if (!rmdirIfEmpty(dirPath)) break;
    dirPath = dirPath.substring(0, dirPath.lastIndexOf('/'));
  }
}

// SD.mkdir() only reliably creates one path level at a time on ESP32's FS
// wrapper - unlike a shell's `mkdir -p`, there's no guarantee it walks
// intermediate segments itself. Creates each of dirPath's segments in
// turn, tolerating one that already exists, so the whole nested
// Year/Month/Day/Camera path always ends up present regardless of which
// prefix already did.
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

// Today's Year/Month/Day folder for cameraDirName - where a snapshot
// captured right now belongs. Local time, matching buildSnapshotFilename's
// own localtime_r below.
static String buildCameraDayDir(const String& cameraDirName) {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y/%m/%d", &tmStruct);
  return String(SNAPSHOTS_ROOT) + "/" + String(buf) + "/" + cameraDirName;
}

// Caller must hold g_sdMutex. Ensures dayDir (this camera's Year/Month/Day
// folder for a snapshot captured right now) exists and there's enough
// room (free-space reserve + per-camera file-count ceiling, config.h) for
// one more newFileSize-byte file, pruning this camera's own oldest files
// first if not - gathered from EVERY Year/Month/Day folder it has any
// files in (listAllFilesForCamera), not just dayDir, so the per-camera
// cap is still enforced globally, not reset to zero every time the date
// rolls over. Capped per call via SD_PRUNE_MAX_FILES_PER_WRITE (see
// filesToPrune's own comment on why: bounds how long this holds the
// mutex, blocking every other camera's own writes, during one prune-then-
// write pass). Returns false only if dayDir doesn't exist and couldn't be
// created.
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

// "<YYYYMMDD-HHMMSS>_<millis>_<source>.jpg" - sortable (chronological as a
// plain string, matching how every sort in this file relies on filename
// order - the trailing "_<source>" never affects that ordering, since
// millis() already guarantees uniqueness before it's ever compared), and
// unique even for several shots within the same second (a motion burst
// can fetch multiple snapshots faster than one second apart, but never
// faster than a millisecond apart in practice). The source suffix
// (snapshot_source.h) is what parseSnapshotSourceFromFilename below reads
// back for the Gallery/Preview column - encoded into the filename rather
// than a separate metadata file, so it survives a reboot for free and
// never needs its own retention/pruning logic.
static String buildSnapshotFilename(SnapshotSource source) {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmStruct);
  return String(buf) + "_" + String(millis()) + "_" + snapshotSourceLabel(source) + ".jpg";
}

// Reverse of buildSnapshotFilename's source suffix. A filename written
// before this feature existed has only two underscore-separated fields
// (no source at all) - falls back to SnapshotSource::Motion for that, and
// for any other unrecognized shape, rather than failing the read over a
// cosmetic label.
static SnapshotSource parseSnapshotSourceFromFilename(const String& name) {
  int firstUnderscore = name.indexOf('_');
  if (firstUnderscore < 0) return SnapshotSource::Motion;
  int secondUnderscore = name.indexOf('_', firstUnderscore + 1);
  if (secondUnderscore < 0) return SnapshotSource::Motion; // old two-field filename, no source suffix
  int dot = name.lastIndexOf('.');
  if (dot < 0 || dot <= secondUnderscore) return SnapshotSource::Motion;
  return snapshotSourceFromLabel(name.substring(secondUnderscore + 1, dot));
}

// The "YYYYMMDD" prefix of a filename built by buildSnapshotFilename above
// (before the "-HHMMSS..." remainder) - "" if the first 8 characters
// aren't all digits (a name this project never actually wrote, or one
// truncated/corrupted enough to not even have a real prefix). Used for
// the Gallery page's date-range browsing (sdSnapshotEntriesAll below) -
// deliberately just the leading digits, not a full parseSnapshotTimestamp-
// style validation (lib/snapshot_storage), since a plain string match
// against this same 8-character prefix is all date filtering needs.
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
      // A short write (card nearly full, power dip, bus glitch) leaves a
      // truncated file that would otherwise sit in this camera's
      // directory indistinguishable from a real snapshot - listAllFilesForCamera/
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

  // Parsing filenames doesn't need the SD mutex - only the directory
  // listing above did.
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

// Recursively deletes every file and subdirectory under dirPath, then
// dirPath itself - used by eraseAllSnapshots below for a full wipe.
// Doesn't need to know whether it's looking at the old flat <camera>/
// layout, the new <Y>/<M>/<D>/<camera>/ layout, or a mix of both on the
// same card (the expected real-world case after this layout shipped,
// since existing history was deliberately left in place) - it just
// removes whatever's actually there, at whatever depth.
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
  // removeTreeRecursive above just deleted SNAPSHOTS_ROOT itself along
  // with everything under it - put the empty root back immediately rather
  // than leaving it to the next write's own ensureDirPath to notice.
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
    // History is unbounded by design (that's the whole point of SD over
    // the fixed-size PSRAM ring) - this walk can run long enough on a
    // large card to trip loop()'s 90s task watchdog (main.cpp's
    // initWatchdog()) when called from there (the automatic periodic
    // check, sdCheckIntervalHours). A watchdog panic reboots immediately,
    // without waitForSdIdle()'s in-flight-operation wait - exactly the
    // "reboot cuts off a FAT operation mid-write" corruption risk that
    // function exists to prevent. No-op (harmless) when called from the
    // Storage page's "check storage" button instead, which runs on
    // PsychicHttp's own task - never subscribed to this watchdog in the
    // first place.
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
    // Re-clamped here, at the point of use, not just at the dashboard form
    // (parseCameraForm) - a config Import writes camera records straight
    // to NVS via deserializeCamera/replaceAllCameras, bypassing that
    // clamp entirely, so cfg.retentionDays can arrive as any uint16_t up
    // to 65535. Without this, filesToExpire's cutoff computation
    // ((time_t)retentionDays * 24 * 60 * 60, a 32-bit signed multiply)
    // overflows for a large enough value - wrapping to an arbitrary
    // cutoff instead of failing safe, in the worst case one that makes
    // every stored file for that camera look expired at once.
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
      // Names are unique per camera (timestamp+millis-based), so this
      // always finds exactly the right file regardless of which
      // Year/Month/Day folder (or the legacy flat layout) it's actually
      // stored under.
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
  // Newest file seen so far, per DISTINCT CAMERA (not per leaf directory -
  // a camera can have many Year/Month/Day folders, plus possibly a legacy
  // flat one; this compares across all of them so directoriesChecked
  // keeps meaning "how many cameras have any history", the same bounded
  // cost this function has always had, rather than growing with how many
  // calendar days of retention have accumulated).
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
