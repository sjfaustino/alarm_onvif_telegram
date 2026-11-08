#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>
#include "camera_store.h"

// The camera list's file format, its save and recovery rules, and the edits
// applied to it. The filesystem is behind CameraFileOps so the failure paths
// can be tested natively; camera_store.cpp supplies LittleFS and the locking.

class CameraFileOps {
 public:
  virtual ~CameraFileOps() = default;
  virtual bool exists(const char* path) = 0;
  virtual bool read(const char* path, std::vector<uint8_t>& out) = 0;
  // Replaces the file; returns the bytes written.
  virtual size_t write(const char* path, const std::vector<uint8_t>& data) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
  virtual bool remove(const char* path) = 0;
};

extern const char* const CAMERA_FILE;
extern const char* const CAMERA_FILE_TMP;

// Unreadable means the file is there but couldn't be read: its contents may
// still be good, so nothing may be written over it on that basis.
enum class CameraFileStatus { Loaded, Missing, Unreadable };

// The temporary file is used only when the real one is gone, which happens if
// a save is cut off between removing the old file and renaming the new one.
CameraFileStatus readCameraListFile(CameraFileOps& fs, uint16_t& version, String& records);

// The file's bytes: the schema version (2 bytes, little-endian), then the
// records.
std::vector<uint8_t> encodeCameraList(const std::vector<CameraConfig>& cameras);

// Writes the temporary file and renames it over the real one, so a failed save
// changes nothing. Returns nullptr on success, else what went wrong.
const char* writeCameraListFile(CameraFileOps& fs, const std::vector<uint8_t>& raw);

struct ParsedCameraList {
  std::vector<CameraConfig> cameras;
  int total = 0;
  int dropped = 0;
};

// onDropped gets each record that failed to parse.
ParsedCameraList parseCameraList(const String& records, uint16_t version,
                                 const std::function<void(const String& record)>& onDropped);

// Whether to save the just-loaded list in the current format. Never when
// nothing was actually read (that would replace the stored list with an empty
// one) or when a record failed to parse (that would drop it for good).
bool shouldRewriteCameraList(bool fromFile, bool fromNvs, int droppedRecords, uint16_t storedVersion);

// Edits to a loaded list; Saved means it changed and needs saving. Names are
// case-insensitive keys.
CameraSaveResult applyCameraAdd(std::vector<CameraConfig>& cameras, const CameraConfig& cam);
CameraSaveResult applyCameraUpdate(std::vector<CameraConfig>& cameras, const String& originalName,
                                   const CameraConfig& cam);
bool applyCameraDelete(std::vector<CameraConfig>& cameras, const String& name);

// The edit form leaves the password blank to mean "keep the stored one".
void keepStoredPasswordIfBlank(CameraConfig& cam, const String& originalName,
                               const std::vector<CameraConfig>& stored);

enum class LiveCameraAction { None, Reload, Start, Stop };

// What a saved edit means for the running camera: wasRunning is whether its
// task is alive, not its old enabled flag (a task with no credentials exits
// with the camera still enabled, and the save is what restarts it).
LiveCameraAction liveActionAfterSave(bool wasRunning, bool enabledNow);
