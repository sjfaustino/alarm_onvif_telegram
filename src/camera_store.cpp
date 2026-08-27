#include "camera_store.h"
#include "camera_serialize.h"
#include "secrets.h"
#include <Preferences.h>
#include <esp_task_wdt.h>

static const char* NVS_NAMESPACE  = "camstore";
static const char* NVS_KEY_LIST   = "list";
static const char* NVS_KEY_SCHEMA = "schema"; // see camera_serialize.h's CAMERA_SCHEMA_VERSION comment
static const char* NVS_KEY_SEED_RESTORED = "seedRestored"; // see restoreMissingCamerasFromSeed()

// Separates whole camera records within the NVS blob (distinct from
// camera_serialize.cpp's own FIELD_SEP, which separates one record's
// fields - this file only ever joins/splits on RECORD_SEP, never sees
// FIELD_SEP directly).
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

std::vector<CameraConfig> loadCameras() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  bool alreadyInitialized = prefs.isKey(NVS_KEY_LIST);
  String blob = alreadyInitialized ? prefs.getString(NVS_KEY_LIST, "") : "";
  // 0 = written before schema versioning existed - see
  // camera_serialize.h's CAMERA_SCHEMA_VERSION comment for what that means
  // for how the records below get parsed.
  uint16_t storedVersion = prefs.getUShort(NVS_KEY_SCHEMA, 0);
  prefs.end();

  if (!alreadyInitialized) {
    std::vector<CameraConfig> cams = seedFromSecrets();
    saveCameras(cams);
    Serial.printf("[camera_store] First boot with NVS-backed camera storage - seeded %u camera(s) "
                  "from secrets.h's CAMERA_SEED.\n", (unsigned)cams.size());
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
          Serial.printf("[camera_store] WARNING: dropped a camera record that failed to parse under "
                        "schema %u (found %u field(s), expected %s).\n", (unsigned)storedVersion,
                        (unsigned)cameraRecordFieldCount(record),
                        (storedVersion == 0) ? "11-14" : "exactly 14");
        }
      }
      recStart = i + 1;
    }
  }

  // One-time migration: anything not already on the current schema gets
  // rewritten in the current layout immediately, so every subsequent load
  // this boot (and every boot after) sees storedVersion == CAMERA_SCHEMA_VERSION.
  //
  // Skipped entirely if any record was dropped above - saveCameras() would
  // permanently overwrite the NVS blob with just the survivors (possibly
  // zero cameras), destroying whatever's still in the raw, unparsed
  // original. Safer to leave NVS untouched and keep re-attempting this
  // same (non-destructive) parse every boot until the real problem - a
  // firmware bug, or genuinely corrupt NVS - is fixed, than to "migrate"
  // by quietly deleting the unparsed records.
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

  return cams;
}

bool saveCameras(const std::vector<CameraConfig>& cameras) {
  String blob;
  for (size_t i = 0; i < cameras.size(); i++) {
    if (i > 0) blob += RECORD_SEP;
    blob += serializeCamera(cameras[i]);
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  // putString/putUShort return 0 on failure (NVS full, value too large for
  // one entry, etc.) - previously ignored, so a failed write here would
  // silently report success to every caller (addCamera/updateCamera/
  // deleteCamera) while NVS quietly kept its old value.
  size_t written = prefs.putString(NVS_KEY_LIST, blob);
  bool schemaOk = prefs.putUShort(NVS_KEY_SCHEMA, CAMERA_SCHEMA_VERSION) > 0;
  prefs.end();

  bool listOk = (blob.length() == 0) || (written > 0);
  if (!listOk || !schemaOk) {
    Serial.printf("[camera_store] ERROR: saveCameras failed to persist %u camera(s) (wrote %u of %u "
                  "bytes) - NVS may be full. The in-memory list changed but NVS still has the old "
                  "data; this WILL be lost on reboot.\n",
                  (unsigned)cameras.size(), (unsigned)written, (unsigned)blob.length());
    return false;
  }
  return true;
}

bool addCamera(const CameraConfig& cam) {
  std::vector<CameraConfig> cams = loadCameras();
  for (auto& c : cams) {
    if (c.name.equalsIgnoreCase(cam.name)) return false;
  }
  cams.push_back(cam);
  return saveCameras(cams);
}

bool deleteCamera(const String& name) {
  std::vector<CameraConfig> cams = loadCameras();
  for (size_t i = 0; i < cams.size(); i++) {
    if (cams[i].name.equalsIgnoreCase(name)) {
      cams.erase(cams.begin() + i);
      return saveCameras(cams);
    }
  }
  return false;
}

bool updateCamera(const String& originalName, const CameraConfig& cam) {
  std::vector<CameraConfig> cams = loadCameras();
  int idx = -1;
  for (size_t i = 0; i < cams.size(); i++) {
    if (cams[i].name.equalsIgnoreCase(originalName)) { idx = (int)i; break; }
  }
  if (idx < 0) return false;

  for (size_t i = 0; i < cams.size(); i++) {
    if ((int)i != idx && cams[i].name.equalsIgnoreCase(cam.name)) return false;
  }

  cams[idx] = cam;
  return saveCameras(cams);
}

size_t restoreMissingCamerasFromSeed() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
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
    // Each iteration does a real NVS read+write; setup() doesn't feed the
    // watchdog otherwise (only loop() does), so a slow flash write here
    // shouldn't be allowed to add up toward the 90s timeout.
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
