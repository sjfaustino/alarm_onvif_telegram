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
//
// page is 0-indexed, 0 = the newest GALLERY_PAGE_SIZE snapshots - out-of-
// range values (a hand-edited URL past the last real page) just render an
// empty grid with "Newer" still offered, rather than erroring; there's
// nothing meaningfully wrong about asking for a page that doesn't exist
// yet (or no longer does, if snapshots were pruned since the link was
// generated).
static String renderCameraGrid(const String& cameraName, size_t page, std::vector<CameraConfig>* liveCameras,
                                std::vector<CameraState>* liveStates) {
  String html = "<h1>Gallery: " + htmlEscape(cameraName) + "</h1>";
  html += "<p><a href=\"/gallery\">&laquo; All cameras</a></p>";

  int idx = findLiveCameraIndex(liveCameras, cameraName);
  if (idx < 0 || !liveStates || idx >= (int)liveStates->size()) {
    html += "<p class=\"hint\">No such camera.</p>";
    return html;
  }

  size_t count = cameraSnapshotCount((*liveCameras)[idx], (*liveStates)[idx]);
  size_t startAge = page * GALLERY_PAGE_SIZE;
  size_t endAge = startAge + GALLERY_PAGE_SIZE; // exclusive; clamped against count below
  if (endAge > count) endAge = count;
  bool hasOlder = endAge < count;   // more snapshots exist past this page
  bool hasNewer = page > 0;         // a more-recent page exists

  if (startAge >= count && count > 0) {
    html += "<p class=\"hint\">No snapshots on this page - only " + String((unsigned)count) +
            " stored in total.</p>";
  } else if (count > GALLERY_PAGE_SIZE) {
    html += "<p class=\"hint\">Showing " + String((unsigned)(startAge + 1)) + "-" + String((unsigned)endAge) +
            " of " + String((unsigned)count) + " stored snapshots.</p>";
  }

  unsigned long renderMs = millis(); // one shared cache-busting value for this page load
  html += "<div>";
  for (size_t age = startAge; age < endAge; age++) {
    String url = "/cameras/snapshot?name=" + urlEncode(cameraName) + "&age=" + String((unsigned)age) +
                 "&t=" + String(renderMs);
    html += "<a href=\"" + url + "\" target=\"_blank\">"
            "<img src=\"" + url + "\" style=\"max-width:160px;max-height:120px;margin:4px;\" "
            "alt=\"snapshot\"></a>";
  }
  html += "</div>";

  if (hasNewer || hasOlder) {
    String base = "/gallery?camera=" + urlEncode(cameraName) + "&page=";
    html += "<p>";
    if (hasNewer) html += "<a href=\"" + base + String((unsigned)(page - 1)) + "\">&laquo; Newer</a> ";
    if (hasOlder) html += "<a href=\"" + base + String((unsigned)(page + 1)) + "\">Older &raquo;</a>";
    html += "</p>";
  }
  return html;
}

String renderGalleryPanel(const String& cameraFilter, size_t page, std::vector<CameraConfig>* liveCameras,
                           std::vector<CameraState>* liveStates) {
  if (cameraFilter.length() == 0) return renderCameraPicker(liveCameras, liveStates);
  return renderCameraGrid(cameraFilter, page, liveCameras, liveStates);
}
