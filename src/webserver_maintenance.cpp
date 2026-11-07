#include "webserver_maintenance.h"
#include "webserver_internal.h"

String renderMaintenancePanel() {
  String html = "<h1>Maintenance</h1>";

  html += "<fieldset><legend>Reboot</legend>";
  html += "<form method=\"POST\" action=\"/maintenance/reboot\" "
          "onsubmit=\"return confirm('Reboot the board now? Every camera stops being monitored "
          "until it finishes reconnecting.');\">";
  html += "<p><button type=\"submit\" class=\"danger\">Reboot now</button></p>";
  html += "</form>";
  html += "<p class=\"hint\">Takes about 15-20 seconds to fully reconnect and resume monitoring - "
          "same as a power cycle, and just as disruptive to every camera's active subscription while "
          "it's down.</p>";
  html += "</fieldset>";

  return html;
}

void registerMaintenanceRoutes(PsychicHttpServer& server) {
  server.on("/maintenance", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Maintenance, "", renderMaintenancePanel()).c_str());
  });

  server.on("/maintenance/reboot", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    Serial.println("[Maintenance] Reboot requested via dashboard.");
    esp_err_t result = response->send(
        200, "text/html",
        renderShell(Tab::Maintenance, "Rebooting now - reconnect in about 15-20 seconds.",
                    renderMaintenancePanel())
            .c_str());
    // Same as the OTA reboot (webserver_firmware.cpp): log only.
    if (xTaskCreate(delayedRebootTask, "maintReboot", 2048, nullptr, 1, nullptr) != pdPASS) {
      Serial.println("[Maintenance] ERROR: failed to start the reboot task (out of memory?) - the "
                      "board will NOT reboot. Try again once memory frees up, or power-cycle manually.");
    }
    return result;
  });
}
