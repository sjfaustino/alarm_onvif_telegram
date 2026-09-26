#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <stddef.h>

// Result of applyConfigImport() (config_backup.h). Each domain is
// independent: found-but-failed-to-save (an NVS write error) is distinct
// from not-found-in-the-file-at-all - the summary needs to tell those apart.
struct ConfigImportApplyResult {
  bool camerasImported = false;    size_t cameraCount = 0;
  bool usersImported = false;      size_t userCount = 0;
  bool networkImported = false;
  bool sdSettingsImported = false;
  bool netWatchdogImported = false;
  bool bridgeWatchdogImported = false;
  bool powerMonitorImported = false;
  bool anyDomainFound = false; // false means the file had no recognizable machine block at all
  // Section present but REJECTED outright (two entries shared a name, or
  // for users a chat ID - config_import_parse.h), not merely absent.
  bool camerasRejectedDuplicate = false;
  bool usersRejectedDuplicate = false;
  // False means the pre-import snapshot hit an NVS write error, so there is
  // nothing to undo with - the summary must warn about that.
  bool backupSaved = false;
};

enum class ImportSummaryFormat {
  Html,      // dashboard banner - links to /import/backup
  PlainText, // Telegram /restore reply - points at the Security page instead
};

// Human-readable summary of an import: what was imported, what was skipped
// or rejected, and the password/reboot/backup warnings. The two formats
// differ only in how they point at the pre-import backup; neither contains
// any user-supplied text.
String summarizeImportResult(const ConfigImportApplyResult& r, ImportSummaryFormat format);
