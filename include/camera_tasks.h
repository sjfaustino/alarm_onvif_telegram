#pragma once
#include <cstddef>

// Spawns cameras[index]'s FreeRTOS monitoring task - same as
// startMonitoring() (main.cpp) at boot, exposed for webserver_cameras.cpp
// to spawn one live when an edit enables a previously-disabled camera.
// index must be a slot that's never had a task before - no live teardown
// path exists yet (see the TODO below), so this must never be called for
// an already-running slot.
//
// TODO: no live teardown path exists for a brand-new camera or a
// disabled/deleted running one - both still require a reboot. Would need
// a way to signal a running cameraTaskFn to unsubscribe/vTaskDelete
// itself, plus growable g_cameras/g_cameraStates for the brand-new case.
void spawnCameraTask(size_t index);
