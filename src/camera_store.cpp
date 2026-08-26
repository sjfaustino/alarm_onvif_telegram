#include "camera_store.h"
#include "secrets.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "camstore";
static const char* NVS_KEY_LIST  = "list";

// Non-printable ASCII separators - no camera name, URL, credential, or note
// should ever legitimately contain these, so records are just split/joined
// on them rather than building full field-escaping machinery.
static const char FIELD_SEP  = '\x1F';
static const char RECORD_SEP = '\x1E';

static String stripSeparators(const String& s) {
  String out = s;
  out.replace(String(FIELD_SEP), "");
  out.replace(String(RECORD_SEP), "");
  return out;
}

static String serializeCamera(const CameraConfig& c) {
  String s;
  s += stripSeparators(c.name);                    s += FIELD_SEP;
  s += stripSeparators(c.deviceServiceUrl);         s += FIELD_SEP;
  s += (c.enabled ? "1" : "0");                     s += FIELD_SEP;
  s += (c.useWSSecurity ? "1" : "0");                s += FIELD_SEP;
  s += (c.includeInitialTerminationTime ? "1" : "0"); s += FIELD_SEP;
  s += (c.includeReplyToAnonymous ? "1" : "0");      s += FIELD_SEP;
  s += stripSeparators(c.snapshotUriOverride);       s += FIELD_SEP;
  s += stripSeparators(c.preferredProfileKeyword);   s += FIELD_SEP;
  s += stripSeparators(c.user);                      s += FIELD_SEP;
  s += stripSeparators(c.pass);                      s += FIELD_SEP;
  s += stripSeparators(c.notes);                     s += FIELD_SEP;
  s += String(c.alertCooldownMs);                    s += FIELD_SEP;
  s += String(c.offlineThresholdMs);
  return s;
}

static CameraConfig deserializeCamera(const String& record) {
  CameraConfig c;
  std::vector<String> fields;
  int fieldStart = 0;
  for (int i = 0; i <= (int)record.length(); i++) {
    if (i == (int)record.length() || record[i] == FIELD_SEP) {
      fields.push_back(record.substring(fieldStart, i));
      fieldStart = i + 1;
    }
  }
  if (fields.size() < 11) return c; // malformed - caller skips entries with an empty name

  c.name                          = fields[0];
  c.deviceServiceUrl              = fields[1];
  c.enabled                       = fields[2] == "1";
  c.useWSSecurity                 = fields[3] == "1";
  c.includeInitialTerminationTime = fields[4] == "1";
  c.includeReplyToAnonymous       = fields[5] == "1";
  c.snapshotUriOverride           = fields[6];
  c.preferredProfileKeyword       = fields[7];
  c.user                          = fields[8];
  c.pass                          = fields[9];
  c.notes                         = fields[10];
  // alertCooldownMs/offlineThresholdMs were added after the original
  // 11-field format - records saved before that keep CameraConfig's
  // defaults via fields.size().
  if (fields.size() >= 12 && fields[11].length() > 0) {
    c.alertCooldownMs = (unsigned long)fields[11].toInt();
  }
  if (fields.size() >= 13 && fields[12].length() > 0) {
    c.offlineThresholdMs = (unsigned long)fields[12].toInt();
  }
  return c;
}

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
  prefs.end();

  if (!alreadyInitialized) {
    std::vector<CameraConfig> cams = seedFromSecrets();
    saveCameras(cams);
    Serial.printf("[camera_store] First boot with NVS-backed camera storage - seeded %u camera(s) "
                  "from secrets.h's CAMERA_SEED.\n", (unsigned)cams.size());
    return cams;
  }

  std::vector<CameraConfig> cams;
  int recStart = 0;
  for (int i = 0; i <= (int)blob.length(); i++) {
    if (i == (int)blob.length() || blob[i] == RECORD_SEP) {
      if (i > recStart) {
        CameraConfig c = deserializeCamera(blob.substring(recStart, i));
        if (c.name.length() > 0) cams.push_back(c);
      }
      recStart = i + 1;
    }
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
  prefs.putString(NVS_KEY_LIST, blob);
  prefs.end();
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
