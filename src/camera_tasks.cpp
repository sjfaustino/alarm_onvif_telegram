#include <Arduino.h>
#include "camera_tasks.h"
#include "camera.h"
#include "config.h"
#include "event_log_store.h"
#include "telegram.h"
#include "telegram_i18n.h"

std::vector<CameraConfig> g_cameras;
std::vector<CameraState> g_cameraStates;

// Starts a task for an existing slot (boot, live re-enable, or live add).
void spawnCameraTask(size_t index) {
  cameraStateInit(g_cameraStates[index]); // must run before any other task can see this camera - see camera.h
  {
    // The web server may already be reading this slot.
    CameraStateLock lock(g_cameraStates[index]);
    g_cameraStates[index].alertsEnabled = loadAlertEnabledPref(index); // restore any /on or /off from before a reboot
  }
  CameraTaskContext* ctx = new CameraTaskContext{&g_cameras[index], &g_cameraStates[index]};
  char taskName[16];
  snprintf(taskName, sizeof(taskName), "cam%u", (unsigned)index);
  // 10KB covers SOAP strings plus a TLS handshake. Core 1, like loopTask, to
  // stay off the core-0 WiFi tasks.
  BaseType_t created =
      xTaskCreatePinnedToCore(cameraTaskFn, taskName, 10240, ctx, tskIDLE_PRIORITY + 1, nullptr, 1);
  if (created != pdPASS) {
    // Task creation can fail under memory pressure; say so loudly, since only
    // freeing memory or rebooting fixes it.
    delete ctx; // never handed to a task, so nothing else will free it
    Serial.printf("[%s] FATAL: xTaskCreatePinnedToCore failed (out of memory?) - this camera will NOT "
                  "be monitored until the board is rebooted (ideally with more free memory).\n",
                  g_cameras[index].name.c_str());
    logEvent(g_cameras[index].name + ": task creation FAILED (out of memory?) - NOT being monitored");
    String cameraName = g_cameras[index].name;
    sendTelegramMessage([cameraName](TelegramLang lang) { return trCameraTaskSpawnFailure(lang, cameraName); });
  }
}

// Guards only the single staged-camera slot, handed from the web server's task
// to loop()'s.
static SemaphoreHandle_t g_pendingNewCameraMutex = xSemaphoreCreateMutex();
static CameraConfig* g_pendingNewCamera = nullptr;

bool stagePendingNewCamera(const CameraConfig& cam) {
  // Unlocked size read from the web server task: at worst stale by one, and
  // applyPendingNewCameraIfAny re-checks.
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
    // Re-checked on the task that owns growing the vectors.
    Serial.printf("[%s] Dropped a staged camera add - reserved capacity (%u) already used up.\n",
                  pending->name.c_str(), (unsigned)MAX_CAMERAS);
    delete pending;
    return;
  }

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
