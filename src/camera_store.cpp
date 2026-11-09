#include "camera_store.h"
#include "camera_file_store.h"
#include "camera_serialize.h"
#include "nvs_chunk.h" // joinChunks, for the old chunked list
#include "secrets.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t g_camerasMutex = xSemaphoreCreateRecursiveMutex();

void lockCameraList() { xSemaphoreTakeRecursive(g_camerasMutex, portMAX_DELAY); }
void unlockCameraList() { xSemaphoreGiveRecursive(g_camerasMutex); }

// The list lives in a LittleFS file; camera_file_store has the format and the
// save and recovery rules. NVS (20 KB, shared with WiFi and PHY calibration)
// had no room for a 14-camera list plus the new copy a safe save needs.

static const char* NVS_NAMESPACE  = "camstore";
// Older NVS formats: read once to migrate, then erased. The single-key list hit
// NVS's per-entry limit; the chunked strings "list0", "list1", ... could be left
// mixed old and new by a save that failed part-way, corrupting a record.
static const char* NVS_KEY_LIST_LEGACY = "list";
static const char* NVS_KEY_LIST_CHUNKS = "listChunks"; // uint16_t chunk count
static const char* NVS_KEY_SCHEMA = "schema"; // the chunked list's version
static std::vector<CameraConfig> seedFromSecrets() {
  std::vector<CameraConfig> cams;
  for (size_t i = 0; i < NUM_CAMERA_SEED; i++) {
    const CameraSeed& s = CAMERA_SEED[i];
    CameraConfig c;
    c.name                          = s.name;
    c.deviceServiceUrl              = s.deviceServiceUrl;
    c.enabled                       = s.enabled;
    c.useWSSecurity                 = s.useWSSecurity;
    c.includeInitialTerminationTime = s.includeInitialTerminationTime;
    c.includeReplyToAnonymous       = s.includeReplyToAnonymous;
    c.snapshotUriOverride           = s.snapshotUriOverride;
    c.preferredProfileKeyword       = s.preferredProfileKeyword;
    c.user                          = s.user;
    c.pass                          = s.pass;
    c.notes                         = s.notes;
    cams.push_back(c);
  }
  return cams;
}

static String chunkKey(uint16_t index) {
  char key[16];
  snprintf(key, sizeof(key), "list%u", (unsigned)index);
  return String(key);
}

// Mounted on first use, which is setup()'s loadCameras(); formats the
// partition if it has never held a filesystem.
static bool cameraFsReady() {
  static bool tried = false, ready = false;
  if (!tried) {
    tried = true;
    ready = LittleFS.begin(true);
    if (!ready) Serial.println("[camera_store] ERROR: could not mount LittleFS - camera changes can't be saved.");
  }
  return ready;
}

class LittleFsCameraOps : public CameraFileOps {
 public:
  bool exists(const char* path) override { return LittleFS.exists(path); }
  bool read(const char* path, std::vector<uint8_t>& out) override {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t len = f.size();
    out.resize(len);
    bool ok = f.read(out.data(), len) == len;
    f.close();
    if (!ok) Serial.printf("[camera_store] ERROR: could not read %s (%u bytes).\n", path, (unsigned)len);
    return ok;
  }
  size_t write(const char* path, const std::vector<uint8_t>& data) override {
    File f = LittleFS.open(path, "w");
    if (!f) return 0;
    size_t written = f.write(data.data(), data.size());
    f.close();
    return written;
  }
  bool rename(const char* from, const char* to) override { return LittleFS.rename(from, to); }
  bool remove(const char* path) override { return LittleFS.remove(path); }
};

static LittleFsCameraOps g_cameraFileOps;

static void eraseNvsCameraList() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  uint16_t chunkCount = prefs.getUShort(NVS_KEY_LIST_CHUNKS, 0);
  for (uint16_t i = 0; i < chunkCount; i++) prefs.remove(chunkKey(i).c_str());
  prefs.remove(NVS_KEY_LIST_CHUNKS);
  prefs.remove(NVS_KEY_SCHEMA);
  prefs.remove(NVS_KEY_LIST_LEGACY);
  prefs.end();
}

std::vector<CameraConfig> loadCameras() {
  String blob;
  uint16_t storedVersion = 0; // 0 = pre-versioning records
  CameraFileStatus fileStatus = CameraFileStatus::Missing;
  if (cameraFsReady()) fileStatus = readCameraListFile(g_cameraFileOps, storedVersion, blob);
  bool fromFile = fileStatus == CameraFileStatus::Loaded;
  bool alreadyInitialized = fileStatus != CameraFileStatus::Missing;
  bool fromNvs = false;

  if (!fromFile) {
    Preferences prefs;
    // Read-write (see loadDashboardAuth).
    prefs.begin(NVS_NAMESPACE, false);
    bool hasChunkedList = prefs.isKey(NVS_KEY_LIST_CHUNKS);
    bool hasLegacyList  = prefs.isKey(NVS_KEY_LIST_LEGACY);
    fromNvs = hasChunkedList || hasLegacyList;
    alreadyInitialized = alreadyInitialized || fromNvs;
    if (hasChunkedList) {
      uint16_t chunkCount = prefs.getUShort(NVS_KEY_LIST_CHUNKS, 0);
      std::vector<String> chunks;
      chunks.reserve(chunkCount);
      for (uint16_t i = 0; i < chunkCount; i++) chunks.push_back(prefs.getString(chunkKey(i).c_str(), ""));
      blob = joinChunks(chunks);
    } else if (hasLegacyList) {
      blob = prefs.getString(NVS_KEY_LIST_LEGACY, ""); // pre-chunking format
    }
    storedVersion = prefs.getUShort(NVS_KEY_SCHEMA, 0);
    prefs.end();
  }

  if (!alreadyInitialized) {
    std::vector<CameraConfig> cams = seedFromSecrets();
    if (!saveCameras(cams)) {
      Serial.println("[camera_store] ERROR: failed to persist the first-boot CAMERA_SEED - "
                      "this boot will still use it, but it wasn't saved and won't survive a reboot.");
    } else {
      Serial.printf("[camera_store] First boot with stored cameras - seeded %u camera(s) "
                    "from secrets.h's CAMERA_SEED.\n", (unsigned)cams.size());
    }
    sortCamerasByName(cams);
    return cams;
  }

  if (storedVersion > CAMERA_SCHEMA_VERSION) {
    Serial.printf("[camera_store] WARNING: stored camera schema (%u) is newer than this firmware "
                  "understands (%u) - was this board previously running newer firmware? Parsing "
                  "with the newest layout this build knows; some fields may come back wrong.\n",
                  (unsigned)storedVersion, (unsigned)CAMERA_SCHEMA_VERSION);
  }

  ParsedCameraList parsed = parseCameraList(blob, storedVersion, [storedVersion](const String& record) {
    Serial.printf("[camera_store] WARNING: dropped a camera record (\"%s\") that failed to parse "
                  "under schema %u (found %u field(s), expected %s).\n",
                  record.substring(0, record.indexOf('\x1F')).c_str(), (unsigned)storedVersion,
                  (unsigned)cameraRecordFieldCount(record), cameraRecordExpectedFields(storedVersion).c_str());
  });
  std::vector<CameraConfig>& cams = parsed.cameras;

  // Rewrite in the current schema, unless a record failed to parse: saving
  // then would permanently drop it. Leave the stored list alone and retry each
  // boot instead.
  if (parsed.dropped > 0) {
    Serial.printf("[camera_store] %d of %d camera record(s) failed to parse - NOT migrating/rewriting "
                  "the stored list this boot so the raw data isn't lost. Only the %u that parsed are active "
                  "for now; saving anything from this dashboard will overwrite the stored list, including "
                  "the unparsed records.\n", parsed.dropped, parsed.total, (unsigned)cams.size());
  } else if (fileStatus == CameraFileStatus::Unreadable && !fromNvs) {
    Serial.println("[camera_store] ERROR: the camera list exists but could not be read - NOT rewriting it. "
                   "No cameras are active; saving anything from this dashboard will overwrite it.");
  } else if (shouldRewriteCameraList(fromFile, fromNvs, parsed.dropped, storedVersion)) {
    // Once per boot: this runs on every page load, and a save that fails for
    // lack of space would fail again.
    static bool migrationTried = false;
    if (!migrationTried) {
      migrationTried = true;
      Serial.printf("[camera_store] Migrating %u camera record(s) from schema %u to %u%s.\n",
                    (unsigned)cams.size(), (unsigned)storedVersion, (unsigned)CAMERA_SCHEMA_VERSION,
                    fromFile ? "" : ", from NVS to LittleFS");
      saveCameras(cams);
    }
  }

  sortCamerasByName(cams);
  return cams;
}

bool saveCameras(const std::vector<CameraConfig>& cameras) {
  std::vector<uint8_t> raw = encodeCameraList(cameras);
  const char* failure = cameraFsReady() ? writeCameraListFile(g_cameraFileOps, raw) : "LittleFS is not mounted";

  if (failure) {
    Serial.printf("[camera_store] ERROR: could not save %u camera(s) (%u bytes): %s. The stored list is "
                  "unchanged.\n", (unsigned)cameras.size(), (unsigned)raw.size(), failure);
    return false;
  }
  eraseNvsCameraList(); // a no-op once migrated; frees about 130 NVS entries the first time
  return true;
}

CameraSaveResult addCamera(const CameraConfig& cam) {
  CameraListLock lock;
  std::vector<CameraConfig> cams = loadCameras();
  CameraSaveResult result = applyCameraAdd(cams, cam);
  if (result == CameraSaveResult::Saved && !saveCameras(cams)) result = CameraSaveResult::WriteFailed;
  return result;
}

bool deleteCamera(const String& name) {
  CameraListLock lock;
  std::vector<CameraConfig> cams = loadCameras();
  return applyCameraDelete(cams, name) && saveCameras(cams);
}

bool replaceAllCameras(const std::vector<CameraConfig>& cameras) {
  CameraListLock lock;
  return saveCameras(cameras);
}

CameraSaveResult updateAllCameras(const std::function<void(CameraConfig&)>& mutate,
                                  std::vector<CameraConfig>& updated) {
  CameraListLock lock;
  std::vector<CameraConfig> cams = loadCameras();
  if (cams.empty()) return CameraSaveResult::NotFound;
  for (auto& c : cams) mutate(c);
  if (!saveCameras(cams)) return CameraSaveResult::WriteFailed;
  updated = cams;
  return CameraSaveResult::Saved;
}

CameraSaveResult updateCamera(const String& originalName, const CameraConfig& cam) {
  CameraListLock lock;
  std::vector<CameraConfig> cams = loadCameras();
  CameraSaveResult result = applyCameraUpdate(cams, originalName, cam);
  if (result == CameraSaveResult::Saved && !saveCameras(cams)) result = CameraSaveResult::WriteFailed;
  return result;
}
