#pragma once
#include <Arduino.h>
#include "config_import_summary.h" // ConfigImportApplyResult

// Config export/import/backup, shared by the dashboard (webserver.cpp's
// /export, /import, /import/backup routes) and Telegram's /backup and
// /restore commands (telegram_commands.cpp). No HTML here - see
// summarizeImportResult for turning an import result into text.

// Plain-text dump of every camera, Telegram user, and network setting
// currently in NVS, for manual disaster recovery (see the Security panel's
// "there's no recovery flow if [the login is] lost - forgetting it means
// erasing the board's NVS entirely" warning - this is the other half of
// that story: recovering everything ELSE that erasing NVS would also
// wipe). Deliberately excludes every password (camera and WiFi both) -
// re-enter those manually after a restore; everything else here is
// exactly what's tedious to reconstruct from memory.
//
// Each section above is followed by a machine-readable block (marked
// "### <SECTION> vN") that applyConfigImport() below reads back - see
// config_import_parse.h for the format itself.
String buildConfigExport();

// Parses `text` (an uploaded export file's content) via parseConfigImport
// (config_import_parse.h) and, for each domain found, overwrites its
// entire persisted store (saveCameras/saveTelegramUsers/
// saveWifiCredentials/saveSdSettings) - wholesale replace, not a merge. A
// domain absent from the file is left completely untouched. Imported
// cameras/network always have blank passwords (never in an export) -
// callers must say so in the banner. Takes effect after a reboot, same as
// any other bulk camera/network change - this never live-applies.
//
// Before touching anything, snapshots the CURRENT config (buildConfigExport())
// into a one-slot NVS backup (overwriting any previous one) - saveCameras()/
// saveTelegramUsers()/saveWifiCredentials()/saveSdSettings() all write NVS
// immediately, not staged for a reboot, so by the time this function
// returns the OLD config is already gone from NVS even though the board
// keeps running on it live until an actual reboot. Without this, importing
// the wrong file and then rebooting (ignoring every warning above) would
// be permanently unrecoverable rather than "download the backup and
// re-import it." See loadConfigBackup() below for retrieving it.
ConfigImportApplyResult applyConfigImport(const String& text);

// The most recent pre-import backup applyConfigImport() saved, in the
// exact same format buildConfigExport() produces (so it can be fed
// straight back into Import to undo) - "" if none has ever been saved
// this way. Served as a download by webserver.cpp's /import/backup route.
String loadConfigBackup();
