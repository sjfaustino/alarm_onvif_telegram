#include "webserver_maintenance.h"

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
