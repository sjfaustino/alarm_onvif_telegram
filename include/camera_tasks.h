#pragma once
#include <cstddef>

// Spawns cameras[index]'s FreeRTOS monitoring task - same as
// startMonitoring() (main.cpp) at boot, exposed for webserver_cameras.cpp
// to spawn one live when an edit enables a previously-disabled camera.
// index must be a slot with no task CURRENTLY running - either never
// spawned before, or one requestCameraStop() (camera.h) has since torn
// down; calling this while a task still owns the slot would create two
// tasks racing over the same CameraConfig/CameraState.
//
// TODO: a brand-new camera (added after this board's current boot, not
// yet occupying any slot at all) still needs a reboot - g_cameras/
// g_cameraStates are sized once at boot and never grow, since CameraState
// ::user/pass and every CameraTaskContext hold raw pointers into their
// elements that a resize would invalidate.
void spawnCameraTask(size_t index);
