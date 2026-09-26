#include <Arduino.h>
#include "camera_tasks.h"
#include "camera.h"
#include "config.h"
#include "event_log_store.h"
#include "telegram.h"
#include "telegram_i18n.h"

std::vector<CameraConfig> g_cameras;
std::vector<CameraState> g_cameraStates;

// Spawns g_cameras[index]'s monitoring task - see camera_tasks.h for the
// external-linkage contract (index must already be a valid, existing slot;
// this never grows g_cameras/g_cameraStates, only starts a task for a slot
// that doesn't have one yet). Called from startMonitoring()'s boot-time
// loop below for every enabled camera, from webserver_cameras.cpp's save
// handler for a single camera live when an edit newly enables one that had
// no task before, and from applyPendingNewCameraIfAny() below right after
// it push_backs a brand-new enabled camera's slot into existence.
void spawnCameraTask(size_t index) {
  cameraStateInit(g_cameraStates[index]); // must run before any other task can see this camera - see camera.h
  {
    // The web server may already be live and rendering /cameras
    // concurrently by the time this runs (true for both call sites - the
    // boot loop runs after startWebServer(), and the live-add path runs
    // while the server's obviously already up) - this write needs the
    // lock too, same as any other cross-task access.
    CameraStateLock lock(g_cameraStates[index]);
    g_cameraStates[index].alertsEnabled = loadAlertEnabledPref(index); // restore any /on or /off from before a reboot
  }
  CameraTaskContext* ctx = new CameraTaskContext{&g_cameras[index], &g_cameraStates[index]};
  char taskName[16];
  snprintf(taskName, sizeof(taskName), "cam%u", (unsigned)index);
  // 10KB stack covers the SOAP String churn plus a WiFiClientSecure TLS
  // handshake with headroom - bump if stack-canary warnings show up.
  // Pinned to core 1, same as Arduino's own loopTask (CONFIG_ARDUINO_
  // RUNNING_CORE) - a reconnect burst dilutes loopTask's share via normal
  // round-robin rather than starving it, but avoids contending with
  // ESP-IDF's WiFi/BT tasks, which stay pinned to core 0 regardless and
  // would be worse to compete with directly (hits every camera, the
  // dashboard, and Telegram, not just loopTask).
  BaseType_t created =
      xTaskCreatePinnedToCore(cameraTaskFn, taskName, 10240, ctx, tskIDLE_PRIORITY + 1, nullptr, 1);
  if (created != pdPASS) {
    // Genuine memory pressure (many cameras, a large PSRAM snapshot
    // history) can make task creation itself fail - previously silent,
    // leaving this camera with no task at all, indistinguishable on the
    // dashboard from "has a task but isn't subscribed yet" unless this is
    // surfaced loudly. Not a per-camera credential/config problem, so no
    // dashboard edit fixes it - only freeing memory (or a reboot) does.
    delete ctx; // never handed to a task, so nothing else will free it
    Serial.printf("[%s] FATAL: xTaskCreatePinnedToCore failed (out of memory?) - this camera will NOT "
                  "be monitored until the board is rebooted (ideally with more free memory).\n",
                  g_cameras[index].name.c_str());
    logEvent(g_cameras[index].name + ": task creation FAILED (out of memory?) - NOT being monitored");
    String cameraName = g_cameras[index].name;
    sendTelegramMessage([cameraName](TelegramLang lang) { return trCameraTaskSpawnFailure(lang, cameraName); });
  }
}

// Guards g_pendingNewCamera only - not g_cameras/g_cameraStates themselves
// (nothing needs to guard those; see applyPendingNewCameraIfAny's own
// comment for why only loop()'s task ever grows them). Same
// single-slot-pointer pattern as CameraState::pendingConfig, just for "a
// whole new camera" instead of "a live edit to an existing one" - staged
// by stagePendingNewCamera (webserver's task), claimed and cleared by
// applyPendingNewCameraIfAny (loop()'s task).
static SemaphoreHandle_t g_pendingNewCameraMutex = xSemaphoreCreateMutex();
static CameraConfig* g_pendingNewCamera = nullptr;

bool stagePendingNewCamera(const CameraConfig& cam) {
  // g_cameras.size() is read here, unlocked, from the webserver's own
  // task - benign the same way this project already accepts an unlocked
  // read of CameraConfig::enabled/.name elsewhere (webserver_cameras.cpp's
  // save handler comment): worst case this sees a stale count by one
  // camera and either stages a request applyPendingNewCameraIfAny will
  // itself re-check and reject, or (extremely unlikely - PsychicHttp only
  // ever has one save in flight at a time) declines to stage when there
  // was actually just enough room. Never a correctness problem either way.
  if (g_cameras.size() >= MAX_CAMERAS) return false;

  CameraConfig* copy = new CameraConfig(cam);
  CameraConfig* old = nullptr;
  xSemaphoreTake(g_pendingNewCameraMutex, portMAX_DELAY);
  old = g_pendingNewCamera; // shouldn't normally happen - see this function's header comment - but replace, don't leak
  g_pendingNewCamera = copy;
  xSemaphoreGive(g_pendingNewCameraMutex);
  delete old;
  return true;
}

void applyPendingNewCameraIfAny() {
  CameraConfig* pending = nullptr;
  xSemaphoreTake(g_pendingNewCameraMutex, portMAX_DELAY);
  pending = g_pendingNewCamera;
  g_pendingNewCamera = nullptr;
  xSemaphoreGive(g_pendingNewCameraMutex);
  if (!pending) return;

  if (g_cameras.size() >= MAX_CAMERAS) {
    // Shouldn't happen - stagePendingNewCamera already checked this - but
    // re-checked here too, on the one task that actually owns the decision
    // to grow these vectors, rather than trusting a check made by a
    // different task at a possibly-stale moment.
    Serial.printf("[%s] Dropped a staged camera add - reserved capacity (%u) already used up.\n",
                  pending->name.c_str(), (unsigned)MAX_CAMERAS);
    delete pending;
    return;
  }

  // Only ever grows here, on loop()'s own task - see camera_tasks.h's own
  // comment (this function's declaration) for why every other reader of
  // g_cameras/g_cameraStates' size()/data() being on this same task is
  // what makes that safe without a dedicated lock around the vectors
  // themselves.
  g_cameras.push_back(*pending);
  g_cameraStates.push_back(CameraState());
  size_t newIdx = g_cameras.size() - 1;
  bool enabled = pending->enabled;
  String name = pending->name;
  delete pending;

  if (enabled) {
    spawnCameraTask(newIdx); // cameraStateInit + alertsEnabled restore happen inside, same as the boot loop below
    logEvent(name + ": added via dashboard, monitoring started live");
  } else {
    Serial.printf("[%s] Disabled - no task created.\n", name.c_str());
  }
}
