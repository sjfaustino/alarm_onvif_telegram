#include "config_import_summary.h"

String summarizeImportResult(const ConfigImportApplyResult& r, ImportSummaryFormat format) {
  if (!r.anyDomainFound) {
    return "No valid configuration sections found in this file - nothing was changed. "
           "(Only files exported by this build or later can be restored - an older export "
           "has nothing for Import to read.)";
  }

  String imported, skipped;
  auto addImported = [&](const String& s) { if (imported.length() > 0) imported += ", "; imported += s; };
  if (r.camerasImported) addImported(String((unsigned)r.cameraCount) + " camera(s)");
  if (r.usersImported) addImported(String((unsigned)r.userCount) + " Telegram user(s)");
  if (r.networkImported) addImported("network settings");
  if (r.sdSettingsImported) addImported("SD settings");
  if (r.netWatchdogImported) addImported("Internet Watchdog settings");
  if (r.bridgeWatchdogImported) addImported("Camera Bridge Watchdog settings");
  if (r.powerMonitorImported) addImported("220V Power Monitor settings");

  auto addSkipped = [&](const String& s) { if (skipped.length() > 0) skipped += ", "; skipped += s; };
  if (!r.camerasImported && !r.camerasRejectedDuplicate) addSkipped("Cameras");
  if (!r.usersImported && !r.usersRejectedDuplicate) addSkipped("Telegram Users");
  if (!r.networkImported) addSkipped("Network");
  if (!r.sdSettingsImported) addSkipped("SD Settings");
  if (!r.netWatchdogImported) addSkipped("Internet Watchdog");
  if (!r.bridgeWatchdogImported) addSkipped("Camera Bridge Watchdog");
  if (!r.powerMonitorImported) addSkipped("220V Power Monitor");

  String banner = imported.length() > 0 ? ("Imported " + imported + ".") : "Nothing was imported.";
  if (skipped.length() > 0) {
    banner += " " + skipped + " not found in this file (or failed to save) - left unchanged.";
  }
  if (r.camerasRejectedDuplicate) {
    banner += " Cameras section REJECTED - two or more cameras in the file share the same name, "
              "which isn't allowed (every camera name must be unique). Fix the file and re-import; "
              "existing cameras were left unchanged.";
  }
  if (r.usersRejectedDuplicate) {
    banner += " Telegram Users section REJECTED - two or more users in the file share a name or a "
              "chat ID, which isn't allowed. Fix the file and re-import; existing Telegram users "
              "were left unchanged.";
  }
  if (r.networkImported) {
    // Stronger wording than the plain camera-password note below -
    // rebooting with a blank WiFi password (not just a broken camera)
    // strands the board off the network entirely, reachable only via
    // physical/serial access to fix.
    banner += " \xE2\x9A\xA0\xEF\xB8\x8F Network was imported WITHOUT a WiFi password (never "
              "included in an export) - go to the Network page and re-enter it now. Rebooting "
              "before fixing this will leave the board unable to reconnect to WiFi at all.";
  }
  if (r.camerasImported) {
    banner += " Imported camera(s) also have blank passwords - re-enter them on the Cameras "
              "page before rebooting.";
  }
  if (imported.length() > 0) {
    if (r.backupSaved) {
      banner += " A backup of what was stored just before this import was saved automatically - ";
      banner += format == ImportSummaryFormat::Html ? "<a href=\"/import/backup\">download it</a>"
                                                    : "download it from the dashboard's Security page";
      banner += " if you need to undo this.";
    } else {
      banner += " \xE2\x9A\xA0\xEF\xB8\x8F The automatic pre-import backup FAILED to save (NVS write "
                "error) - there is nothing to undo this with if it turns out wrong.";
    }
    banner += " Reboot the board (Maintenance page) to apply.";
  }
  return banner;
}
