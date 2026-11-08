#include "camera_file_store.h"
#include "camera_serialize.h"

const char* const CAMERA_FILE = "/cameras.dat";
const char* const CAMERA_FILE_TMP = "/cameras.tmp";

// Separates records; camera_serialize separates fields.
static const char RECORD_SEP = '\x1E';

static bool readInto(CameraFileOps& fs, const char* path, uint16_t& version, String& records) {
  std::vector<uint8_t> raw;
  if (!fs.read(path, raw) || raw.size() < 2) return false;
  version = (uint16_t)(raw[0] | (raw[1] << 8));
  records = "";
  records.reserve(raw.size() - 2);
  for (size_t i = 2; i < raw.size(); i++) records += (char)raw[i];
  return true;
}

CameraFileStatus readCameraListFile(CameraFileOps& fs, uint16_t& version, String& records) {
  if (fs.exists(CAMERA_FILE)) {
    return readInto(fs, CAMERA_FILE, version, records) ? CameraFileStatus::Loaded : CameraFileStatus::Unreadable;
  }
  return fs.exists(CAMERA_FILE_TMP) && readInto(fs, CAMERA_FILE_TMP, version, records) ? CameraFileStatus::Loaded
                                                                                        : CameraFileStatus::Missing;
}

std::vector<uint8_t> encodeCameraList(const std::vector<CameraConfig>& cameras) {
  std::vector<uint8_t> raw = {(uint8_t)(CAMERA_SCHEMA_VERSION & 0xFF), (uint8_t)(CAMERA_SCHEMA_VERSION >> 8)};
  for (size_t i = 0; i < cameras.size(); i++) {
    if (i > 0) raw.push_back((uint8_t)RECORD_SEP);
    String record = serializeCamera(cameras[i]);
    raw.insert(raw.end(), record.c_str(), record.c_str() + record.length());
  }
  return raw;
}

const char* writeCameraListFile(CameraFileOps& fs, const std::vector<uint8_t>& raw) {
  if (fs.write(CAMERA_FILE_TMP, raw) != raw.size()) {
    fs.remove(CAMERA_FILE_TMP);
    return "writing the new list failed (filesystem full?)";
  }
  if (fs.rename(CAMERA_FILE_TMP, CAMERA_FILE)) return nullptr;
  // LittleFS's rename replaces the target atomically; this fallback is in case
  // a VFS layer refuses an existing one.
  if (fs.remove(CAMERA_FILE) && fs.rename(CAMERA_FILE_TMP, CAMERA_FILE)) return nullptr;
  return "replacing the old list failed";
}

ParsedCameraList parseCameraList(const String& records, uint16_t version,
                                 const std::function<void(const String& record)>& onDropped) {
  ParsedCameraList parsed;
  int start = 0;
  for (int i = 0; i <= (int)records.length(); i++) {
    if (i < (int)records.length() && records[i] != RECORD_SEP) continue;
    if (i > start) {
      parsed.total++;
      String record = records.substring(start, i);
      CameraConfig c = deserializeCamera(record, version);
      if (c.name.length() > 0) {
        parsed.cameras.push_back(c);
      } else {
        parsed.dropped++;
        onDropped(record);
      }
    }
    start = i + 1;
  }
  return parsed;
}

bool shouldRewriteCameraList(bool fromFile, bool fromNvs, int droppedRecords, uint16_t storedVersion) {
  if (droppedRecords > 0 || (!fromFile && !fromNvs)) return false;
  return storedVersion != CAMERA_SCHEMA_VERSION || !fromFile;
}

static int findCamera(const std::vector<CameraConfig>& cameras, const String& name) {
  for (size_t i = 0; i < cameras.size(); i++) {
    if (cameras[i].name.equalsIgnoreCase(name)) return (int)i;
  }
  return -1;
}

CameraSaveResult applyCameraAdd(std::vector<CameraConfig>& cameras, const CameraConfig& cam) {
  if (findCamera(cameras, cam.name) >= 0) return CameraSaveResult::NameTaken;
  cameras.push_back(cam);
  return CameraSaveResult::Saved;
}

CameraSaveResult applyCameraUpdate(std::vector<CameraConfig>& cameras, const String& originalName,
                                   const CameraConfig& cam) {
  int idx = findCamera(cameras, originalName);
  if (idx < 0) return CameraSaveResult::NotFound;
  int sameName = findCamera(cameras, cam.name);
  if (sameName >= 0 && sameName != idx) return CameraSaveResult::NameTaken;
  cameras[idx] = cam;
  return CameraSaveResult::Saved;
}

bool applyCameraDelete(std::vector<CameraConfig>& cameras, const String& name) {
  int idx = findCamera(cameras, name);
  if (idx < 0) return false;
  cameras.erase(cameras.begin() + idx);
  return true;
}

void keepStoredPasswordIfBlank(CameraConfig& cam, const String& originalName,
                               const std::vector<CameraConfig>& stored) {
  if (cam.pass.length() > 0) return;
  int idx = findCamera(stored, originalName);
  if (idx >= 0) cam.pass = stored[idx].pass;
}

LiveCameraAction liveActionAfterSave(bool wasRunning, bool enabledNow) {
  if (wasRunning) return enabledNow ? LiveCameraAction::Reload : LiveCameraAction::Stop;
  return enabledNow ? LiveCameraAction::Start : LiveCameraAction::None;
}
