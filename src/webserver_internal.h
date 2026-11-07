#pragma once
// Shared by the webserver*.cpp files only - not part of webserver.h's API.
#include <Arduino.h>
#include <PsychicHttp.h>
#include <vector>
#include "auth_store.h"
#include "background_job.h"
#include "camera.h"
#include "camera_store.h"

enum class Tab { None, Network, Cameras, Users, Activity, Gallery, Firmware, Maintenance, Storage, Security,
                  HardwareInternet, HardwareBridge, HardwarePower, Capabilities };

// Set by startWebServer().
extern std::vector<CameraConfig>* g_liveCameras;
extern std::vector<CameraState>* g_liveStates;

// The page around contentHtml: sidebar, banner (inserted unescaped) and
// auto-refresh.
String renderShell(Tab active, const String& banner, const String& contentHtml);
String backgroundJobBanner(BackgroundJobStartOutcome outcome, const String& startedText,
                           const String& alreadyRunningText, const String& failedText);
void delayedRebootTask(void*);
// Updates the login enforced on the next request.
void applyDashboardLogin(const DashboardAuth& auth);

// One per page, each in that page's own file.
void registerNetworkRoutes(PsychicHttpServer& server);
void registerCameraRoutes(PsychicHttpServer& server);
void registerUserRoutes(PsychicHttpServer& server);
void registerActivityRoutes(PsychicHttpServer& server);
void registerGalleryRoutes(PsychicHttpServer& server);
void registerFirmwareRoutes(PsychicHttpServer& server);
void registerMaintenanceRoutes(PsychicHttpServer& server);
void registerHardwareRoutes(PsychicHttpServer& server);
void registerStorageRoutes(PsychicHttpServer& server);
void registerCapabilitiesRoutes(PsychicHttpServer& server);
void registerSecurityRoutes(PsychicHttpServer& server);
