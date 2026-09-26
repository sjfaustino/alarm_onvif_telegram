#include "camera_store.h"
#include "camera_serialize.h"
#include "nvs_chunk.h"
#include "secrets.h"
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Serializes the load-modify-save sequence so concurrent edits (two tabs, a
// retried submit) can't lose each other's changes. Separate from the web
// layer's own save mutex.
static SemaphoreHandle_t g_camerasMutex = xSemaphoreCreateMutex();

static const char* NVS_NAMESPACE  = "camstore";
// Old single-key list: read as a fallback, never written. One large value hit
// NVS's per-entry limit and silently lost cameras (see nvs_chunk.h).
static const char* NVS_KEY_LIST_LEGACY = "list";
static const char* NVS_KEY_LIST_CHUNKS = "listChunks"; // uint16_t chunk count
static const size_t NVS_CHUNK_MAX_BYTES = 1500;
static const char* NVS_KEY_SCHEMA = "schema"; // see camera_serialize.h's CAMERA_SCHEMA_VERSION comment
// Bumped to rerun the restore once more after the chunking fix.
static const char* NVS_KEY_SEED_RESTORED = "seedRestore3"; // see restoreMissingCamerasFromSeed()

// Separates records; FIELD_SEP (camera_serialize) separates fields.
static const char RECORD_SEP = '\x1E';

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

std::vector<CameraConfig> loadCameras() {
  Preferences prefs;
  // Read-write (see loadDashboardAuth).
  prefs.begin(NVS_NAMESPACE, false);
  bool hasChunkedList = prefs.isKey(NVS_KEY_LIST_CHUNKS);
  bool hasLegacyList  = prefs.isKey(NVS_KEY_LIST_LEGACY);
  bool alreadyInitialized = hasChunkedList || hasLegacyList;

  String blob;
  if (hasChunkedList) {
    uint16_t chunkCount = prefs.getUShort(NVS_KEY_LIST_CHUNKS, 0);
    std::vector<String> chunks;
    chunks.reserve(chunkCount);
    for (uint16_t i = 0; i < chunkCount; i++) chunks.push_back(prefs.getString(chunkKey(i).c_str(), ""));
    blob = joinChunks(chunks);
  } else if (hasLegacyList) {
    blob = prefs.getString(NVS_KEY_LIST_LEGACY, ""); // pre-chunking format - see its declaration comment
  }
  // 0 = pre-versioning records.
  uint16_t storedVersion = prefs.getUShort(NVS_KEY_SCHEMA, 0);
  prefs.end();

  if (!alreadyInitialized) {
    std::vector<CameraConfig> cams = seedFromSecrets();
    if (!saveCameras(cams)) {
      Serial.println("[camera_store] ERROR: failed to persist the first-boot CAMERA_SEED to NVS - "
                      "this boot will still use it, but it wasn't saved and won't survive a reboot.");
    } else {
      Serial.printf("[camera_store] First boot with NVS-backed camera storage - seeded %u camera(s) "
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

  std::vector<CameraConfig> cams;
  int totalRecords = 0;
  int droppedRecords = 0;
  int recStart = 0;
  for (int i = 0; i <= (int)blob.length(); i++) {
    if (i == (int)blob.length() || blob[i] == RECORD_SEP) {
      if (i > recStart) {
        totalRecords++;
        String record = blob.substring(recStart, i);
        CameraConfig c = deserializeCamera(record, storedVersion);
        if (c.name.length() > 0) {
          cams.push_back(c);
        } else {
          droppedRecords++;
          // Expected field count per version, for the log (kept by hand).
          const char* expected = "exactly 24";
          if (storedVersion == 0) expected = "11-14";
          else if (storedVersion == 1) expected = "exactly 14";
          else if (storedVersion == 2) expected = "exactly 19";
          else if (storedVersion == 3) expected = "exactly 20";
          else if (storedVersion == 4) expected = "exactly 22";
          Serial.printf("[camera_store] WARNING: dropped a camera record that failed to parse under "
                        "schema %u (found %u field(s), expected %s).\n", (unsigned)storedVersion,
                        (unsigned)cameraRecordFieldCount(record), expected);
        }
      }
      recStart = i + 1;
    }
  }

  // Rewrite in the current schema, unless a record failed to parse: saving
  // then would permanently drop it. Leave NVS alone and retry each boot
  // instead.
  if (droppedRecords > 0) {
    Serial.printf("[camera_store] %d of %d camera record(s) failed to parse - NOT migrating/rewriting "
                  "NVS this boot so the raw data isn't lost. Only the %u that parsed are active for "
                  "now; saving anything from this dashboard will overwrite the stored list, including "
                  "the unparsed records.\n", droppedRecords, totalRecords, (unsigned)cams.size());
  } else if (storedVersion != CAMERA_SCHEMA_VERSION) {
    Serial.printf("[camera_store] Migrating %u camera record(s) from schema %u to %u.\n",
                  (unsigned)cams.size(), (unsigned)storedVersion, (unsigned)CAMERA_SCHEMA_VERSION);
    saveCameras(cams);
  }

  sortCamerasByName(cams);
  return cams;
}

bool saveCameras(const std::vector<CameraConfig>& cameras) {
  String blob;
  for (size_t i = 0; i < cameras.size(); i++) {
    if (i > 0) blob += RECORD_SEP;
    blob += serializeCamera(cameras[i]);
  }
  std::vector<String> chunks = splitIntoChunks(blob, NVS_CHUNK_MAX_BYTES);

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;

  bool chunksOk = true;
  for (size_t i = 0; i < chunks.size(); i++) {
    // putString returns 0 on failure; this used to be ignored.
    if (prefs.putString(chunkKey((uint16_t)i).c_str(), chunks[i]) == 0) chunksOk = false;
  }
  // Remove leftover chunks from a larger previous save.
  uint16_t oldChunkCount = prefs.getUShort(NVS_KEY_LIST_CHUNKS, 0);
  for (uint16_t i = (uint16_t)chunks.size(); i < oldChunkCount; i++) prefs.remove(chunkKey(i).c_str());
  if (prefs.isKey(NVS_KEY_LIST_LEGACY)) prefs.remove(NVS_KEY_LIST_LEGACY); // done with the pre-chunking format

  bool countOk = prefs.putUShort(NVS_KEY_LIST_CHUNKS, (uint16_t)chunks.size()) > 0;
  bool schemaOk = prefs.putUShort(NVS_KEY_SCHEMA, CAMERA_SCHEMA_VERSION) > 0;
  prefs.end();

  if (!chunksOk || !countOk || !schemaOk) {
    Serial.printf("[camera_store] ERROR: saveCameras failed to persist %u camera(s) across %u "
                  "chunk(s), %u bytes total - NVS may be full. The in-memory list changed but NVS "
                  "still has the old data; this WILL be lost on reboot.\n",
                  (unsigned)cameras.size(), (unsigned)chunks.size(), (unsigned)blob.length());
    return false;
  }
  return true;
}

bool addCamera(const CameraConfig& cam) {
  xSemaphoreTake(g_camerasMutex, portMAX_DELAY);
  std::vector<CameraConfig> cams = loadCameras();
  bool collides = false;
  for (auto& c : cams) {
    if (c.name.equalsIgnoreCase(cam.name)) { collides = true; break; }
  }
  bool ok = false;
  if (!collides) {
    cams.push_back(cam);
    ok = saveCameras(cams);
  }
  xSemaphoreGive(g_camerasMutex);
  return ok;
}

bool deleteCamera(const String& name) {
  xSemaphoreTake(g_camerasMutex, portMAX_DELAY);
  std::vector<CameraConfig> cams = loadCameras();
  bool ok = false;
  for (size_t i = 0; i < cams.size(); i++) {
    if (cams[i].name.equalsIgnoreCase(name)) {
      cams.erase(cams.begin() + i);
      ok = saveCameras(cams);
      break;
    }
  }
  xSemaphoreGive(g_camerasMutex);
  return ok;
}

bool replaceAllCameras(const std::vector<CameraConfig>& cameras) {
  xSemaphoreTake(g_camerasMutex, portMAX_DELAY);
  bool ok = saveCameras(cameras);
  xSemaphoreGive(g_camerasMutex);
  return ok;
}

bool updateAllCameras(const std::function<void(CameraConfig&)>& mutate) {
  xSemaphoreTake(g_camerasMutex, portMAX_DELAY);
  std::vector<CameraConfig> cams = loadCameras(); // safe under the mutex - same as addCamera/updateCamera/deleteCamera do
  if (cams.empty()) {
    xSemaphoreGive(g_camerasMutex);
    return false;
  }
  for (auto& c : cams) mutate(c);
  bool ok = saveCameras(cams);
  xSemaphoreGive(g_camerasMutex);
  return ok;
}

bool updateCamera(const String& originalName, const CameraConfig& cam) {
  xSemaphoreTake(g_camerasMutex, portMAX_DELAY);
  std::vector<CameraConfig> cams = loadCameras();
  int idx = -1;
  for (size_t i = 0; i < cams.size(); i++) {
    if (cams[i].name.equalsIgnoreCase(originalName)) { idx = (int)i; break; }
  }
  bool ok = false;
  if (idx >= 0) {
    bool collides = false;
    for (size_t i = 0; i < cams.size(); i++) {
      if ((int)i != idx && cams[i].name.equalsIgnoreCase(cam.name)) { collides = true; break; }
    }
    if (!collides) {
      cams[idx] = cam;
      ok = saveCameras(cams);
    }
  }
  xSemaphoreGive(g_camerasMutex);
  return ok;
}

size_t restoreMissingCamerasFromSeed() {
  Preferences prefs;
  // Read-write (see loadDashboardAuth).
  prefs.begin(NVS_NAMESPACE, false);
  bool alreadyRestored = prefs.getBool(NVS_KEY_SEED_RESTORED, false);
  prefs.end();
  if (alreadyRestored) return 0;

  std::vector<CameraConfig> existing = loadCameras();
  std::vector<CameraConfig> seed = seedFromSecrets();

  Serial.printf("[camera_store] Restoring from CAMERA_SEED: %u already stored, %u in secrets.h.\n",
                (unsigned)existing.size(), (unsigned)seed.size());
  for (auto& e : existing) Serial.printf("[camera_store]   already have: \"%s\"\n", e.name.c_str());

  size_t added = 0;
  for (auto& s : seed) {
    bool found = false;
    for (auto& e : existing) {
      if (e.name.equalsIgnoreCase(s.name)) { found = true; break; }
    }
    if (found) {
      Serial.printf("[camera_store]   skip \"%s\": already present.\n", s.name.c_str());
    } else if (addCamera(s)) {
      added++;
      Serial.printf("[camera_store]   restored \"%s\".\n", s.name.c_str());
    } else {
      Serial.printf("[camera_store]   FAILED to restore \"%s\" - addCamera() returned false (name "
                    "collision detected mid-loop, or the NVS write itself failed - see any "
                    "saveCameras ERROR line above).\n", s.name.c_str());
    }
    // setup() doesn't feed the watchdog, so feed it per NVS write here.
    esp_task_wdt_reset();
  }

  Preferences writePrefs;
  if (writePrefs.begin(NVS_NAMESPACE, false)) {
    writePrefs.putBool(NVS_KEY_SEED_RESTORED, true);
    writePrefs.end();
  }

  if (added > 0) {
    Serial.printf("[camera_store] Restored %u camera(s) from CAMERA_SEED that were missing from the "
                  "stored list - review/rename/delete as needed via the dashboard.\n", (unsigned)added);
  }
  return added;
}
