#include "webserver_activity.h"
#include "event_log_store.h"
#include "format_utils.h"

String renderActivityPanel() {
  std::vector<EventLogEntry> events = recentEvents(); // oldest-first

  String html = "<h1>Activity</h1>";
  html += "<p class=\"hint\">The most recent " + String((unsigned)EVENT_LOG_CAPACITY) +
          " events since this board's last boot - not persisted, resets on reboot.</p>";
  html += "<table><tr><th>When</th><th>Event</th></tr>";

  if (events.empty()) {
    html += "<tr><td colspan=\"2\">Nothing logged yet.</td></tr>";
  } else {
    // Newest first for display - reverse iteration over the oldest-first
    // storage order, not a copy/sort.
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
      html += "<tr><td>" + formatElapsedSince(it->ms, millis()) + "</td><td>" + htmlEscape(it->text) +
              "</td></tr>";
    }
  }
  html += "</table>";
  return html;
}
