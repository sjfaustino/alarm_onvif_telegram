#include "webserver_capabilities.h"
#include "format_utils.h" // htmlEscape

String renderCapabilitiesPanel(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  String html = "<h1>Capabilities</h1>";
  html += "<p class=\"hint\">What each camera's own ONVIF event schema actually advertises - a camera "
          "only shows up here once it's successfully connected at least once this boot "
          "(cameraSetupSequence). \"Known\" is what this firmware already acts on for that camera; "
          "\"Also advertises\" lists anything else the schema mentions that isn't wired up to anything "
          "yet - candidates for a future feature. A camera whose event schema doesn't respond at all "
          "(or hasn't run yet) shows a note instead of a blank row.</p>";

  html += "<table><tr><th>Camera</th><th>Status</th><th>Known topics</th><th>Also advertises "
          "(not yet used)</th></tr>";

  if (!liveCameras || liveCameras->size() == 0) {
    html += "<tr><td colspan=\"4\">No cameras configured.</td></tr>";
    html += "</table>";
    return html;
  }

  for (size_t i = 0; i < liveCameras->size(); i++) {
    CameraConfig& c = (*liveCameras)[i];
    html += "<tr><td>" + htmlEscape(c.name) + "</td>";

    bool isLive = liveStates && i < liveStates->size() && c.enabled;
    if (!isLive) {
      html += "<td colspan=\"3\" class=\"hint\">" +
              String(c.enabled ? "Not currently running (added since this board's last boot) - reboot to "
                                  "check its capabilities."
                                : "Disabled.") +
              "</td></tr>";
      continue;
    }

    String supportedEventTopics, unusedEventTopics;
    {
      CameraState& st = (*liveStates)[i];
      CameraStateLock lock(st);
      supportedEventTopics = st.supportedEventTopics;
      unusedEventTopics = st.unusedEventTopics;
    }

    if (supportedEventTopics.length() == 0 && unusedEventTopics.length() == 0) {
      html += "<td class=\"hint\">Running</td>"
              "<td colspan=\"2\" class=\"hint\">Event schema not read yet, or this camera's event "
              "service hasn't responded successfully - see the Cameras page for its current "
              "subscription status.</td></tr>";
      continue;
    }

    html += "<td><span class=\"badge badge-on\">Running</span></td>";
    html += "<td>" + (supportedEventTopics.length() > 0 ? htmlEscape(supportedEventTopics)
                                                          : String("<span class=\"hint\">none</span>")) +
            "</td>";
    html += "<td>" + (unusedEventTopics.length() > 0 ? htmlEscape(unusedEventTopics)
                                                       : String("<span class=\"hint\">none</span>")) +
            "</td></tr>";
  }

  html += "</table>";
  return html;
}
