#pragma once
#include <cstddef>
#include <vector>
#include "camera.h"       // CameraState
#include "camera_store.h" // CameraConfig

// Every configured camera and its runtime state, index-aligned. Loaded and
// reserved to MAX_CAMERAS in setup(); only loop()'s task ever changes their
// size (see applyPendingNewCameraIfAny below). Defined in camera_tasks.cpp.
extern std::vector<CameraConfig> g_cameras;
extern std::vector<CameraState> g_cameraStates;

// Starts the monitoring task for g_cameras[index] (boot, a live re-enable, or
// a live add). The slot must have no running task, or two tasks would share
// its config/state.
void spawnCameraTask(size_t index);

// Queues a new camera for loop() to add and start. False if MAX_CAMERAS is
// used up (the caller then says a reboot is needed). One pending slot, last
// write wins - PsychicHttp handles one save at a time.
bool stagePendingNewCamera(const CameraConfig& cam);

// Called every loop() tick to apply a staged camera. Only loop()'s task may
// change the vectors' size: several loop()-task readers (heartbeat, bridge
// watchdog, command polling, retention, timer reverts) use size()/data()
// without a lock. Reserved capacity keeps element addresses stable, but
// doesn't make concurrent resizing safe.
void applyPendingNewCameraIfAny();
