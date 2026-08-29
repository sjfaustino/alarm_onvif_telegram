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
//
// TODO: no live teardown path exists for the two cases above (a brand-new
// camera, or disabling/deleting one whose task is already running) - both
// still require a reboot, and saveCameraSubmission's applyNote says so
// explicitly rather than silently pretending the change applied. Adding
// live teardown would need a way to signal a running cameraTaskFn to
// unsubscribe and vTaskDelete itself, plus making g_cameras/g_cameraStates
// growable (or pre-sized past what's currently configured) for the
// brand-new-camera case - real work, not attempted yet.
void spawnCameraTask(size_t index);
