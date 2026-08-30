#pragma once
#include <Arduino.h>

// Security panel content (dashboard login status + change form). Split
// out of webserver.cpp - see webserver_network.h's comment for why. The
// actual login-checking/updating (the auth middleware) stays in
// webserver.cpp, since it's tied to the PsychicHttpServer instance there.
String renderSecurityPanel();

// Plain-text dump of every camera, Telegram user, and network setting
// currently in NVS, for manual disaster recovery (see this panel's own
// "there's no recovery flow if [the login is] lost - forgetting it means
// erasing the board's NVS entirely" warning - this is the other half of
// that story: recovering everything ELSE that erasing NVS would also
// wipe). Deliberately excludes every password (camera and WiFi both) -
// re-enter those manually after a restore; everything else here is
// exactly what's tedious to reconstruct from memory. Served as a
// downloadable file by webserver.cpp's /export route.
//
// Each section above is followed by a machine-readable block (marked
// "### <SECTION> vN") that applyConfigImport() below reads back - see
// config_import_parse.h for the format itself.
String buildConfigExport();

// Result of applyConfigImport() - counts/flags for the /import route's
// banner (webserver.cpp). Each domain is independent: found-but-failed-to-
// save (an NVS write error) is distinct from not-found-in-the-file-at-all -
// the banner needs to tell those apart.
struct ConfigImportApplyResult {
  bool camerasImported = false;    size_t cameraCount = 0;
  bool usersImported = false;      size_t userCount = 0;
  bool networkImported = false;
  bool sdSettingsImported = false;
  bool anyDomainFound = false; // false means the file had no recognizable machine block at all
};

// Parses `text` (an uploaded export file's content) via parseConfigImport
// (config_import_parse.h) and, for each domain found, overwrites its
// entire persisted store (saveCameras/saveTelegramUsers/
// saveWifiCredentials/saveSdSettings) - wholesale replace, not a merge. A
// domain absent from the file is left completely untouched. Imported
// cameras/network always have blank passwords (never in an export) -
// callers must say so in the banner. Takes effect after a reboot, same as
// any other bulk camera/network change - this never live-applies.
ConfigImportApplyResult applyConfigImport(const String& text);
