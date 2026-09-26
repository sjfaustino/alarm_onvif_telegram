#pragma once
#include <Arduino.h>
#include "config_import_summary.h" // ConfigImportApplyResult

// Config export/import, shared by the dashboard and Telegram /backup and
// /restore.

// Text dump of every camera, Telegram user and network setting, for disaster
// recovery (e.g. after erasing NVS to reset a lost login). The readable
// sections omit passwords; each is followed by a "### <SECTION> vN"
// machine-readable block (config_import_parse.h). WiFi passwords are never
// exported; the camera block does carry camera passwords.
String buildConfigExport();

// Parses an export and replaces each domain found wholesale (absent domains
// are untouched). Saves apply to NVS at once but only take effect after a
// reboot.
//
// First saves the current config as a one-slot backup (loadConfigBackup), so
// importing the wrong file and rebooting is recoverable.
ConfigImportApplyResult applyConfigImport(const String& text);

// Last pre-import backup, in export format (re-import to undo); "" if none.
String loadConfigBackup();
