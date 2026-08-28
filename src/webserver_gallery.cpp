#include "webserver_gallery.h"
#include "snapshot_history.h"
#include "sd_store.h"
#include "format_utils.h"
#include "config.h" // GALLERY_PAGE_SIZE

static int findLiveCameraIndex(std::vector<CameraConfig>* liveCameras, const String& name) {
  if (!liveCameras) return -1;
  for (size_t i = 0; i < liveCameras->size(); i++) {
    if ((*liveCameras)[i].name.equalsIgnoreCase(name)) return (int)i;
  }
  return -1;
}

// Camera picker - every camera with any stored history, linking to its
// own thumbnail grid.
static String renderCameraPicker(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  String html = "<h1>Gallery</h1>";
  if (!sdActive()) {
    html += "<p class=\"hint\">SD storage isn't active - the Cameras page's Preview column already "
            "shows everything available (the last " + String((unsigned)SNAPSHOT_HISTORY_SIZE) +
            " per camera). Enable SD storage (System &gt; Storage) for a larger, browsable history "
            "here.</p>";
  }

  bool any = false;
  html += "<table><tr><th>Camera</th><th>Stored snapshots</th></tr>";
  if (liveCameras && liveStates) {
    for (size_t i = 0; i < liveCameras->size() && i < liveStates->size(); i++) {
      size_t count = cameraSnapshotCount((*liveCameras)[i], (*liveStates)[i]);
      if (count == 0) continue;
      any = true;
      html += "<tr><td><a href=\"/gallery?camera=" + urlEncode((*liveCameras)[i].name) + "\">" +
              htmlEscape((*liveCameras)[i].name) + "</a></td><td>" + String((unsigned)count) + "</td></tr>";
    }
  }
  if (!any) html += "<tr><td colspan=\"2\">No stored snapshots yet.</td></tr>";
  html += "</table>";
  return html;
}

// This camera's thumbnail grid - reuses the existing /cameras/snapshot
// route (webserver.cpp) for every image, see webserver_gallery.h's own
// comment for why. Each thumbnail's <img> independently re-triggers that
// route's full SD directory listing (sd_store.cpp's readSdSnapshot) -
// accepted, documented cost, consistent with this project's "webserver
// operations are lower priority than camera SD ops" stance elsewhere; the
// GALLERY_PAGE_SIZE cap below bounds how many times that happens per page
// load.
static String renderCameraGrid(const String& cameraName, std::vector<CameraConfig>* liveCameras,
                                std::vector<CameraState>* liveStates) {
  String html = "<h1>Gallery: " + htmlEscape(cameraName) + "</h1>";
  html += "<p><a href=\"/gallery\">&laquo; All cameras</a></p>";

  int idx = findLiveCameraIndex(liveCameras, cameraName);
  if (idx < 0 || !liveStates || idx >= (int)liveStates->size()) {
    html += "<p class=\"hint\">No such camera.</p>";
    return html;
  }

  size_t count = cameraSnapshotCount((*liveCameras)[idx], (*liveStates)[idx]);
  size_t shown = count < GALLERY_PAGE_SIZE ? count : GALLERY_PAGE_SIZE;
  if (count > GALLERY_PAGE_SIZE) {
    html += "<p class=\"hint\">Showing the newest " + String((unsigned)GALLERY_PAGE_SIZE) + " of " +
            String((unsigned)count) + " stored snapshots.</p>";
  }

  unsigned long renderMs = millis(); // one shared cache-busting value for this page load
  html += "<div>";
  for (size_t age = 0; age < shown; age++) {
    String url = "/cameras/snapshot?name=" + urlEncode(cameraName) + "&age=" + String((unsigned)age) +
                 "&t=" + String(renderMs);
    html += "<a href=\"" + url + "\" target=\"_blank\">"
            "<img src=\"" + url + "\" style=\"max-width:160px;max-height:120px;margin:4px;\" "
            "alt=\"snapshot\"></a>";
  }
  html += "</div>";
  return html;
}

String renderGalleryPanel(const String& cameraFilter, std::vector<CameraConfig>* liveCameras,
                           std::vector<CameraState>* liveStates) {
  if (cameraFilter.length() == 0) return renderCameraPicker(liveCameras, liveStates);
  return renderCameraGrid(cameraFilter, liveCameras, liveStates);
}
