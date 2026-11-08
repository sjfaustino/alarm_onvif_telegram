#include "camera_file_store.h" // keepStoredPasswordIfBlank
#include "webserver_internal.h"
#include "webserver_cameras.h"
#include "camera_store.h"
#include "format_utils.h"
#include "snapshot_history.h"
#include "telegram.h" // setAllCamerasAlertState, MotionDetectionKind

// Cameras page routes; the page content and its background jobs are in
// webserver_cameras.cpp.

void registerCameraRoutes(PsychicHttpServer& server) {
  server.on("/cameras", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    // One-time banner from /cameras/save. Escaped here, where it's rendered:
    // this route can be requested directly, and trusting a pre-escaped value
    // once allowed ?note=<script> (reflected XSS). The save builds it raw.
    String note = htmlEscape(request->getParam("note", ""));

    // From a discovery result's Add link; escaped when rendered.
    String prefillUrl = request->getParam("prefillUrl", "");
    CameraConfig prefill;
    CameraConfig* prefillPtr = nullptr;
    if (prefillUrl.length() > 0) {
      prefill.name = request->getParam("prefillName", "");
      prefill.deviceServiceUrl = prefillUrl;
      prefillPtr = &prefill;
    }
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, note, renderCamerasPanel(prefillPtr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/edit", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    for (auto& c : loadCameras()) {
      if (c.name.equalsIgnoreCase(name)) {
        CameraConfig prefill = c;
        prefill.pass = ""; // never populate a password field with the real value
        return response->send(
            200, "text/html",
            renderShell(Tab::Cameras, "", renderCamerasPanel(&prefill, true, g_liveCameras, g_liveStates)).c_str());
      }
    }
    return response->redirect("/cameras");
  });

  server.on("/cameras/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    CameraConfig submitted = parseCameraForm(request);
    String originalName = request->getParam("originalName", "");
    originalName.trim();

    String banner;
    String applyNote;
    if (!saveCameraSubmission(submitted, originalName, banner, applyNote, g_liveCameras, g_liveStates)) {
      submitted.pass = "";
      return response->send(
          200, "text/html",
          renderShell(Tab::Cameras, banner,
                      renderCamerasPanel(&submitted, originalName.length() > 0, g_liveCameras, g_liveStates))
              .c_str());
    }
    // Redirect (POST-redirect-GET) even with a note, so refresh doesn't
    // resubmit.
    String redirectUrl = "/cameras";
    if (applyNote.length() > 0) redirectUrl += "?note=" + urlEncode(applyNote);
    return response->redirect(redirectUrl.c_str());
  });

  server.on("/cameras/test", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    CameraConfig submitted = parseCameraForm(request);
    String originalName = request->getParam("originalName", "");
    originalName.trim();
    bool isEdit = originalName.length() > 0;

    CameraConfig testCfg = submitted;
    if (isEdit && testCfg.pass.length() == 0) keepStoredPasswordIfBlank(testCfg, originalName, loadCameras());
    // Background task; testCfg (with the resolved password) is copied.
    String banner = backgroundJobBanner(
        startTestConnectionAsync(testCfg),
        "Testing camera connection in the background - reload this page in a moment to see the result.",
        "A connection test is already running in the background - reload in a moment to see its result.",
        "Could not start the connection test - the device is low on memory right now. Try again in a moment.");

    submitted.pass = "";
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(&submitted, isEdit, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/discover", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Background task; returns immediately.
    String banner = backgroundJobBanner(
        startCameraDiscoveryAsync(), "Searching the network for cameras in the background.",
        "A network search is already running in the background - reload in a moment to see its result.",
        "Could not start the network search - the device is low on memory right now. Try again in a moment.");
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/test-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Background task; returns immediately.
    String banner = backgroundJobBanner(
        startTestAllCamerasAsync(), "Test started in the background.",
        "A test is already running in the background - reload in a moment to see its result.",
        "Could not start the test - the device is low on memory right now. Try again in a moment.");
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/test-alert", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    name.trim();

    // Unknown values fall back to Generic; "pet" sets isPetEvent instead.
    String kindParam = request->getParam("kind", "generic");
    MotionDetectionKind kind = MotionDetectionKind::Generic;
    bool isPetEvent = false;
    if (kindParam == "person") kind = MotionDetectionKind::Person;
    else if (kindParam == "vehicle") kind = MotionDetectionKind::Vehicle;
    else if (kindParam == "pet") isPetEvent = true;

    CameraConfig cfg;
    bool found = false;
    for (auto& c : loadCameras()) {
      if (c.name.equalsIgnoreCase(name)) { cfg = c; found = true; break; }
    }

    int idx = -1;
    if (g_liveCameras) {
      for (size_t i = 0; i < g_liveCameras->size(); i++) {
        if ((*g_liveCameras)[i].name.equalsIgnoreCase(name)) { idx = (int)i; break; }
      }
    }

    String banner;
    if (!found || idx < 0 || !g_liveStates || idx >= (int)g_liveStates->size() || !(*g_liveCameras)[idx].enabled) {
      // No live CameraState to send through.
      banner = "Camera \"" + htmlEscape(name) + "\" isn't currently running (disabled, or added since "
               "the last reboot) - nothing to test. Not sent.";
    } else {
      // Background task. Passes the live CameraState (not a copy) for its
      // resolved snapshot URI and credentials.
      banner = backgroundJobBanner(
          startTestAlertAsync(cfg, (*g_liveStates)[idx], kind, isPetEvent),
          "Sending test alert in the background - reload this page in a moment to see the result.",
          "A test alert is already being sent in the background - reload in a moment to see its result.",
          "Could not start sending the test alert - the device is low on memory right now. Try again in "
          "a moment.");
    }
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/mute-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String duration = request->getParam("duration", "");
    duration.trim();
    String result = setAllCamerasAlertState(g_liveCameras->data(), g_liveStates->data(), g_liveCameras->size(),
                                             false, duration, "the dashboard", TelegramLang::English);
    // Escape the result: a failure echoes the submitted duration text, and
    // banners are inserted unescaped (this was a reflected XSS).
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, htmlEscape(result), renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/unmute-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Unmute all is permanent, like a bare /on all.
    String result = setAllCamerasAlertState(g_liveCameras->data(), g_liveStates->data(), g_liveCameras->size(),
                                             true, "", "the dashboard", TelegramLang::English);
    // Escaped for consistency with mute-all, in case a duration is ever added.
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, htmlEscape(result), renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/quiet-hours-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String result = applyQuietHoursToAllCameras(request, g_liveCameras, g_liveStates);
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, result, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/person-alerts-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String result = applyPersonAlertsToAllCameras(request, g_liveCameras, g_liveStates);
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, result, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/vehicle-alerts-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String result = applyVehicleAlertsToAllCameras(request, g_liveCameras, g_liveStates);
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, result, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/pet-alerts-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String result = applyPetAlertsToAllCameras(request, g_liveCameras, g_liveStates);
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, result, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  // One history entry (SD or PSRAM ring, per snapshot_history.h); age 0 =
  // newest. Behind rate limiting and auth like every route - it's camera
  // footage.
  server.on("/cameras/snapshot", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    String name = request->getParam("name", "");
    long age = request->getParam("age", "0").toInt();
    int idx = -1;
    if (g_liveCameras) {
      for (size_t i = 0; i < g_liveCameras->size(); i++) {
        if ((*g_liveCameras)[i].name.equalsIgnoreCase(name)) { idx = (int)i; break; }
      }
    }
    if (idx < 0 || !g_liveStates || idx >= (int)g_liveStates->size() || age < 0) {
      return response->send(404, "text/plain", "No such camera.");
    }

    // The bytes are copied out, so nothing is held during send().
    uint8_t* copy = nullptr;
    size_t len = 0;
    bool ok = readCameraSnapshot((*g_liveCameras)[idx], (*g_liveStates)[idx], (size_t)age, &copy, &len);
    if (!ok || !copy || len == 0) {
      return response->send(404, "text/plain", "No snapshot captured yet for this camera.");
    }
    esp_err_t result = response->send(200, "image/jpeg", copy, len);
    free(copy);
    return result;
  });

  server.on("/delete", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    // Stop the task before removing the record, or it keeps alerting on a
    // camera the dashboard no longer lists.
    stopLiveCameraIfRunning(name, g_liveCameras, g_liveStates);
    deleteCamera(name);
    return response->redirect("/cameras");
  });
}
