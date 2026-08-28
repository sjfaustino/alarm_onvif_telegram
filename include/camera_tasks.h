#pragma once
#include <cstddef>

// Spawns cameras[index]'s FreeRTOS monitoring task - the same thing
// startMonitoring() (main.cpp) does for every enabled camera at boot,
// exposed here so webserver_cameras.cpp's save handler can do the same
// for a single camera live, without a reboot, when an edit newly enables
// a camera that had no running task before.
//
// index must refer to a slot already present in main.cpp's live
// g_cameras/g_cameraStates vectors - those are sized once at boot and
// never grow, so a camera added to NVS after boot still needs a reboot
// before it has a live slot to spawn a task for at all. Same for actually
// stopping an already-running task (disabling/deleting a live camera) -
// there's no live teardown path yet, only spawn. Both call sites
// (webserver_cameras.cpp's saveCameraSubmission) are explicit about which
// case they're in and only call this for the one it's actually safe for:
// a slot that exists but has never had cameraStateInit()/a task before.
void spawnCameraTask(size_t index);
