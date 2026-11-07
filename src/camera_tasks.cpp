#include <Arduino.h>
#include "camera_tasks.h"
#include "camera.h"
#include "config.h"
#include "event_log_store.h"
#include "telegram.h"
#include "telegram_i18n.h"

std::vector<CameraConfig> g_cameras;
std::vector<CameraState> g_cameraStates;

// Camera task stacks are in PSRAM, which must be freed with vTaskDeleteWithCaps
// and preferably not by the task itself. A stopping task suspends itself instead,
// and it's deleted here, from loop() or from spawnCameraTask reusing the slot.
static SemaphoreHandle_t g_reapMutex = xSemaphoreCreateMutex();

// Caller holds g_reapMutex.
static void reapIfStopped(size_t index) {
  TaskHandle_t handle = g_cameraStates[index].taskHandle;
  if (handle && eTaskGetState(handle) == eSuspended) {
    vTaskDeleteWithCaps(handle);
    g_cameraStates[index].taskHandle = nullptr;
  }
}

void reapStoppedCameraTasks() {
  xSemaphoreTake(g_reapMutex, portMAX_DELAY);
  for (size_t i = 0; i < g_cameraStates.size(); i++) reapIfStopped(i);
  xSemaphoreGive(g_reapMutex);
}

bool cameraTaskAlive(size_t index) {
  xSemaphoreTake(g_reapMutex, portMAX_DELAY);
  TaskHandle_t handle = index < g_cameraStates.size() ? g_cameraStates[index].taskHandle : nullptr;
  bool alive = handle && eTaskGetState(handle) != eSuspended;
  xSemaphoreGive(g_reapMutex);
  return alive;
}

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
  // stay off the core-0 WiFi tasks. The stack is in PSRAM: with 8 cameras,
  // internal-RAM stacks left too little for a TLS handshake (free heap fell to
  // 6.7KB during a photo send). Such a task must never touch flash - see
  // loadTelegramUsers.
  xSemaphoreTake(g_reapMutex, portMAX_DELAY);
  reapIfStopped(index); // a previous task in this slot may still be awaiting deletion
  BaseType_t created = xTaskCreatePinnedToCoreWithCaps(cameraTaskFn, taskName, 10240, ctx, tskIDLE_PRIORITY + 1,
                                                       &g_cameraStates[index].taskHandle, 1, MALLOC_CAP_SPIRAM);
  xSemaphoreGive(g_reapMutex);
  if (created != pdPASS) {
    // Task creation can fail under memory pressure; say so loudly, since only
    // freeing memory or rebooting fixes it.
    delete ctx; // never handed to a task, so nothing else will free it
    Serial.printf("[%s] FATAL: camera task creation failed (out of memory?) - this camera will NOT "
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

bool isPendingNewCamera(const String& name) {
  xSemaphoreTake(g_pendingNewCameraMutex, portMAX_DELAY);
  bool pending = g_pendingNewCamera && g_pendingNewCamera->name.equalsIgnoreCase(name);
  xSemaphoreGive(g_pendingNewCameraMutex);
  return pending;
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
