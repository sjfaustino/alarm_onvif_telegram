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

// Spawns cameras[index]'s FreeRTOS monitoring task - same as
// startMonitoring() (main.cpp) at boot, exposed for webserver_cameras.cpp
// to spawn one live when an edit enables a previously-disabled camera, or
// when applyPendingNewCameraIfAny() below just gave a brand-new camera its
// first live slot. index must be a slot with no task CURRENTLY running -
// either never spawned before, or one requestCameraStop() (camera.h) has
// since torn down; calling this while a task still owns the slot would
// create two tasks racing over the same CameraConfig/CameraState.
void spawnCameraTask(size_t index);

// Stages cam to be added to g_cameras/g_cameraStates and
// spawned live (if enabled), on loop()'s own task - see
// applyPendingNewCameraIfAny's own comment for why the actual push_back
// must happen there, not here. Called from webserver_cameras.cpp's
// saveCameraSubmission when a brand-new camera (originalName empty) is
// added via the dashboard. Returns false without staging anything if
// MAX_CAMERAS' reserved capacity (config.h) is already used up - the
// caller falls back to the original "needs a reboot" banner in that case.
// Only ever one stage pending at a time (last-write-wins, matching
// CameraState::pendingConfig's own single-slot shape) - safe because
// PsychicHttp services one request at a time, so at most one dashboard
// save can be in flight to call this.
bool stagePendingNewCamera(const CameraConfig& cam);

// Called once per loop() tick (main.cpp) - claims and applies whatever
// stagePendingNewCamera staged, if anything.
//
// Why this can't just push_back from the webserver's own task the moment
// a new camera is submitted, even with MAX_CAMERAS' reserved capacity
// guaranteeing no reallocation: reservation only protects the ADDRESSES
// of elements that already exist (so a running camera task's
// CameraTaskContext/CameraState::user/pass pointers stay valid across a
// later push_back elsewhere) - it says nothing about two different tasks
// concurrently reading and mutating the vectors' own size()/structure.
// Several functions (sendHeartbeat, checkBridgeCamerasAndMaybePulseRelay,
// pollTelegramCommands, enforceSnapshotRetention,
// checkScheduledAlertReverts - all in main.cpp/health_monitor.cpp/telegram_commands.cpp) already read
// g_cameras.size()/.data() on loop()'s own task, with no lock of their
// own, relying on those vectors never changing shape except from that
// same task. Restricting the one operation that DOES change their shape
// (growing them for a brand-new camera) to that same task keeps that true
// - the alternative would be adding a lock around every one of those
// existing read sites just to cover a rare "camera added" event.
void applyPendingNewCameraIfAny();
