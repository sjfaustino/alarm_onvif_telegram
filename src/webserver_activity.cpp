#include "webserver_activity.h"
#include "event_log_store.h"
#include "format_utils.h"
#include "sd_store.h"
#include "config.h" // ACTIVITY_LOG_MAX_BYTES

String renderActivityPanel() {
  std::vector<EventLogEntry> events = recentEvents(); // oldest-first

  String html = "<h1>Activity</h1>";
  html += "<p class=\"hint\">The most recent " + String((unsigned)EVENT_LOG_CAPACITY) +
          " events since this board's last boot - this page only, not persisted, resets on reboot.</p>";
  if (sdActive()) {
    html += "<p class=\"hint\">Also persisted to SD (<code>/activity.log</code>, capped at " +
            String((unsigned)(ACTIVITY_LOG_MAX_BYTES / 1024)) +
            "KB - oldest entries are dropped by wiping and restarting the file once that's exceeded) - "
            "<a href=\"/activity/download\">download the full persisted history</a>.</p>";
  }
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
