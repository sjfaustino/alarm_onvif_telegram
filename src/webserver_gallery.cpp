#include "webserver_gallery.h"
#include "snapshot_history.h"
#include "snapshot_source.h" // snapshotSourceLabel - per-thumbnail caption
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

// Every SnapshotSource value, for the filter bar below - enum class
// doesn't support iteration, so this is the one place that has to spell
// out all eight by hand. Keep in sync with snapshot_source.h's enum.
static const SnapshotSource kAllSnapshotSources[] = {
    SnapshotSource::Motion, SnapshotSource::Person,    SnapshotSource::Vehicle, SnapshotSource::Pet,
    SnapshotSource::Tamper, SnapshotSource::Timelapse, SnapshotSource::Test,   SnapshotSource::Manual,
};

// This camera's thumbnail grid, optionally filtered to one SnapshotSource
// (sourceFilter == "" shows everything) - reuses the existing
// /cameras/snapshot route (webserver.cpp) for every image, see
// webserver_gallery.h's own comment for why. Each thumbnail's <img>
// independently re-triggers that route's full SD directory listing
// (sd_store.cpp's readSdSnapshot) - accepted, documented cost, consistent
// with this project's "webserver operations are lower priority than
// camera SD ops" stance elsewhere; the GALLERY_PAGE_SIZE cap below bounds
// how many times that happens per page load.
//
// page is 0-indexed, 0 = the newest GALLERY_PAGE_SIZE snapshots (of
// whichever set sourceFilter selects) - out-of-range values (a hand-
// edited URL past the last real page) just render an empty grid with
// "Newer" still offered, rather than erroring; there's nothing
// meaningfully wrong about asking for a page that doesn't exist yet (or
// no longer does, if snapshots were pruned since the link was generated).
static String renderCameraGrid(const String& cameraName, size_t page, const String& sourceFilter,
                                std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  String html = "<h1>Gallery: " + htmlEscape(cameraName) + "</h1>";
  html += "<p><a href=\"/gallery\">&laquo; All cameras</a></p>";

  int idx = findLiveCameraIndex(liveCameras, cameraName);
  if (idx < 0 || !liveStates || idx >= (int)liveStates->size()) {
    html += "<p class=\"hint\">No such camera.</p>";
    return html;
  }
  CameraConfig& cfg = (*liveCameras)[idx];
  CameraState& st = (*liveStates)[idx];

  // Newest-first, index i == age i - one bulk fetch (one SD directory
  // listing) instead of calling cameraSnapshotSourceAt once per entry,
  // since the filter bar's per-kind counts below need to inspect every
  // stored entry anyway.
  std::vector<SnapshotSource> sources = cameraSnapshotSourcesAll(cfg, st);

  size_t countByKind[8] = {0};
  for (SnapshotSource s : sources) countByKind[(size_t)s]++;

  String baseUrl = "/gallery?camera=" + urlEncode(cameraName);
  html += "<p>";
  html += sourceFilter.length() == 0
      ? ("<strong>All (" + String((unsigned)sources.size()) + ")</strong> ")
      : ("<a href=\"" + baseUrl + "\">All (" + String((unsigned)sources.size()) + ")</a> ");
  // Only a kind this camera actually has at least one stored snapshot of
  // gets a link - an idle camera (never tampered with, no timelapse
  // configured) shouldn't show a wall of empty filter options.
  for (SnapshotSource s : kAllSnapshotSources) {
    size_t n = countByKind[(size_t)s];
    if (n == 0) continue;
    String label = snapshotSourceLabel(s);
    String linkUrl = baseUrl + "&source=" + urlEncode(label);
    html += sourceFilter == label
        ? ("<strong>" + label + " (" + String((unsigned)n) + ")</strong> ")
        : ("<a href=\"" + linkUrl + "\">" + label + " (" + String((unsigned)n) + ")</a> ");
  }
  html += "</p>";

  // Ages to actually display, newest-first - every stored age when
  // sourceFilter is empty, or just the ages matching it otherwise. Kept
  // as real age values (not re-derived from a raw count), so the
  // thumbnails below always request the right entry from the unfiltered
  // backing store regardless of which subset is being paged through.
  std::vector<size_t> displayAges;
  if (sourceFilter.length() == 0) {
    displayAges.reserve(sources.size());
    for (size_t age = 0; age < sources.size(); age++) displayAges.push_back(age);
  } else {
    SnapshotSource wanted = snapshotSourceFromLabel(sourceFilter);
    for (size_t age = 0; age < sources.size(); age++) {
      if (sources[age] == wanted) displayAges.push_back(age);
    }
  }

  size_t count = displayAges.size();
  size_t startAge = page * GALLERY_PAGE_SIZE;
  size_t endAge = startAge + GALLERY_PAGE_SIZE; // exclusive; clamped against count below
  if (endAge > count) endAge = count;
  bool hasOlder = endAge < count;   // more snapshots exist past this page
  bool hasNewer = page > 0;         // a more-recent page exists

  String filterSuffix = sourceFilter.length() > 0 ? (" for \"" + htmlEscape(sourceFilter) + "\"") : "";
  if (count == 0) {
    html += "<p class=\"hint\">No stored snapshots" + filterSuffix + ".</p>";
  } else if (startAge >= count) {
    html += "<p class=\"hint\">No snapshots on this page - only " + String((unsigned)count) +
            " stored" + filterSuffix + ".</p>";
  } else if (count > GALLERY_PAGE_SIZE) {
    html += "<p class=\"hint\">Showing " + String((unsigned)(startAge + 1)) + "-" + String((unsigned)endAge) +
            " of " + String((unsigned)count) + " stored snapshots" + filterSuffix + ".</p>";
  }

  unsigned long renderMs = millis(); // one shared cache-busting value for this page load
  html += "<div>";
  for (size_t i = startAge; i < endAge; i++) {
    size_t age = displayAges[i];
    String url = "/cameras/snapshot?name=" + urlEncode(cameraName) + "&age=" + String((unsigned)age) +
                 "&t=" + String(renderMs);
    // Visible caption, unlike the Cameras page's own tiny 48px Preview
    // strip (webserver_cameras.cpp uses a title-attribute tooltip there
    // instead, no room for text) - this grid's whole purpose is browsing
    // history, so "what triggered this" is worth a permanent label, not
    // just a hover.
    String sourceLabel = htmlEscape(snapshotSourceLabel(sources[age]));
    html += "<span style=\"display:inline-block;margin:4px;text-align:center;\">"
            "<a href=\"" + url + "\" target=\"_blank\">"
            "<img src=\"" + url + "\" style=\"display:block;max-width:160px;max-height:120px;\" "
            "alt=\"snapshot\"></a>"
            "<span class=\"hint\">" + sourceLabel + "</span></span>";
  }
  html += "</div>";

  if (hasNewer || hasOlder) {
    String base = baseUrl + (sourceFilter.length() > 0 ? ("&source=" + urlEncode(sourceFilter)) : "") + "&page=";
    html += "<p>";
    if (hasNewer) html += "<a href=\"" + base + String((unsigned)(page - 1)) + "\" class=\"secondary\">"
                           "&laquo; Newer</a> ";
    if (hasOlder) html += "<a href=\"" + base + String((unsigned)(page + 1)) + "\" class=\"secondary\">"
                           "Older &raquo;</a>";
    html += "</p>";
  }
  return html;
}

String renderGalleryPanel(const String& cameraFilter, size_t page, const String& sourceFilter,
                           std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  if (cameraFilter.length() == 0) return renderCameraPicker(liveCameras, liveStates);
  return renderCameraGrid(cameraFilter, page, sourceFilter, liveCameras, liveStates);
}
