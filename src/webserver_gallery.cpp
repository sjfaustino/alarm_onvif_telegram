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

// "YYYYMMDD" -> "YYYY-MM-DD", for display only - entries/URLs keep the raw
// 8-digit form (a plain string compare against SnapshotEntryInfo::date),
// this is purely so the date links read like a date instead of a bare
// number.
static String formatDateForDisplay(const String& yyyymmdd) {
  if (yyyymmdd.length() != 8) return yyyymmdd; // defensive - never actually reached in practice
  return yyyymmdd.substring(0, 4) + "-" + yyyymmdd.substring(4, 6) + "-" + yyyymmdd.substring(6, 8);
}

// This camera's thumbnail grid, optionally filtered to one SnapshotSource
// and/or one capture date (sourceFilter/dateFilter == "" shows everything
// for that axis - both apply together, AND'd) - reuses the existing
// /cameras/snapshot route (webserver.cpp) for every image, see
// webserver_gallery.h's own comment for why. Each thumbnail's <img>
// independently re-triggers that route's full SD directory listing
// (sd_store.cpp's readSdSnapshot) - accepted, documented cost, consistent
// with this project's "webserver operations are lower priority than
// camera SD ops" stance elsewhere; the GALLERY_PAGE_SIZE cap below bounds
// how many times that happens per page load.
//
// page is 0-indexed, 0 = the newest GALLERY_PAGE_SIZE snapshots (of
// whichever set the filters select) - out-of-range values (a hand-edited
// URL past the last real page) just render an empty grid with "Newer"
// still offered, rather than erroring; there's nothing meaningfully wrong
// about asking for a page that doesn't exist yet (or no longer does, if
// snapshots were pruned since the link was generated).
static String renderCameraGrid(const String& cameraName, size_t page, const String& sourceFilter,
                                const String& dateFilter, std::vector<CameraConfig>* liveCameras,
                                std::vector<CameraState>* liveStates) {
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
  // listing) instead of calling cameraSnapshotSourceAt/date-parsing once
  // per entry, since the filter bars' own counts below need to inspect
  // every stored entry anyway.
  std::vector<SnapshotEntryInfo> entries = cameraSnapshotEntriesAll(cfg, st);

  size_t countByKind[8] = {0};
  for (auto& e : entries) countByKind[(size_t)e.source]++;

  // Distinct capture dates this camera actually has, newest-first for
  // free (entries are already newest-first; only the first time a date is
  // seen is a new distinct entry) - "" (PSRAM ring, or an unparseable
  // legacy filename) never becomes a filter option. Counts too, same
  // reasoning as the per-kind bar below.
  std::vector<String> distinctDates;
  std::vector<size_t> countByDate;
  for (auto& e : entries) {
    if (e.date.length() == 0) continue;
    bool found = false;
    for (size_t i = 0; i < distinctDates.size(); i++) {
      if (distinctDates[i] == e.date) { countByDate[i]++; found = true; break; }
    }
    if (!found) { distinctDates.push_back(e.date); countByDate.push_back(1); }
  }

  String baseUrl = "/gallery?camera=" + urlEncode(cameraName);
  // Carries whichever OTHER filter is active along with a link, so
  // clicking a kind doesn't silently drop an active date filter (or vice
  // versa) - both axes are meant to combine, not replace each other.
  String dateQueryParam = dateFilter.length() > 0 ? ("&date=" + urlEncode(dateFilter)) : "";
  String sourceQueryParam = sourceFilter.length() > 0 ? ("&source=" + urlEncode(sourceFilter)) : "";

  html += "<p>";
  html += sourceFilter.length() == 0
      ? ("<strong>All (" + String((unsigned)entries.size()) + ")</strong> ")
      : ("<a href=\"" + baseUrl + dateQueryParam + "\">All (" + String((unsigned)entries.size()) + ")</a> ");
  // Only a kind this camera actually has at least one stored snapshot of
  // gets a link - an idle camera (never tampered with, no timelapse
  // configured) shouldn't show a wall of empty filter options.
  for (SnapshotSource s : kAllSnapshotSources) {
    size_t n = countByKind[(size_t)s];
    if (n == 0) continue;
    String label = snapshotSourceLabel(s);
    String linkUrl = baseUrl + "&source=" + urlEncode(label) + dateQueryParam;
    html += sourceFilter == label
        ? ("<strong>" + label + " (" + String((unsigned)n) + ")</strong> ")
        : ("<a href=\"" + linkUrl + "\">" + label + " (" + String((unsigned)n) + ")</a> ");
  }
  html += "</p>";

  // Browse-by-date bar - only shown if this camera has at least one
  // dated entry (SD-backed history; the PSRAM ring never offers any).
  if (!distinctDates.empty()) {
    html += "<p>";
    html += dateFilter.length() == 0 ? "<strong>All dates</strong> "
                                      : ("<a href=\"" + baseUrl + sourceQueryParam + "\">All dates</a> ");
    for (size_t i = 0; i < distinctDates.size(); i++) {
      String linkUrl = baseUrl + "&date=" + urlEncode(distinctDates[i]) + sourceQueryParam;
      String label = formatDateForDisplay(distinctDates[i]) + " (" + String((unsigned)countByDate[i]) + ")";
      html += dateFilter == distinctDates[i] ? ("<strong>" + label + "</strong> ")
                                              : ("<a href=\"" + linkUrl + "\">" + label + "</a> ");
    }
    html += "</p>";
  }

  // Ages to actually display, newest-first - every stored age whose
  // source (if sourceFilter is set) AND date (if dateFilter is set)
  // match. Kept as real age values (not re-derived from a raw count), so
  // the thumbnails below always request the right entry from the
  // unfiltered backing store regardless of which subset is being paged
  // through.
  SnapshotSource wantedSource = sourceFilter.length() > 0 ? snapshotSourceFromLabel(sourceFilter)
                                                           : SnapshotSource::Motion;
  std::vector<size_t> displayAges;
  displayAges.reserve(entries.size());
  for (size_t age = 0; age < entries.size(); age++) {
    if (sourceFilter.length() > 0 && entries[age].source != wantedSource) continue;
    if (dateFilter.length() > 0 && entries[age].date != dateFilter) continue;
    displayAges.push_back(age);
  }

  size_t count = displayAges.size();
  size_t startAge = page * GALLERY_PAGE_SIZE;
  size_t endAge = startAge + GALLERY_PAGE_SIZE; // exclusive; clamped against count below
  if (endAge > count) endAge = count;
  bool hasOlder = endAge < count;   // more snapshots exist past this page
  bool hasNewer = page > 0;         // a more-recent page exists

  String filterSuffix;
  if (sourceFilter.length() > 0) filterSuffix += " for \"" + htmlEscape(sourceFilter) + "\"";
  if (dateFilter.length() > 0) filterSuffix += " on " + formatDateForDisplay(dateFilter);
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
    String sourceLabel = htmlEscape(snapshotSourceLabel(entries[age].source));
    html += "<span style=\"display:inline-block;margin:4px;text-align:center;\">"
            "<a href=\"" + url + "\" target=\"_blank\">"
            "<img src=\"" + url + "\" style=\"display:block;max-width:160px;max-height:120px;\" "
            "alt=\"snapshot\"></a>"
            "<span class=\"hint\">" + sourceLabel + "</span></span>";
  }
  html += "</div>";

  if (hasNewer || hasOlder) {
    String base = baseUrl + sourceQueryParam + dateQueryParam + "&page=";
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
                           const String& dateFilter, std::vector<CameraConfig>* liveCameras,
                           std::vector<CameraState>* liveStates) {
  if (cameraFilter.length() == 0) return renderCameraPicker(liveCameras, liveStates);
  return renderCameraGrid(cameraFilter, page, sourceFilter, dateFilter, liveCameras, liveStates);
}
